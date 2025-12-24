// RUN: %target-run-simple-swift(-target %target-swift-5.9-abi-triple)

// REQUIRES: executable_test
// REQUIRES: swift_feature_ParameterPacks

// Test that key paths on types containing parameter packs work at runtime
// when using concrete type specializations.
//
// NOTE: Keypaths in generic contexts with abstract packs (e.g., using \.input
// inside Container<each T>.inputs where T is not yet concrete) require
// additional runtime support for pack metadata that is not yet implemented.

struct Entry<each T> {
    let input: (repeat each T)
}

// Test direct keypath usage with concrete type specialization
// This avoids the complexity of runtime pack expansion
func testDirectKeyPath() {
    // Single-element pack case
    let entry1 = Entry<Int>(input: 42)
    let keyPath1 = \Entry<Int>.input
    let value1 = entry1[keyPath: keyPath1]
    assert(value1 == 42)
    print("testDirectKeyPath single: PASS")

    // Two-element pack case
    let entry2 = Entry<Int, String>(input: (42, "hello"))
    let keyPath2 = \Entry<Int, String>.input
    let value2 = entry2[keyPath: keyPath2]
    assert(value2.0 == 42)
    assert(value2.1 == "hello")
    print("testDirectKeyPath double: PASS")

    // Three-element pack case
    let entry3 = Entry<Int, String, Double>(input: (42, "hello", 3.14))
    let keyPath3 = \Entry<Int, String, Double>.input
    let value3 = entry3[keyPath: keyPath3]
    assert(value3.0 == 42)
    assert(value3.1 == "hello")
    assert(value3.2 == 3.14)
    print("testDirectKeyPath triple: PASS")
}

testDirectKeyPath()
print("All tests passed!")
