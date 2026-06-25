//
// XpuLowDiscrepancyTests — cajeta-gfx plan §3 (slice 1): the low-discrepancy
// sampling sequences (cajeta.xpu.LowDiscrepancy).
//
// §3 is the acceleration/sampling/residency FOUNDATION (cajeta.xpu, spec §8.7).
// Its sampling half rests on quasi-random low-discrepancy sequences: the Van der
// Corput radical inverse (reflect an index's base-b digits about the radix point)
// and the Halton / Hammersley points built from it. These are pure, deterministic-
// per-index float math — the analytic core of TDD 3.b ("Sobol/Halton low-
// discrepancy stats within bounds") — so they verify on the host with exact known
// values (Van der Corput base 2 of 1/2/3/4 = 0.5/0.25/0.75/0.125).
//
// The runI32 harness JITs a tiny program returning 0 on success or a negative
// failure code; floats are checked against a tolerance via two-sided range tests.
//
// GPU-free (pure host JIT; no device).
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

const char* IMP = "import cajeta.xpu.LowDiscrepancy;\n";

} // namespace

// 3b — Van der Corput (radical inverse base 2): index i reflected about the radix
// point. 1 -> .1b = 0.5, 2 -> .01b = 0.25, 3 -> .11b = 0.75, 4 -> .001b = 0.125,
// and 0 -> 0.
TEST(XpuLowDiscrepancyTests, vanDerCorputBase2KnownValues) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = LowDiscrepancy.vanDerCorput(1);\n"
        "        if (a < 0.499f) { return -1; }\n"
        "        if (a > 0.501f) { return -2; }\n"
        "        float32 b = LowDiscrepancy.vanDerCorput(2);\n"
        "        if (b < 0.249f) { return -3; }\n"
        "        if (b > 0.251f) { return -4; }\n"
        "        float32 c = LowDiscrepancy.vanDerCorput(3);\n"
        "        if (c < 0.749f) { return -5; }\n"
        "        if (c > 0.751f) { return -6; }\n"
        "        float32 d = LowDiscrepancy.vanDerCorput(4);\n"
        "        if (d < 0.124f) { return -7; }\n"
        "        if (d > 0.126f) { return -8; }\n"
        "        float32 z = LowDiscrepancy.vanDerCorput(0);\n"
        "        if (z < -0.001f) { return -9; }\n"
        "        if (z > 0.001f) { return -10; }\n"
        "        return 0;\n"), 0);
}

// 3b — radical inverse in other bases: base 3 of 1 = .1(3) = 1/3, of 2 = .2(3) =
// 2/3; base 2 of 1 = 0.5; index 0 in any base = 0.
TEST(XpuLowDiscrepancyTests, radicalInverseOtherBases) {
    EXPECT_EQ(runI32(IMP,
        "        float32 t = LowDiscrepancy.radicalInverse(1, 3);\n"
        "        if (t < 0.332f) { return -1; }\n"
        "        if (t > 0.334f) { return -2; }\n"
        "        float32 u = LowDiscrepancy.radicalInverse(2, 3);\n"
        "        if (u < 0.666f) { return -3; }\n"
        "        if (u > 0.667f) { return -4; }\n"
        "        float32 h = LowDiscrepancy.radicalInverse(1, 2);\n"
        "        if (h < 0.499f) { return -5; }\n"
        "        if (h > 0.501f) { return -6; }\n"
        "        float32 z = LowDiscrepancy.radicalInverse(0, 5);\n"
        "        if (z < -0.001f) { return -7; }\n"
        "        if (z > 0.001f) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3b — every sample is in [0, 1): scan a handful of indices in two bases and
// reject anything outside the unit interval.
TEST(XpuLowDiscrepancyTests, samplesStayInUnitRange) {
    EXPECT_EQ(runI32(IMP,
        "        int32 i = 1;\n"
        "        while (i < 64) {\n"
        "            float32 v2 = LowDiscrepancy.radicalInverse(i, 2);\n"
        "            if (v2 < 0.0f) { return -1; }\n"
        "            if (v2 >= 1.0f) { return -2; }\n"
        "            float32 v3 = LowDiscrepancy.radicalInverse(i, 3);\n"
        "            if (v3 < 0.0f) { return -3; }\n"
        "            if (v3 >= 1.0f) { return -4; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3b — halton(i, base) IS the radical inverse, and the Hammersley first coordinate
// is the uniform stratified axis index/count (0/4, 2/4 = 0.5, 3/4 = 0.75).
TEST(XpuLowDiscrepancyTests, haltonAndHammersley) {
    EXPECT_EQ(runI32(IMP,
        "        float32 hp = LowDiscrepancy.halton(3, 2);\n"   // == vanDerCorput(3) = 0.75
        "        if (hp < 0.749f) { return -1; }\n"
        "        if (hp > 0.751f) { return -2; }\n"
        "        float32 x0 = LowDiscrepancy.hammersleyX(0, 4);\n"
        "        if (x0 < -0.001f) { return -3; }\n"
        "        if (x0 > 0.001f) { return -4; }\n"
        "        float32 x2 = LowDiscrepancy.hammersleyX(2, 4);\n"
        "        if (x2 < 0.499f) { return -5; }\n"
        "        if (x2 > 0.501f) { return -6; }\n"
        "        float32 x3 = LowDiscrepancy.hammersleyX(3, 4);\n"
        "        if (x3 < 0.749f) { return -7; }\n"
        "        if (x3 > 0.751f) { return -8; }\n"
        "        return 0;\n"), 0);
}
