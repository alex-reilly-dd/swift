// RUN: %target-swift-frontend -O -emit-sil %s | %FileCheck %s

// REQUIRES: swift_in_compiler

protocol Plugin {
    func handle() -> Int
}

struct PluginA: Plugin {
    func handle() -> Int { 1 }
}

struct PluginB: Plugin {
    func handle() -> Int { 2 }
}

struct Processor<each P: Plugin> {
    let plugins: (repeat each P)

    func process() -> Int {
        var sum = 0
        for plugin in repeat each plugins {
            sum += plugin.handle()
        }
        return sum
    }
}

// Test that witness_method on pack elements is devirtualized.
// The optimization is so effective that everything gets constant-folded to 3.
// CHECK-LABEL: sil {{.*}}@$s{{.*}}test{{.*}}
// CHECK-NOT: witness_method
// CHECK: integer_literal $Builtin.Int64, 3
// CHECK: } // end sil function
@inline(never)
public func test() -> Int {
    let processor = Processor(plugins: (PluginA(), PluginB()))
    return processor.process()
}
