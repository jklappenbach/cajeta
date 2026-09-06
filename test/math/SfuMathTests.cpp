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

// The correctly-rounded float32 fma, computed exactly and portably. The
// product of two float32s is EXACT in a double (24 + 24 = 48 < 53 bits), so
// a*b + c is the exact value (p + c) with p a double; TwoSum splits that sum
// into a rounded double s and an exactly-representable error e, so the true
// value is s + e with no information lost. Rounding s to float32 is correct
// unless s sits EXACTLY on a float32 rounding midpoint, where the sign of e
// decides (e == 0 is a genuine tie: round-to-even, i.e. what (float)s gives).
//
// Why not libm's fmaf: on Windows, mingw-w64's std::fmaf is NOT correctly
// rounded. Measured 2026-09-06 (release full sweep): a=161.18603515625,
// b=-2343.9921875, c=1.4512825012207031 — exact a*b+c = -377817.3558578…,
// correctly rounded float32 = -377817.34375 (which cajeta's fmaf returned);
// mingw's fmaf returned -377817.375, one ulp off. glibc's is correct, which
// is why this test was green on Linux with std::fmaf as the oracle. The
// product is the thing under test; the oracle has to be beyond doubt.
static float exactFmaf(float a, float b, float c) {
    const double p = (double) a * (double) b;   // exact
    const double s = p + (double) c;            // rounded
    // TwoSum (Knuth): e is exact, s + e == p + c exactly.
    const double bb = s - p;
    const double e = (p - (s - bb)) + ((double) c - bb);
    const float r = (float) s;
    if (e == 0.0) return r;
    // Is s exactly halfway between r and its neighbour toward s's other side?
    const float up = std::nextafter(r, (float) INFINITY);
    const float dn = std::nextafter(r, (float) -INFINITY);
    const double midUp = ((double) r + (double) up) * 0.5;
    const double midDn = ((double) r + (double) dn) * 0.5;
    if (s == midUp) return e > 0.0 ? up : r;
    if (s == midDn) return e < 0.0 ? dn : r;
    return r;   // s strictly inside r's rounding interval: e cannot move it
}

// fmaf reconstructs a single-rounding fused multiply-add exactly (2Sum +
// round-to-odd); the oracle is the exact correctly-rounded value above. A
// large sweep across mixed magnitudes — any double-rounding defect shows up
// here. On glibc the exact oracle and std::fmaf must agree (a check on the
// oracle itself); on mingw they do not, and mingw is the one that is wrong.
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
        float ref = exactFmaf(a, b, c);
#if !defined(_WIN32)
        // glibc's fmaf is correctly rounded: it must agree with the exact
        // oracle, or the oracle is what's broken.
        ASSERT_EQ(std::fmaf(a, b, c), ref) << "oracle self-check a=" << a
                                           << " b=" << b << " c=" << c;
#endif
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
