// RUN: %target-swift-frontend -emit-sil -O %s | %FileCheck %s

// Test that pack operations are eliminated when packs have concrete element types.

@inline(never)
func packIdentity<each T>(_ x: repeat each T) -> (repeat each T) {
    return (repeat each x)
}

// CHECK-LABEL: sil {{.*}} @$s27concrete_pack_elimination4testSi_SStyF
// CHECK-NOT: alloc_pack
// CHECK-NOT: pack_element_set
// CHECK-NOT: pack_element_get
// CHECK-NOT: scalar_pack_index
// CHECK-NOT: dealloc_pack
// CHECK: } // end sil function
public func test() -> (Int, String) {
    return packIdentity(42, "hello")
}

// The specialized packIdentity should also be pack-free
// CHECK-LABEL: sil {{.*}}packIdentity{{.*}}Si_SSQP_Tg5Tf8xx_n
// CHECK-NOT: alloc_pack
// CHECK-NOT: pack_element_set
// CHECK-NOT: pack_element_get
// CHECK-NOT: dealloc_pack
// CHECK: } // end sil function
