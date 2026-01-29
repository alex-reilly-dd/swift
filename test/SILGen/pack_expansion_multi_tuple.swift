// RUN: %target-swift-emit-silgen %s | %FileCheck %s

// Test that tuples containing multiple pack expansions can be passed to
// functions expecting a single pack expansion tuple parameter.
// This tests the fix for the SILGen assertion "only single pack expansion
// tuples are currently supported" when combining two packs into one tuple.

protocol P {}
struct A: P {}

struct Holder<each T: P> {
    let values: (repeat each T)
}

// CHECK-LABEL: sil hidden [ossa] @$s26pack_expansion_multi_tuple4test_1yyx_txxQp_tq__tq_Qp_tRvzRv_AA1PRzAaER_r0_lF
func test<each X: P, each Y: P>(_ x: (repeat each X), _ y: (repeat each Y)) {
    // This creates a tuple (repeat each X, repeat each Y) with two pack expansions
    // and passes it to Holder which expects (repeat each T)
    let h = Holder(values: (repeat each x, repeat each y))
    print(h)
}

func callTest() {
    test((A(),), (A(),))
}
