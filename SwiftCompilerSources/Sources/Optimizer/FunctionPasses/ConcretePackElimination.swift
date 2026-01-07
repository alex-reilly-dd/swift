//===--- ConcretePackElimination.swift ------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2025 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// Eliminates pack allocations with concrete element types by replacing pack
/// element operations with direct uses of the stored addresses.
///
/// For address packs:
/// - pack_element_set stores an ADDRESS into the pack at an index
/// - pack_element_get retrieves that ADDRESS from the pack
///
/// This pass transforms:
/// ```
///   %pack = alloc_pack $Pack{Int, String}
///   %idx0 = scalar_pack_index 0 of $Pack{Int, String}
///   %idx1 = scalar_pack_index 1 of $Pack{Int, String}
///   pack_element_set %addr0 into %idx0 of %pack
///   pack_element_set %addr1 into %idx1 of %pack
///   %elt0 = pack_element_get %idx0 of %pack as $*Int
///   %elt1 = pack_element_get %idx1 of %pack as $*String
///   dealloc_pack %pack
/// ```
/// into:
/// ```
///   // All uses of %elt0 → use %addr0 directly
///   // All uses of %elt1 → use %addr1 directly
/// ```
///
/// This optimization is critical for parameter pack performance because
/// generic specialization creates specialized functions with concrete pack
/// types, but the internal pack operations remain. This pass eliminates
/// those operations when all indices are statically known.
///
let concretePackElimination = FunctionPass(name: "concrete-pack-elimination") {
  (function: Function, context: FunctionPassContext) in

  // Collect all alloc_pack and pack_length instructions first to avoid iterator invalidation
  var allocPacks: [AllocPackInst] = []
  var packLengths: [PackLengthInst] = []
  for block in function.blocks {
    for inst in block.instructions {
      if let allocPack = inst as? AllocPackInst {
        allocPacks.append(allocPack)
      } else if let packLength = inst as? PackLengthInst {
        packLengths.append(packLength)
      }
    }
  }

  for allocPack in allocPacks {
    tryEliminatePack(allocPack, context)
  }

  // Constant fold pack_length for concrete packs
  for packLength in packLengths {
    tryFoldPackLength(packLength, context)
  }
}

/// Attempts to eliminate an alloc_pack by replacing pack_element_get with
/// the addresses stored via pack_element_set.
private func tryEliminatePack(_ allocPack: AllocPackInst, _ context: FunctionPassContext) {
  let packType = allocPack.type

  // Only handle packs without pack expansions (all concrete element types)
  guard packType.isSILPack,
        !packType.containsSILPackExpansionType else {
    return
  }

  let elements = packType.packElements
  guard !elements.isEmpty else {
    return
  }

  // Build a map from index -> stored address
  // Also collect all pack_element_gets for replacement
  var storedAddresses: [Int: any Value] = [:]
  var packElementGets: [PackElementGetInst] = []
  var packElementSets: [PackElementSetInst] = []
  var deallocPack: DeallocPackInst? = nil
  var scalarIndicesToErase: [ScalarPackIndexInst] = []
  var debugValues: [DebugValueInst] = []

  for use in allocPack.uses {
    let user = use.instruction

    if let set = user as? PackElementSetInst {
      guard let scalarIdx = set.index as? ScalarPackIndexInst else {
        // Dynamic index - can't eliminate this pack
        return
      }
      let idx = scalarIdx.componentIndex
      // Record the address stored at this index
      storedAddresses[idx] = set.value
      packElementSets.append(set)
      // Track index for later cleanup if it's only used by this pack
      if isIndexOnlyUsedByPack(scalarIdx, pack: allocPack) {
        scalarIndicesToErase.append(scalarIdx)
      }
    } else if let get = user as? PackElementGetInst {
      guard let scalarIdx = get.index as? ScalarPackIndexInst else {
        // Dynamic index - can't eliminate this pack
        return
      }
      packElementGets.append(get)
      // Track index for later cleanup if it's only used by this pack
      if isIndexOnlyUsedByPack(scalarIdx, pack: allocPack) {
        scalarIndicesToErase.append(scalarIdx)
      }
    } else if let dealloc = user as? DeallocPackInst {
      deallocPack = dealloc
    } else if let debug = user as? DebugValueInst {
      // debug_value instructions can be safely erased
      debugValues.append(debug)
    } else {
      // Unknown use - can't eliminate this pack
      return
    }
  }

  guard let _ = deallocPack else {
    // No dealloc_pack found - something is wrong
    return
  }

  // Verify we have stored addresses for all indices that are being read
  for get in packElementGets {
    guard let scalarIdx = get.index as? ScalarPackIndexInst else {
      return
    }
    let idx = scalarIdx.componentIndex
    if storedAddresses[idx] == nil {
      // Reading from an index that wasn't set - can't eliminate
      return
    }
  }

  // Replace pack_element_get uses with the corresponding stored address
  for get in packElementGets {
    guard let scalarIdx = get.index as? ScalarPackIndexInst else {
      continue
    }
    let idx = scalarIdx.componentIndex
    guard let storedAddr = storedAddresses[idx] else {
      continue
    }

    // Replace all uses of the pack_element_get with the stored address
    get.uses.replaceAll(with: storedAddr, context)
    context.erase(instruction: get)
  }

  // Erase pack_element_sets (they're no longer needed)
  for set in packElementSets {
    context.erase(instruction: set)
  }

  // Erase debug_value instructions
  for debug in debugValues {
    context.erase(instruction: debug)
  }

  // Erase the dealloc_pack and alloc_pack
  if let dealloc = deallocPack {
    context.erase(instruction: dealloc)
  }
  context.erase(instruction: allocPack)

  // Clean up scalar_pack_index instructions that are now unused
  // Use a Set to avoid duplicate erasures
  var erasedIndices = Set<ObjectIdentifier>()
  for idx in scalarIndicesToErase {
    let id = ObjectIdentifier(idx)
    if !erasedIndices.contains(id) && idx.uses.isEmpty {
      erasedIndices.insert(id)
      context.erase(instruction: idx)
    }
  }
}

/// Checks if a scalar_pack_index is only used by pack operations on the given pack.
private func isIndexOnlyUsedByPack(_ index: ScalarPackIndexInst, pack: AllocPackInst) -> Bool {
  for use in index.uses {
    let user = use.instruction

    if let set = user as? PackElementSetInst {
      // Check if the pack operand is our pack
      guard set.pack === pack else {
        return false
      }
    } else if let get = user as? PackElementGetInst {
      // Check if the pack operand is our pack
      guard get.pack === pack else {
        return false
      }
    } else {
      // Unknown user - be conservative
      return false
    }
  }
  return true
}

/// Attempts to constant fold a pack_length instruction when the pack type
/// has no pack expansions (all concrete element types).
///
/// For example:
/// ```
///   %len = pack_length $Pack{Int, String, Double}
/// ```
/// becomes:
/// ```
///   %len = integer_literal $Builtin.Word, 3
/// ```
private func tryFoldPackLength(_ packLength: PackLengthInst, _ context: FunctionPassContext) {
  let packType = packLength.packType

  // Only handle packs without pack expansions (all concrete element types)
  // Use containsPackExpansionType which works for both AST PackType and SIL SILPackType
  guard !packType.containsPackExpansionType else {
    return
  }

  // Get the number of elements in the concrete pack
  let numElements = packType.numPackElements
  guard numElements > 0 else {
    return
  }

  // Create an integer literal with the pack length
  // The result type of pack_length is Builtin.Word
  let builder = Builder(before: packLength, context)
  let literal = builder.createIntegerLiteral(numElements, type: packLength.type)

  // Replace all uses of pack_length with the constant
  packLength.uses.replaceAll(with: literal, context)
  context.erase(instruction: packLength)
}
