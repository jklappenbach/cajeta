//
// L2-2 lambda tests — captures.
// Lambdas can now reference outer-scope primitives; each captured value is
// copied into a captures struct at the lambda creation site and unpacked
// inside the synthesized function via the implicit captures-ptr first arg.
// Capture-by-value semantics: subsequent mutation of the outer local does
// not affect what the lambda sees. See cajeta-docs/Lambdas.md.
//
// Out of scope (later L2 sub-slices):
//   - Heap captures by borrow
//   - `#name` transfer in captures
//   - Compile-error on writes to value-captured primitives
//   - Block-body lambdas
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Single primitive capture. The lambda body adds its param to a captured
// local; we exercise it after the outer local was set.
TEST(LambdaL2Tests, capturesPrimitiveByValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 add = x -> x + base;\n"
        "        return add(5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Two captured primitives, ordered to verify the captures-struct field
// layout matches between the populate site (outer code) and the unpack
// site (lambda body).
TEST(LambdaL2Tests, capturesTwoPrimitives) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = 7;\n"
        "        int32 b = 3;\n"
        "        (int32) -> int32 fn = x -> x * a + b;\n"
        "        return fn(4);\n"  // 4*7 + 3 = 31
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// Capture-by-VALUE means the lambda sees the value at the moment of
// capture; later mutation of the outer local doesn't change what the
// lambda reads. Mirrors the spec example.
TEST(LambdaL2Tests, captureSnapshotsAtCreationTime) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        (int32) -> int32 add = x -> x + base;\n"
        "        base = 999;\n"
        "        return add(5);\n"  // captured base=10, not 999 → 15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Two lambdas in the same scope, each capturing a different local. Verifies
// captures structs are scoped per-lambda (not shared) and the call sites
// dispatch through the right closure record.
TEST(LambdaL2Tests, multipleLambdasCaptureIndependently) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 left = 100;\n"
        "        int32 right = 7;\n"
        "        (int32) -> int32 addLeft = x -> x + left;\n"
        "        (int32) -> int32 mulRight = x -> x * right;\n"
        "        return addLeft(1) + mulRight(2);\n"  // 101 + 14 = 115
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 115);
}

// Param shadows an outer name with the same identifier. The captured
// binding shouldn't be created in the first place — the body's `base`
// resolves to the lambda parameter.
TEST(LambdaL2Tests, paramShadowsOuterName) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 999;\n"
        "        (int32) -> int32 fn = base -> base + 1;\n"
        "        return fn(4);\n"  // resolves to param, not outer
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
