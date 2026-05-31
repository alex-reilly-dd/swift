// RUN: %target-run-simple-swift | %FileCheck %s
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
