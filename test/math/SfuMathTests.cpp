//
// SfuMathTests — cajeta.math.sfu.SfuMath, the bit-exact CUDA SFU fast-math
// models promoted from dev.cajeta.xgboost.fastmath (where the capture-table
// methodology and the 4090 bit-parity proofs live). Without the RCP_TABLE /
// EX2_TABLE captures the class serves its documented placeholder semantics
// (correctly-rounded accurate math), which is what these tests pin: exact
// algebraic identities that hold under BOTH the placeholder and a real SFU
// table, plus fmaf's single-rounding guarantee checked against libm's fma
// over a large pseudo-random sweep. The hardware-table bit-parity itself is
// certified downstream (cajeta-xgboost's GpuSplitFinder suite on the 4090).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* SRC =
    "package test;\n"
    "import cajeta.math.sfu.SfuMath;\n"
    "public final class D {\n"
    "    public static float32 fdiv(float32 a, float32 b) {\n"
    "        return SfuMath.fdividef(a, b);\n"
    "    }\n"
    "    public static float32 fma3(float32 a, float32 b, float32 c) {\n"
    "        return SfuMath.fmaf(a, b, c);\n"
    "    }\n"
    "    public static float32 ex(float32 x) {\n"
    "        return SfuMath.expf(x);\n"
    "    }\n"
    "    public static int32 tables() {\n"
    "        int32 r = 0;\n"
    "        if (SfuMath.hasRcpTable()) { r = r + 1; }\n"
    "        if (SfuMath.hasEx2Table()) { r = r + 2; }\n"
    "        return r;\n"
    "    }\n"
    "}\n";

// The tables are env-var-loaded lazily once per process; clear both so the
// suite always exercises the documented no-capture placeholder path.
void clearCaptureEnv() {
    unsetenv("RCP_TABLE");
    unsetenv("EX2_TABLE");
}

float ulpDiff(float a, float b) {
    if (a == b) return 0.0f;
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    return (float) std::abs((double) ia - (double) ib);
}

} // namespace

TEST(SfuMathTests, noCaptureMeansNoTables) {
    clearCaptureEnv();
    auto jit = CajetaJit::compile(SRC, "test.D");
    auto tables = jit->lookup<int32_t (*)()>("tables");
    EXPECT_EQ(tables(), 0);
}

// rcp of a power of two is exact in every model (mantissa index 0 -> rcp(1.0)
// = 1.0 on silicon and in the placeholder), so division by a power of two is
// a pure exponent shift and must be EXACT — under the placeholder and under a
// real capture alike. Out-of-domain b (negative / zero / inf) falls back to
// accurate division by contract.
TEST(SfuMathTests, powerOfTwoDivisionsExact) {
    clearCaptureEnv();
    auto jit = CajetaJit::compile(SRC, "test.D");
    auto fdiv = jit->lookup<float (*)(float, float)>("fdiv");
    EXPECT_EQ(fdiv(6.0f, 2.0f), 3.0f);
    EXPECT_EQ(fdiv(7.0f, 0.25f), 28.0f);
    EXPECT_EQ(fdiv(-3.0f, 8.0f), -0.375f);
    EXPECT_EQ(fdiv(1.0f, 4194304.0f), 1.0f / 4194304.0f);   // 2^22
    // Out-of-domain: negative and zero denominators take the accurate path.
    EXPECT_EQ(fdiv(1.0f, -2.0f), -0.5f);
    EXPECT_TRUE(std::isinf(fdiv(1.0f, 0.0f)));
}

// General denominators: a * rcp(b) with a correctly-rounded (placeholder) or
// +-2 ULP (silicon) reciprocal — always within a few ULP of accurate division.
TEST(SfuMathTests, generalDivisionWithinUlps) {
    clearCaptureEnv();
    auto jit = CajetaJit::compile(SRC, "test.D");
    auto fdiv = jit->lookup<float (*)(float, float)>("fdiv");
    uint32_t s = 0x9E3779B9u;
    for (int i = 0; i < 20000; ++i) {
        s = s * 1664525u + 1013904223u;
        float a = ((float) (s >> 8) / 16777216.0f - 0.5f) * 2000.0f;
        s = s * 1664525u + 1013904223u;
        float b = ((float) (s >> 8) / 16777216.0f) * 999.0f + 0.001f;  // positive
        float got = fdiv(a, b);
        float ref = a / b;
        ASSERT_LE(ulpDiff(got, ref), 4.0f)
            << "a=" << a << " b=" << b << " got=" << got << " ref=" << ref;
    }
}

// fmaf reconstructs a single-rounding fused multiply-add exactly (2Sum +
// round-to-odd); libm's fma is the oracle. A large sweep across mixed
// magnitudes — any double-rounding defect shows up here.
TEST(SfuMathTests, fmafMatchesLibmExactly) {
    clearCaptureEnv();
    auto jit = CajetaJit::compile(SRC, "test.D");
    auto fma3 = jit->lookup<float (*)(float, float, float)>("fma3");
    uint32_t s = 0xB5297A4Du;
    for (int i = 0; i < 200000; ++i) {
        auto next = [&s]() {
            s = s * 1664525u + 1013904223u;
            int exp = (int) (s >> 27) - 16;               // 2^-16 .. 2^15
            float m = (float) ((s >> 8) & 0x7FFFF) / 524288.0f + 0.5f;
            float v = std::ldexp(m, exp);
            return (s & 1u) ? -v : v;
        };
        float a = next(), b = next(), c = next();
        float got = fma3(a, b, c);
        float ref = std::fmaf(a, b, c);
        ASSERT_EQ(std::isnan(got), std::isnan(ref));
        if (!std::isnan(ref))
            ASSERT_EQ(got, ref) << "a=" << a << " b=" << b << " c=" << c;
    }
}

// Placeholder expf: the exact CUDA reduction pipeline around a correctly-
// rounded exp2 — within a few ULP of accurate expf across the normal range,
// and exactly 1 at 0 (the reduction is exact there).
TEST(SfuMathTests, expfPlaceholderTracksAccurate) {
    clearCaptureEnv();
    auto jit = CajetaJit::compile(SRC, "test.D");
    auto ex = jit->lookup<float (*)(float)>("ex");
    EXPECT_EQ(ex(0.0f), 1.0f);
    for (float x = -87.0f; x <= 87.0f; x += 0.37f) {
        float got = ex(x);
        float ref = std::exp(x);
        ASSERT_NEAR(got / ref, 1.0f, 4e-6f) << "x=" << x;
    }
    // Saturation tails.
    EXPECT_EQ(ex(-200.0f), 0.0f);
    EXPECT_TRUE(std::isinf(ex(200.0f)));
}
