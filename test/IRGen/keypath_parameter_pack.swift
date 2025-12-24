// RUN: %target-swift-frontend -emit-ir %s -verify -target %target-swift-5.9-abi-triple

// REQUIRES: swift_feature_ParameterPacks

// Key paths on types containing parameter packs are not yet supported.
// This test ensures we emit a diagnostic instead of crashing.

struct Entry<each T> {
    let input: (repeat each T)
}

struct Container<each T> {
    let entries: [Entry<repeat each T>]

    var inputs: [(repeat each T)] {
        entries.map(\.input) // expected-error {{unimplemented IR generation feature key paths with parameter packs}}
    }
}
