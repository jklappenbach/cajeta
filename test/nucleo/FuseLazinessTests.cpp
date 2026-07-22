//
// nucleo-expr Unit 1 — the laziness and fail-loud halves (plan 1.1.2, 1.1.4).
//
// 1.1.2: building a fused function runs NOTHING. Measured with
// Cajeta.liveCount() (the DropGapTests idiom), not asserted by inspection.
// 1.1.4: an op outside the fusible set is a named, located compile error —
// the transform-intrinsics diagnostic bar, reusing its error family.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int64_t runI64(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.lang.Cajeta;\n"
        "public final class T {\n"
        "    public static int64 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<int64_t (*)()>("run")();
}

} // namespace

// 1.1.2 — BUILDING allocates nothing. Constructing the fused function without
// calling it leaves the live-set population unchanged: laziness is observable,
// building is free.
TEST(FuseLaziness, buildingAllocatesNothing) {
    EXPECT_EQ(runI64(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        (Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) ->\n"
        "                Tensor.sub<float32>(Tensor.mul<float32>(t, t), t));\n"
        "        return Cajeta.liveCount() - base;"), 0);
}

// 1.1.2 (the other half) — CALLING it does allocate: exactly the result. This
// is the contrast that makes the zero above meaningful rather than vacuous.
TEST(FuseLaziness, callingAllocatesTheResult) {
    EXPECT_GT(runI64(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) ->\n"
        "                Tensor.sub<float32>(Tensor.mul<float32>(t, t), t));\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Cajeta.liveCount() - base;"), 0);
}

// 1.1.4, OVERTAKEN BY U2 and repurposed (2026-07-20, U5 sweep): this test
// originally asserted a reduction operand was a named error ("until U2
// stages it") — U2 landed staging but never updated the assertion (its
// targeted blast radius missed this file). The staged behavior is now the
// pin: `mul(t, sum(t))` hoists the reduction once and broadcasts it into the
// fused loop. x = {1,2}: sum = 3, r = {3,6}, sum(r) = 9.
TEST(FuseLaziness, reductionOperandStagesAndBroadcasts) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        float32[] fa = { 1.0f, 2.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 2;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) ->\n"
        "                Tensor.mul<float32>(t, Tensor.sum<float32,float32>(t)));\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 9.0f);
}

// 1.1.4 — a non-lambda target keeps the transform family's specialization
// error (the Fuse recognizer shares that gate).
TEST(FuseLaziness, nonSpecializableTargetIsNamedError) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    public static float32 viaParam((Tensor<float32>) -> #Tensor<float32> f) {\n"
        "        (Tensor<float32>) -> #Tensor<float32> g = Fuse(f);\n"
        "        return 0.0f;\n"
        "    }\n"
        "    public static float32 run() { return 0.0f; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.T");
        FAIL() << "expected a non-specializable Fuse target to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE");
    }
}
