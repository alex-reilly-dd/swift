// RUN: %target-typecheck-verify-swift

// Feature: generalize SE-0399 ("Tuple of Value Pack Expansion") to tuples that
// contain a pack-expansion element ALONGSIDE other elements (the proposal's
// Future Direction "Lift the single value pack with no label restriction").
//
// Accessing the pack-expansion element of such a tuple (e.g. `bar.1`) yields
// the value pack, usable inside a `repeat each` expansion. Concrete elements
// (e.g. the head `bar.0`) are accessed normally.
//
// This file is the Sema milestone: everything below must type-check with NO
// errors once the feature is implemented. (It currently fails: `repeat each
// bar.1` reports "value pack expansion can only appear ...".)

func consume<each T>(_ values: repeat each T) {}

// Forward the pack element of a mixed tuple into a variadic-generic call.
func forwardTail<each U>(_ bar: ([UInt8], repeat each U)) {
  consume(repeat each bar.1)
}

// Reconstruct the tail as a tuple.
func tailTuple<each U>(_ bar: ([UInt8], repeat each U)) -> (repeat each U) {
  return (repeat each bar.1)
}

// Peel the concrete head and forward the tail to a single-tuple-arg closure.
func peelAndCall<each U>(
  _ bar: ([UInt8], repeat each U),
  _ body: ((repeat each U)) -> Void
) {
  let bytes: [UInt8] = bar.0
  _ = bytes
  body((repeat each bar.1))
}

// The pack element may appear in a compound expansion pattern.
func mapTail<each U>(_ bar: (Int, repeat each U)) -> (repeat Optional<each U>) {
  return (repeat Optional(each bar.1))
}
