//
// XpuSobolTests — cajeta-gfx plan §3 (3.b-rest-sobol): the Sobol' (0,2)-sequence
// quasi-random low-discrepancy generator (cajeta.xpu.Sobol).
//
// The optional low-discrepancy quality upgrade parked off LowDiscrepancy. Sobol'
// is a digital (0,2)-net: its 2-D points are stratified across every dyadic
// elementary interval, so the first 2^k points hit every cell of every aligned
// 2^a x 2^b grid (a+b=k) exactly once — a stronger fill than Halton's coprime-base
// radical inverses. Built by the standard direction-number method (Bratley & Fox /
// Joe & Kuo): dimension 0's identity directions reproduce the Van der Corput
// base-2 radical inverse exactly (a cross-check against LowDiscrepancy), and
// dimension 1 (primitive polynomial x+1) is the classic companion axis.
//
// Direction numbers are carried as int64 (no int32 sign-bit on 2^31/2^32 values)
// and every scale is built by doubling (int64 add only) — no int32 `>>>`, no
// variable-amount or sign-bit-constant shift/multiply. Pure, deterministic per
// index, so it verifies on the host against exact known values (pure host JIT,
// no device).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP =
    "import cajeta.xpu.Sobol;\n"
    "import cajeta.xpu.LowDiscrepancy;\n";

} // namespace

// 3.b-rest-sobol — dimension 0 IS the Van der Corput base-2 radical inverse:
// dim0(1)=0.5, dim0(2)=0.25, dim0(3)=0.75, dim0(4)=0.125, dim0(0)=0; and each
// matches LowDiscrepancy.vanDerCorput of the same index exactly.
TEST(XpuSobolTests, dim0MatchesVanDerCorput) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Sobol.dim0(1);\n"
        "        if (a < 0.499f) { return -1; }\n"
        "        if (a > 0.501f) { return -2; }\n"
        "        float32 b = Sobol.dim0(2);\n"
        "        if (b < 0.249f) { return -3; }\n"
        "        if (b > 0.251f) { return -4; }\n"
        "        float32 c = Sobol.dim0(3);\n"
        "        if (c < 0.749f) { return -5; }\n"
        "        if (c > 0.751f) { return -6; }\n"
        "        float32 d = Sobol.dim0(4);\n"
        "        if (d < 0.124f) { return -7; }\n"
        "        if (d > 0.126f) { return -8; }\n"
        "        float32 z = Sobol.dim0(0);\n"
        "        if (z < -0.001f) { return -9; }\n"
        "        if (z > 0.001f) { return -10; }\n"
        // cross-check vs the radical inverse, index by index
        "        int32 i = 0;\n"
        "        while (i < 32) {\n"
        "            float32 s = Sobol.dim0(i);\n"
        "            float32 r = LowDiscrepancy.vanDerCorput(i);\n"
        "            float32 e = s - r;\n"
        "            if (e < 0.0f) { e = 0.0f - e; }\n"
        "            if (e > 0.00001f) { return -11; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3.b-rest-sobol — dimension 1 known values (primitive polynomial x+1, m_1=1):
// dim1(1)=0.5, dim1(2)=0.75, dim1(3)=0.25, dim1(4)=0.625, dim1(0)=0. These are
// the exact dyadic fractions of the classic second Sobol' axis.
TEST(XpuSobolTests, dim1KnownValues) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Sobol.dim1(1);\n"
        "        if (a < 0.499f) { return -1; }\n"
        "        if (a > 0.501f) { return -2; }\n"
        "        float32 b = Sobol.dim1(2);\n"
        "        if (b < 0.749f) { return -3; }\n"
        "        if (b > 0.751f) { return -4; }\n"
        "        float32 c = Sobol.dim1(3);\n"
        "        if (c < 0.249f) { return -5; }\n"
        "        if (c > 0.251f) { return -6; }\n"
        "        float32 d = Sobol.dim1(4);\n"
        "        if (d < 0.624f) { return -7; }\n"
        "        if (d > 0.626f) { return -8; }\n"
        "        float32 z = Sobol.dim1(0);\n"
        "        if (z < -0.001f) { return -9; }\n"
        "        if (z > 0.001f) { return -10; }\n"
        "        return 0;\n"), 0);
}

// 3.b-rest-sobol — every sample of both dimensions stays in [0, 1): scan a
// stretch of indices and reject anything outside the unit interval.
TEST(XpuSobolTests, samplesStayInUnitRange) {
    EXPECT_EQ(runI32(IMP,
        "        int32 i = 0;\n"
        "        while (i < 128) {\n"
        "            float32 x = Sobol.dim0(i);\n"
        "            if (x < 0.0f) { return -1; }\n"
        "            if (x >= 1.0f) { return -2; }\n"
        "            float32 y = Sobol.dim1(i);\n"
        "            if (y < 0.0f) { return -3; }\n"
        "            if (y >= 1.0f) { return -4; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3.b-rest-sobol — the (0,2)-net stratification property: the first 4 points
// (dim0, dim1) fall one in each of the four quadrants of the unit square (the
// 2x2 elementary-interval grid). Accumulate a per-quadrant count and require
// each quadrant to be hit exactly once.
TEST(XpuSobolTests, twoDStratifiedQuadrants) {
    EXPECT_EQ(runI32(IMP,
        "        int32 q0 = 0;\n"
        "        int32 q1 = 0;\n"
        "        int32 q2 = 0;\n"
        "        int32 q3 = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            float32 x = Sobol.dim0(i);\n"
        "            float32 y = Sobol.dim1(i);\n"
        "            int32 qx = 0;\n"
        "            if (x >= 0.5f) { qx = 1; }\n"
        "            int32 qy = 0;\n"
        "            if (y >= 0.5f) { qy = 1; }\n"
        "            int32 q = qx * 2 + qy;\n"
        "            if (q == 0) { q0 = q0 + 1; }\n"
        "            if (q == 1) { q1 = q1 + 1; }\n"
        "            if (q == 2) { q2 = q2 + 1; }\n"
        "            if (q == 3) { q3 = q3 + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (q0 != 1) { return -1; }\n"
        "        if (q1 != 1) { return -2; }\n"
        "        if (q2 != 1) { return -3; }\n"
        "        if (q3 != 1) { return -4; }\n"
        "        return 0;\n"), 0);
}
