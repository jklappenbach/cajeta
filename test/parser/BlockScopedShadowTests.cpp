//
// Tests for block-scoped NAME bindings. There is one Scope per method, so a
// local declared inside a nested `{ ... }` is putField'd into the same map
// that holds the method's parameters. Without a save/restore at the closing
// brace the declaration rebinds that name for the REST of the method, and
// when the shadowed outer binding has a different type the compiler emits
// malformed IR instead of a diagnostic — e.g. a `float32[] v` parameter
// shadowed by a `float32 v` makes a later `v[i] = ...` index the scalar,
// producing a GEP/load whose base operand is a float. Archives are
// unverified bitcode, so that ships silently and only aborts at instruction
// selection in the eventual --emit=exe, in a function far from the cause.
// See specs/archive/codegen-block-scope-shadow-spec.md.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

double runF64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<double (*)()>("run")();
}

} // namespace

// The exact defect: an array parameter shadowed by a same-named scalar local
// declared inside an `if` block. After the block the name must resolve to the
// PARAMETER again, so the indexed store writes the array.
TEST(BlockScopedShadowTests, scalarLocalDoesNotCaptureArrayParamAfterBlock) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static float64 f(float32[] v, int64 n) {\n"
        "        if (n > (int64) 0) {\n"
        "            float32 v = (float32) 1.5;\n"
        "            if (v < (float32) 0.0) { return (float64) 0.0; }\n"
        "        }\n"
        "        v[0] = (float32) 2.5;\n"   // the parameter, not the dead scalar
        "        return (float64) v[0];\n"
        "    }\n"
        "    public static float64 run() {\n"
        "        float32[] a = heap float32[2];\n"
        "        return D.f(a, (int64) 1);\n"
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(2.5, runF64(src));
}

// The shadow is genuinely in effect INSIDE the block — restoring at the brace
// must not defeat the shadow while it is live.
TEST(BlockScopedShadowTests, shadowIsVisibleInsideItsOwnBlock) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static float64 f(float32[] v) {\n"
        "        v[0] = (float32) 7.0;\n"
        "        float64 seen = (float64) 0.0;\n"
        "        {\n"
        "            float32 v = (float32) 3.25;\n"
        "            seen = (float64) v;\n"          // the local, not the array
        "        }\n"
        "        return seen + (float64) v[0];\n"    // the parameter again
        "    }\n"
        "    public static float64 run() {\n"
        "        float32[] a = heap float32[2];\n"
        "        return D.f(a);\n"
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(10.25, runF64(src));
}

// Sibling blocks reusing one name each get their own binding; neither leaks
// into the other or past the second closing brace.
TEST(BlockScopedShadowTests, siblingBlocksDoNotLeakIntoEachOther) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        float64 t = (float64) 100.0;\n"
        "        float64 acc = (float64) 0.0;\n"
        "        { int64 t = (int64) 3; acc = acc + (float64) t; }\n"
        "        { int64 t = (int64) 4; acc = acc + (float64) t; }\n"
        "        return acc + t;\n"   // the outer float64, restored
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(107.0, runF64(src));
}

// Nesting composes: each block restores its own binding, innermost first.
TEST(BlockScopedShadowTests, nestedShadowsRestoreOutermostBinding) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        int64 x = (int64) 1;\n"
        "        float64 acc = (float64) 0.0;\n"
        "        {\n"
        "            float32 x = (float32) 2.0;\n"
        "            {\n"
        "                float64 x = (float64) 4.0;\n"
        "                acc = acc + x;\n"
        "            }\n"
        "            acc = acc + (float64) x;\n"   // back to the float32
        "        }\n"
        "        return acc + (float64) x;\n"      // back to the int64
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(7.0, runF64(src));
}

// A loop body is a block too: the per-iteration local must not capture the
// same-named array it indexes on the next statement.
TEST(BlockScopedShadowTests, loopBodyShadowDoesNotCaptureOuterArray) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        float32[] w = heap float32[3];\n"
        "        int64 i = (int64) 0;\n"
        "        while (i < (int64) 3) {\n"
        "            float32 w = (float32) i;\n"
        "            i = i + (int64) 1;\n"
        "        }\n"
        "        w[2] = (float32) 9.0;\n"
        "        return (float64) w[2];\n"
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(9.0, runF64(src));
}
