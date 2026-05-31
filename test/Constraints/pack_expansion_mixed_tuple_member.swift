// RUN: %target-typecheck-verify-swift

// Boundaries of mixed-tuple pack-element access (the positive cases live in
// mixed_tuple_pack_expansion.swift). Accessing the unique unlabeled
// pack-expansion element of a tuple via `.N` materializes a pack that must be
// used inside a `repeat each` expansion; the cases below are still rejected.

func consume<each T>(_ values: repeat each T) {}

// A materialized pack must appear inside an expansion, not be used as a scalar.
func bareWithoutExpansion<each U>(_ bar: ([UInt8], repeat each U)) {
  consume(bar.1)
  // expected-error@-1 {{pack reference 'each U' can only appear in pack expansion}}
}

// `repeat each` over the WHOLE mixed tuple is not the same as expanding its
// pack element, and remains an error.
func wholeTupleExpansion<each U>(_ bar: ([UInt8], repeat each U)) {
  consume(repeat each bar)
  // expected-error@-1 {{'each' cannot be applied to non-pack type '([UInt8], repeat each U)'}}
}

// A tuple with more than one pack-expansion element has no unique pack element
// to materialize, so positional access is not supported.
func multiplePackExpansions<each U, each V>(_ bar: (repeat each U, repeat each V)) {
  consume(repeat each bar.0)
  // expected-error@-1 {{value pack expansion can only appear inside a function argument list, tuple element, or as the expression of a for-in loop}}
}
