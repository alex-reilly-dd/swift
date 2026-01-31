//===--- LoopUnroll.cpp - Loop unrolling ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-loopunroll"

#include "llvm/ADT/DepthFirstIterator.h"

#include "swift/Basic/Assertions.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/Analysis/IsSelfRecursiveAnalysis.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/LoopUtils.h"
#include "swift/SILOptimizer/Utils/PerformanceInlinerUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"

using namespace swift;
using namespace swift::PatternMatch;

using llvm::DenseMap;
using llvm::MapVector;

/// Check if the loop header contains a dynamic_pack_index instruction,
/// indicating this is a pack iteration loop.
static DynamicPackIndexInst *getPackIndexInst(SILLoop *Loop) {
  auto *Header = Loop->getHeader();
  if (!Header)
    return nullptr;

  for (auto &Inst : *Header) {
    if (auto *DPI = dyn_cast<DynamicPackIndexInst>(&Inst))
      return DPI;
  }
  return nullptr;
}

/// Try to get the trip count for a pack iteration loop.
/// Returns the trip count if the pack type is concrete (no pack expansions).
static std::optional<uint64_t> getPackLoopTripCount(SILLoop *Loop) {
  auto *DPI = getPackIndexInst(Loop);
  if (!DPI)
    return std::nullopt;

  auto PackType = DPI->getIndexedPackType();
  if (PackType->containsPackExpansionType())
    return std::nullopt;

  uint64_t TripCount = PackType->getNumElements();
  if (TripCount == 0 || TripCount > 32)
    return std::nullopt;

  LLVM_DEBUG(llvm::dbgs() << "  Pack loop detected with concrete trip count: "
                          << TripCount << "\n");
  return TripCount;
}

namespace {

/// Clone the basic blocks in a loop.
///
/// Currently invalidates the DomTree.
class LoopCloner : public SILCloner<LoopCloner> {
  SILLoop *Loop;

  friend class SILInstructionVisitor<LoopCloner>;
  friend class SILCloner<LoopCloner>;

public:
  LoopCloner(SILLoop *Loop)
      : SILCloner<LoopCloner>(*Loop->getHeader()->getParent()), Loop(Loop) {}

  /// Clone the basic blocks in the loop.
  void cloneLoop();

  void sinkAddressProjections();

  // Update SSA helper.
  void collectLoopLiveOutValues(
      DenseMap<SILValue, SmallVector<SILValue, 8>> &LoopLiveOutValues);

protected:
  // SILCloner CRTP override.
  SILValue getMappedValue(SILValue V) {
    if (auto *BB = V->getParentBlock()) {
      if (!Loop->contains(BB))
        return V;
    }
    return SILCloner<LoopCloner>::getMappedValue(V);
  }
  // SILCloner CRTP override.
  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    SILCloner<LoopCloner>::postProcess(Orig, Cloned);
  }
};

} // end anonymous namespace

void LoopCloner::sinkAddressProjections() {
  SinkAddressProjections sinkProj;
  for (auto *bb : Loop->getBlocks()) {
    for (auto &inst : *bb) {
      for (auto res : inst.getResults()) {
        if (!res->getType().isAddress()) {
          continue;
        }
        for (auto use : res->getUses()) {
          auto *user = use->getUser();
          if (Loop->contains(user)) {
            continue;
          }
          bool canSink = sinkProj.analyzeAddressProjections(&inst);
          assert(canSink);
          sinkProj.cloneProjections();
        }
      }
    }
  }
}

void LoopCloner::cloneLoop() {
  SmallVector<SILBasicBlock *, 16> ExitBlocks;
  Loop->getExitBlocks(ExitBlocks);

  sinkAddressProjections();
  // Clone the entire loop.
  cloneReachableBlocks(Loop->getHeader(), ExitBlocks,
                       /*insertAfter*/Loop->getLoopLatch());
}

/// Determine the number of iterations the loop is at most executed. The loop
/// might contain early exits so this is the maximum if no early exits are
/// taken.
static std::optional<uint64_t> getMaxLoopTripCount(SILLoop *Loop,
                                                   SILBasicBlock *Preheader,
                                                   SILBasicBlock *Header,
                                                   SILBasicBlock *Latch) {

  // Skip a split backedge.
  SILBasicBlock *OrigLatch = Latch;
  if (!Loop->isLoopExiting(Latch) &&
      !(Latch = Latch->getSinglePredecessorBlock())) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: no single predecessor for non-exiting latch\n");
    return std::nullopt;
  }
  if (!Loop->isLoopExiting(Latch)) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: latch is not loop-exiting\n");
    return std::nullopt;
  }

 // Get the loop exit condition.
  auto *CondBr = dyn_cast<CondBranchInst>(Latch->getTerminator());
  if (!CondBr) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: no CondBranchInst terminator\n");
    return std::nullopt;
  }

  // Match an add 1 recurrence.

  auto *Cmp = dyn_cast<BuiltinInst>(CondBr->getCondition());
  if (!Cmp) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: condition is not a BuiltinInst\n");
    return std::nullopt;
  }

  unsigned Adjust = 0;
  SILBasicBlock *Exit = CondBr->getTrueBB();

  switch (Cmp->getBuiltinInfo().ID) {
    case BuiltinValueKind::ICMP_EQ:
    case BuiltinValueKind::ICMP_SGE:
      break;
    case BuiltinValueKind::ICMP_SGT:
      Adjust = 1;
      break;
    case BuiltinValueKind::ICMP_SLE:
      Exit = CondBr->getFalseBB();
      Adjust = 1;
      break;
    case BuiltinValueKind::ICMP_NE:
    case BuiltinValueKind::ICMP_SLT:
      Exit = CondBr->getFalseBB();
      break;
    default:
      LLVM_DEBUG(llvm::dbgs() << "  Trip count: unrecognized comparison builtin: "
                              << getBuiltinName(Cmp->getBuiltinInfo().ID) << "\n");
      return std::nullopt;
  }

  if (Loop->contains(Exit)) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: exit block is inside loop\n");
    return std::nullopt;
  }

  auto *End = dyn_cast<IntegerLiteralInst>(Cmp->getArguments()[1]);
  if (!End) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: bound is not an IntegerLiteralInst: "
                            << *Cmp->getArguments()[1]);
    return std::nullopt;
  }

  SILValue RecNext = Cmp->getArguments()[0];
  SILPhiArgument *RecArg;

  // Match signed add with overflow, unsigned add with overflow and
  // add without overflow.
  if (!match(RecNext, m_TupleExtractOperation(
                          m_ApplyInst(BuiltinValueKind::SAddOver,
                                      m_SILPhiArgument(RecArg), m_One()),
                          0)) &&
      !match(RecNext, m_TupleExtractOperation(
                          m_ApplyInst(BuiltinValueKind::UAddOver,
                                      m_SILPhiArgument(RecArg), m_One()),
                          0)) &&
      !match(RecNext, m_ApplyInst(BuiltinValueKind::Add,
                                      m_SILPhiArgument(RecArg), m_One()))) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: increment pattern not matched: " << *RecNext);
    return std::nullopt;
  }

  if (RecArg->getParent() != Header) {
    LLVM_DEBUG(llvm::dbgs() << "  Trip count: phi arg not in header\n");
    return std::nullopt;
  }

  auto *Start = dyn_cast_or_null<IntegerLiteralInst>(
      RecArg->getIncomingPhiValue(Preheader));
  if (!Start)
    return std::nullopt;

  if (RecNext != RecArg->getIncomingPhiValue(OrigLatch))
    return std::nullopt;

  auto StartVal = Start->getValue();
  auto EndVal = End->getValue();
  if (StartVal.sgt(EndVal))
    return std::nullopt;

  auto Dist = EndVal - StartVal;
  if (Dist.getBitWidth() > 64)
    return std::nullopt;

  if (Dist == 0)
    return std::nullopt;

  return Dist.getZExtValue() + Adjust;
}

/// Check whether we can duplicate the instructions in the loop and use a
/// heuristic that looks at the trip count and the cost of the instructions in
/// the loop to determine whether we should unroll this loop.
static bool canAndShouldUnrollLoop(SILLoop *Loop, uint64_t TripCount,
                                   IsSelfRecursiveAnalysis *SRA,
                                   DeadEndBlocks *deb) {
  assert(Loop->getSubLoops().empty() && "Expect innermost loops");
  if (TripCount > 32)
    return false;

  // We can unroll a loop if we can duplicate the instructions it holds.
  uint64_t Cost = 0;
  LLVM_DEBUG(llvm::dbgs() << "Checking canAndShouldUnrollLoop with TripCount="
                          << TripCount << "\n");
  // Average number of instructions per basic block.
  // It is used to estimate the cost of the callee
  // inside a loop.
  const uint64_t InsnsPerBB = 4;
  // Use command-line threshold for unrolling.
  const uint64_t SILLoopUnrollThreshold = Loop->getBlocks().empty() ? 0 : 
    (Loop->getBlocks())[0]->getParent()->getModule().getOptions().UnrollThreshold;
  for (auto *BB : Loop->getBlocks()) {
    for (auto &Inst : *BB) {
      if (!canDuplicateLoopInstruction(Loop, &Inst, deb)) {
        LLVM_DEBUG(llvm::dbgs() << "Cannot duplicate instruction: " << Inst << "\n");
        return false;
      }
      if (instructionInlineCost(Inst) != InlineCost::Free)
        ++Cost;
      if (auto AI = FullApplySite::isa(&Inst)) {
        auto Callee = AI.getCalleeFunction();
        if (Callee && getEligibleFunction(AI, InlineSelection::Everything, SRA)) {
          // If callee is rather big and potentially inlinable, it may be better
          // not to unroll, so that the body of the callee can be inlined later.
          Cost += Callee->size() * InsnsPerBB;
        }
      }
      if (Cost * TripCount > SILLoopUnrollThreshold) {
        LLVM_DEBUG(llvm::dbgs() << "Cost " << Cost << " * TripCount " << TripCount
                                << " = " << (Cost * TripCount) << " > threshold "
                                << SILLoopUnrollThreshold << "\n");
        return false;
      }
  }
  }
  LLVM_DEBUG(llvm::dbgs() << "Final cost " << Cost << " * TripCount " << TripCount
                          << " = " << (Cost * TripCount) << " <= threshold "
                          << SILLoopUnrollThreshold << "\n");
  return true;
}

/// Redirect the terminator of the current loop iteration's latch to the next
/// iterations header or if this is the last iteration remove the backedge to
/// the header.
static void redirectTerminator(SILBasicBlock *Latch, unsigned CurLoopIter,
                               unsigned LastLoopIter, SILBasicBlock *CurrentHeader,
                               SILBasicBlock *NextIterationsHeader) {

  auto *CurrentTerminator = Latch->getTerminator();

  // We can either have a split backedge as our latch terminator.
  //   HeaderBlock:
  //     ...
  //     cond_br %cond, ExitBlock, BackedgeBlock
  //
  //   BackedgeBlock:
  //     br HeaderBlock:
  //
  // Or a conditional branch back to the header.
  //   HeaderBlock:
  //     ...
  //     cond_br %cond, ExitBlock, HeaderBlock
  //
  // Redirect the HeaderBlock target to the unrolled successor. In the
  // unrolled block of the last iteration unconditionally jump to the
  // ExitBlock instead.

  // Handle the split backedge case.
  if (auto *Br = dyn_cast<BranchInst>(CurrentTerminator)) {
    // On the last iteration change the conditional exit to an unconditional
    // one.
    if (CurLoopIter == LastLoopIter) {
      auto *CondBr = cast<CondBranchInst>(
          Latch->getSinglePredecessorBlock()->getTerminator());
      if (CondBr->getTrueBB() != Latch)
        SILBuilderWithScope(CondBr).createBranch(
            CondBr->getLoc(), CondBr->getTrueBB(), CondBr->getTrueArgs());
      else
        SILBuilderWithScope(CondBr).createBranch(
            CondBr->getLoc(), CondBr->getFalseBB(), CondBr->getFalseArgs());
      CondBr->eraseFromParent();
      return;
    }

    // Otherwise, branch to the next iteration's header.
    SILBuilderWithScope(Br).createBranch(Br->getLoc(), NextIterationsHeader,
                                         Br->getArgs());
    Br->eraseFromParent();
    return;
  }

  // Otherwise, we have a conditional branch to the header.
  auto *CondBr = cast<CondBranchInst>(CurrentTerminator);
  // On the last iteration change the conditional exit to an unconditional
  // one.
  if (CurLoopIter == LastLoopIter) {
    if (CondBr->getTrueBB() == CurrentHeader) {
      SILBuilderWithScope(CondBr).createBranch(
          CondBr->getLoc(), CondBr->getFalseBB(), CondBr->getFalseArgs());
    } else {
      assert(CondBr->getFalseBB() == CurrentHeader);
      SILBuilderWithScope(CondBr).createBranch(
          CondBr->getLoc(), CondBr->getTrueBB(), CondBr->getTrueArgs());
    }
    CondBr->eraseFromParent();
    return;
  }

  // Otherwise, branch to the next iteration's header.
  if (CondBr->getTrueBB() == CurrentHeader) {
    SILBuilderWithScope(CondBr).createCondBranch(
        CondBr->getLoc(), CondBr->getCondition(), NextIterationsHeader,
        CondBr->getTrueArgs(), CondBr->getFalseBB(), CondBr->getFalseArgs());
  } else {
    assert(CondBr->getFalseBB() == CurrentHeader);
    SILBuilderWithScope(CondBr).createCondBranch(
        CondBr->getLoc(), CondBr->getCondition(), CondBr->getTrueBB(),
        CondBr->getTrueArgs(), NextIterationsHeader, CondBr->getFalseArgs());
  }
  CondBr->eraseFromParent();
}

/// Collect all the loop live out values in the map that maps original live out
/// value to live out value in the cloned loop.
void LoopCloner::collectLoopLiveOutValues(
    DenseMap<SILValue, SmallVector<SILValue, 8>> &LoopLiveOutValues) {
  for (auto *Block : Loop->getBlocks()) {
    // Look at block arguments.
    for (auto *Arg : Block->getArguments()) {
      for (auto *Op : Arg->getUses()) {
        // Is this use outside the loop?
        if (!Loop->contains(Op->getUser())) {
          auto ArgumentValue = SILValue(Arg);
          if (!LoopLiveOutValues.count(ArgumentValue))
            LoopLiveOutValues[ArgumentValue].push_back(
                getMappedValue(ArgumentValue));
        }
      }
    }
    // And the instructions.
    for (auto &Inst : *Block) {
      for (SILValue result : Inst.getResults()) {
        for (auto *Op : result->getUses()) {
          // Ignore uses inside the loop.
          if (Loop->contains(Op->getUser()))
            continue;

          auto UsedValue = Op->get();
          assert(UsedValue == result && "Instructions must match");

          if (!LoopLiveOutValues.count(UsedValue))
            LoopLiveOutValues[UsedValue].push_back(getMappedValue(result));
        }
      }
    }
  }
}

static void
updateSSA(SILFunction *Fn, SILLoop *Loop,
          DenseMap<SILValue, SmallVector<SILValue, 8>> &LoopLiveOutValues) {
  SILSSAUpdater SSAUp;
  for (auto &MapEntry : LoopLiveOutValues) {
    // Collect out of loop uses of this value.
    auto OrigValue = MapEntry.first;
    SmallVector<UseWrapper, 16> UseList;
    for (auto Use : OrigValue->getUses())
      if (!Loop->contains(Use->getUser()->getParent()))
        UseList.push_back(UseWrapper(Use));
    // Update SSA of use with the available values.
    SSAUp.initialize(Fn, OrigValue->getType(), OrigValue->getOwnershipKind());
    SSAUp.addAvailableValue(OrigValue->getParentBlock(), OrigValue);
    for (auto NewValue : MapEntry.second)
      SSAUp.addAvailableValue(NewValue->getParentBlock(), NewValue);
    for (auto U : UseList) {
      Operand *Use = U;
      SSAUp.rewriteUse(*Use);
    }
  }
}

/// Check whether we can unroll a pack iteration loop (allowing nested loops).
static bool canUnrollPackLoop(SILLoop *Loop, uint64_t TripCount,
                              IsSelfRecursiveAnalysis *SRA,
                              DeadEndBlocks *deb) {
  // Pack loops are typically small (few elements), so we use a more
  // aggressive threshold.
  if (TripCount > 16)
    return false;

  uint64_t Cost = 0;
  const uint64_t InsnsPerBB = 4;
  // Use a higher threshold for pack loops since they enable devirtualization.
  const uint64_t PackLoopUnrollThreshold = Loop->getBlocks().empty() ? 0 :
    (Loop->getBlocks())[0]->getParent()->getModule().getOptions().UnrollThreshold * 2;

  for (auto *BB : Loop->getBlocks()) {
    for (auto &Inst : *BB) {
      if (!canDuplicateLoopInstruction(Loop, &Inst, deb)) {
        LLVM_DEBUG(llvm::dbgs() << "Cannot duplicate pack loop instruction: " << Inst << "\n");
        return false;
      }
      if (instructionInlineCost(Inst) != InlineCost::Free)
        ++Cost;
      if (auto AI = FullApplySite::isa(&Inst)) {
        auto Callee = AI.getCalleeFunction();
        if (Callee && getEligibleFunction(AI, InlineSelection::Everything, SRA)) {
          Cost += Callee->size() * InsnsPerBB;
        }
      }
      if (Cost * TripCount > PackLoopUnrollThreshold) {
        LLVM_DEBUG(llvm::dbgs() << "Pack loop cost " << Cost << " * TripCount "
                                << TripCount << " = " << (Cost * TripCount)
                                << " > threshold " << PackLoopUnrollThreshold << "\n");
        return false;
      }
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "Pack loop final cost " << Cost << " * TripCount "
                          << TripCount << " = " << (Cost * TripCount)
                          << " <= threshold " << PackLoopUnrollThreshold << "\n");
  return true;
}

/// Try to fully unroll a pack iteration loop. Pack loops can have nested loops
/// (e.g., for iterating over arrays returned by protocol methods), so this
/// function handles non-innermost loops.
static bool tryToUnrollPackLoop(SILLoop *Loop, IsSelfRecursiveAnalysis *SRA,
                                DeadEndBlocks *deb) {
  // Get the pack loop trip count from the pack type.
  auto TripCount = getPackLoopTripCount(Loop);
  if (!TripCount) {
    return false;
  }

  LLVM_DEBUG(llvm::dbgs() << "Trying to unroll pack loop with trip count "
                          << *TripCount << ": \n" << *Loop);

  auto *Preheader = Loop->getLoopPreheader();
  if (!Preheader)
    return false;

  auto *Latch = Loop->getLoopLatch();
  if (!Latch)
    return false;

  auto *Header = Loop->getHeader();

  if (!canUnrollPackLoop(Loop, *TripCount, SRA, deb)) {
    LLVM_DEBUG(llvm::dbgs() << "Not unrolling pack loop, exceeds cost threshold\n");
    return false;
  }

  // Check for non-condbr exits (same as regular loop unrolling).
  SmallVector<SILBasicBlock *, 16> ExitingBlocks;
  Loop->getExitingBlocks(ExitingBlocks);
  for (auto &Exit : ExitingBlocks)
    if (!isa<CondBranchInst>(Exit->getTerminator()))
      return false;

  LLVM_DEBUG(llvm::dbgs() << "Unrolling pack loop in "
                          << Header->getParent()->getName()
                          << " " << *Loop << "\n");

  SmallVector<SILBasicBlock *, 16> Headers;
  Headers.push_back(Header);

  SmallVector<SILBasicBlock *, 16> Latches;
  Latches.push_back(Latch);

  DenseMap<SILValue, SmallVector<SILValue, 8>> LoopLiveOutValues;

  // Copy the body TripCount-1 times.
  for (uint64_t Cnt = 1; Cnt < *TripCount; ++Cnt) {
    LoopCloner cloner(Loop);
    cloner.cloneLoop();
    Headers.push_back(cloner.getOpBasicBlock(Header));
    Latches.push_back(cloner.getOpBasicBlock(Latch));

    if (Cnt == 1)
      cloner.collectLoopLiveOutValues(LoopLiveOutValues);
    else {
      for (auto &MapEntry : LoopLiveOutValues) {
        SILValue MappedValue = cloner.getOpValue(MapEntry.first);
        MapEntry.second.push_back(MappedValue);
        assert(MapEntry.second.size() == Cnt);
      }
    }
  }

  // Thread the loop clones.
  for (unsigned Iteration = 0, End = Latches.size(); Iteration != End;
       ++Iteration) {
    auto *CurrentLatch = Latches[Iteration];
    auto LastIteration = End - 1;
    auto *CurrentHeader = Headers[Iteration];
    auto *NextIterationsHeader =
        Iteration == LastIteration ? nullptr : Headers[Iteration + 1];
    redirectTerminator(CurrentLatch, Iteration, LastIteration, CurrentHeader,
                       NextIterationsHeader);
  }

  // Update SSA.
  updateSSA(Header->getParent(), Loop, LoopLiveOutValues);

  // Note: We don't replace dynamic_pack_index with scalar_pack_index here.
  // After unrolling, each iteration's index becomes a constant through
  // constant folding, and SILCombiner will handle the pack element
  // optimizations (converting dynamic_pack_index + constant to scalar forms
  // and devirtualizing witness_method calls).

  return true;
}

/// Try to fully unroll the loop if we can determine the trip count and the trip
/// count is below a threshold.
static bool tryToUnrollLoop(SILLoop *Loop, IsSelfRecursiveAnalysis *SRA, DeadEndBlocks *deb) {
  assert(Loop->getSubLoops().empty() && "Expecting innermost loops");

  LLVM_DEBUG(llvm::dbgs() << "Trying to unroll loop : \n" << *Loop);
  auto *Preheader = Loop->getLoopPreheader();
  if (!Preheader)
    return false;

  auto *Latch = Loop->getLoopLatch();
  if (!Latch)
    return false;

  auto *Header = Loop->getHeader();

  std::optional<uint64_t> MaxTripCount =
      getMaxLoopTripCount(Loop, Preheader, Header, Latch);
  if (!MaxTripCount) {
    LLVM_DEBUG(llvm::dbgs() << "Not unrolling, did not find trip count\n");
    return false;
  }

  if (!canAndShouldUnrollLoop(Loop, MaxTripCount.value(), SRA, deb)) {
    LLVM_DEBUG(llvm::dbgs() << "Not unrolling, exceeds cost threshold\n");
    return false;
  }

  // TODO: We need to split edges from non-condbr exits for the SSA updater. For
  // now just don't handle loops containing such exits.
  SmallVector<SILBasicBlock *, 16> ExitingBlocks;
  Loop->getExitingBlocks(ExitingBlocks);
  for (auto &Exit : ExitingBlocks)
    if (!isa<CondBranchInst>(Exit->getTerminator()))
      return false;

  LLVM_DEBUG(llvm::dbgs() << "Unrolling loop in "
                          << Header->getParent()->getName()
                          << " " << *Loop << "\n");

  SmallVector<SILBasicBlock *, 16> Headers;
  Headers.push_back(Header);

  SmallVector<SILBasicBlock *, 16> Latches;
  Latches.push_back(Latch);

  DenseMap<SILValue, SmallVector<SILValue, 8>> LoopLiveOutValues;

  // Copy the body MaxTripCount-1 times.
  for (uint64_t Cnt = 1; Cnt < *MaxTripCount; ++Cnt) {
    // Clone the blocks in the loop.
    LoopCloner cloner(Loop);
    cloner.cloneLoop();
    Headers.push_back(cloner.getOpBasicBlock(Header));
    Latches.push_back(cloner.getOpBasicBlock(Latch));

    // Collect values defined in the loop but used outside. On the first
    // iteration we populate the map from original loop to cloned loop. On
    // subsequent iterations we only need to update this map with the values
    // from the new iteration's clone.
    if (Cnt == 1)
      cloner.collectLoopLiveOutValues(LoopLiveOutValues);
    else {
      for (auto &MapEntry : LoopLiveOutValues) {
        // Look it up in the value map.
        SILValue MappedValue = cloner.getOpValue(MapEntry.first);
        MapEntry.second.push_back(MappedValue);
        assert(MapEntry.second.size() == Cnt);
      }
    }
  }

  // Thread the loop clones by redirecting the loop latches to the successor
  // iteration's header.
  for (unsigned Iteration = 0, End = Latches.size(); Iteration != End;
       ++Iteration) {
    auto *CurrentLatch = Latches[Iteration];
    auto LastIteration = End - 1;
    auto *CurrentHeader = Headers[Iteration];
    auto *NextIterationsHeader =
        Iteration == LastIteration ? nullptr : Headers[Iteration + 1];

    redirectTerminator(CurrentLatch, Iteration, LastIteration, CurrentHeader,
                       NextIterationsHeader);
  }

  // Fixup SSA form for loop values used outside the loop.
  updateSSA(Loop->getFunction(), Loop, LoopLiveOutValues);
  return true;
}

// =============================================================================
//                                 Driver
// =============================================================================

namespace {

class LoopUnrolling : public SILFunctionTransform {

  void run() override {
    bool Changed = false;
    auto *Fun = getFunction();
    SILLoopInfo *LoopInfo = PM->getAnalysis<SILLoopAnalysis>()->get(Fun);
    IsSelfRecursiveAnalysis *SRA = PM->getAnalysis<IsSelfRecursiveAnalysis>();
    DeadEndBlocks *deb = PM->getAnalysis<DeadEndBlocksAnalysis>()->get(Fun);

    LLVM_DEBUG(llvm::dbgs() << "Loop Unroll running on function : "
                            << Fun->getName() << "\n");

    // First, try to unroll pack iteration loops. These loops may have nested
    // loops (e.g., for iterating over arrays returned by protocol methods),
    // so we handle them specially before processing innermost loops.
    SmallVector<SILLoop *, 16> AllLoops;
    for (auto *Loop : *LoopInfo) {
      SmallVector<SILLoop *, 8> Worklist;
      Worklist.push_back(Loop);
      for (unsigned i = 0; i < Worklist.size(); ++i) {
        auto *L = Worklist[i];
        AllLoops.push_back(L);
        for (auto *SubLoop : *L)
          Worklist.push_back(SubLoop);
      }
    }

    // Try to unroll pack loops (can have nested loops).
    for (auto *Loop : AllLoops) {
      if (getPackIndexInst(Loop)) {
        if (tryToUnrollPackLoop(Loop, SRA, deb)) {
          Changed = true;
          // Pack loop unrolling invalidates loop info, so we need to
          // recompute it before continuing.
          invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
          LoopInfo = PM->getAnalysis<SILLoopAnalysis>()->get(Fun);
          break;  // Start over with fresh loop info.
        }
      }
    }

    // Collect innermost loops.
    SmallVector<SILLoop *, 16> InnermostLoops;

    for (auto *Loop : *LoopInfo) {
      SmallVector<SILLoop *, 8> Worklist;
      Worklist.push_back(Loop);

      for (unsigned i = 0; i < Worklist.size(); ++i) {
        auto *L = Worklist[i];
        for (auto *SubLoop : *L)
          Worklist.push_back(SubLoop);
        if (L->getSubLoops().empty())
          InnermostLoops.push_back(L);
      }
    }

    if (InnermostLoops.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "No innermost loops\n");
      return;
    }

    // Try to unroll innermost loops.
    for (auto *Loop : InnermostLoops)
      Changed |= tryToUnrollLoop(Loop, SRA, deb);

    if (Changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createLoopUnroll() {
  return new LoopUnrolling();
}
