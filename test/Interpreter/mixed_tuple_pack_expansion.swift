// RUN: %target-run-simple-swift | %FileCheck %s
// RUN: %target-run-simple-swift(-O) | %FileCheck %s
// REQUIRES: executable_test

// Execution milestone for mixed-tuple pack expansion (SE-0399 generalized to a
// pack-expansion element alongside concrete elements). Verifies the pack
// element of a mixed tuple is read out correctly at runtime and forwarded to a
// variadic-generic callee, across empty / single / multi / heterogeneous packs.

func describe<each U>(_ values: repeat each U) -> [String]
    where repeat each U: CustomStringConvertible {
  var result: [String] = []
  for v in repeat each values {
    result.append(v.description)
  }
  return result
}

// Peel the concrete head, forward the pack tail.
func tailDescribe<each U>(_ bar: (String, repeat each U))
    where repeat each U: CustomStringConvertible {
  let joined = describe(repeat each bar.1).joined(separator: ",")
  print("\(bar.0): \(joined)")
}

tailDescribe(("nums", 1, 2, 3))
// CHECK: nums: 1,2,3

tailDescribe(("empty"))
// CHECK: empty:{{ ?$}}

tailDescribe(("hetero", "a", 42, true))
// CHECK: hetero: a,42,true

// Reconstruct the tail tuple and read its concrete instantiation back.
func roundTrip<each U>(_ bar: (Int, repeat each U)) -> (repeat each U) {
  return (repeat each bar.1)
}

let rt: (String, Int) = roundTrip((0, "x", 7))
print(rt.0, rt.1)
// CHECK: x 7

// Passing a mixed tuple (concrete head + pack tail) where the callee's parameter
// is a *single* pack-expansion tuple `(repeat each U)` materializes the whole
// tuple — including the concrete head — into the callee's value pack. The head
// occupies a fixed slot in that pack; SILGen (emitExpandedPackExpansionTuple)
// used to skip non-expansion components, leaving that slot uninitialized so the
// callee read a garbage element (crashing at -Onone). All elements, head first,
// must be observed.
func consumeAll<each U>(_ values: (repeat each U)) -> [String]
    where repeat each U: CustomStringConvertible {
  var result: [String] = []
  for v in repeat each values {
    result.append(v.description)
  }
  return result
}

func prependThenConsume<each U>(_ tail: repeat each U) -> [String]
    where repeat each U: CustomStringConvertible {
  return consumeAll((99, repeat each tail))
}

print(prependThenConsume("a", "b").joined(separator: ","))
// CHECK: 99,a,b

print(prependThenConsume(1, 2, 3).joined(separator: ","))
// CHECK: 99,1,2,3

print(prependThenConsume().joined(separator: ","))
// CHECK: 99{{ ?$}}

// Forming an *lvalue* to a concrete (non-expansion) element of a mixed tuple —
// to mutate it in place — must project through a structural pack index, because
// the tuple's layout is dynamic. SILGen (TupleElementComponent::project) used to
// emit a fixed-offset `tuple_element_addr`, which asserts ("tuples with pack
// expansions must be indexed with tuple_pack_element_addr") and, in a no-asserts
// build, addresses the element against the wrong (static) layout. Both a
// read-modify-write through a sub-element and a whole-element store must work.
func mutateHead<each U>(_ tail: repeat each U) -> [UInt8]
    where repeat each U: CustomStringConvertible {
  var t: ([UInt8], repeat each U) = ([], repeat each tail)
  t.0.append(1)        // read-modify-write of a sub-element via its address
  t.0.append(2)
  t.0 = t.0 + [3]      // whole-element store
  return t.0
}

print(mutateHead("a", "b").map(String.init).joined(separator: ","))
// CHECK: 1,2,3

print(mutateHead().map(String.init).joined(separator: ","))
// CHECK: 1,2,3

print(mutateHead(1, true, "z").map(String.init).joined(separator: ","))
// CHECK: 1,2,3
