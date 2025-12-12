// RUN: %target-swift-emit-silgen -target %target-swift-5.9-abi-triple %s

// Test that passing a pack expansion tuple to a throwing closure compiles
// without crashing. This tests the fix for a bug where SILGen would crash
// when attempting to expand a tuple containing pack expansions (e.g.,
// `(repeat each T)`) into pack arguments, because the element count is
// only known at runtime in a generic context.

// Test case 1: Direct parameter passing
struct S<each Input> {
    func run(_ input: (repeat each Input), test: ((repeat each Input)) throws -> Void) throws {
        try test(input)
    }
}

// Test case 2: Iterator pattern (from PropertyTestingKit)
// This tests the case where a pack expansion tuple is obtained from iterating
// over an array and then passed to a closure.
struct FuzzEngine<each Input> {
    func run(inputs: [(repeat each Input)], test: ((repeat each Input)) throws -> Void) throws {
        for input in inputs {
            try test(input)
        }
    }
}
