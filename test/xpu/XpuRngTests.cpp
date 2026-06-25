//
// XpuRngTests — cajeta-gfx plan §3 (slice 2): the stateful stream RNG +
// weighted reservoir sampling (cajeta.xpu.Rng).
//
// The other half of TDD 3.b ("PCG/xoshiro reproducible per seed; ... reservoir
// WRS converges to the target distribution"). Rng is an xorshift64 stream (the
// advanced 64-bit state is itself the output) threaded functionally —
// seedState(seed) -> state, next(state) -> state, output(state) -> 64 random
// bits (int64), toUnitFloat(bits) -> [0,1). weightedReservoirChoice does
// one-pass reservoir-of-size-1 (RIS) selection with probability proportional to
// weight.
//
// xorshift64 (not PCG/splitmix64) because the current JIT codegen mis-lowers
// the int64-multiply-by-a-sign-bit-set-constant that those generators need
// (illegal instruction) and the int32 `>>>` PCG's rotate uses (arithmetic
// shift). xorshift64 is multiply-free and uses only constant int64 shifts +
// xor, which lower correctly. (Diagnosed via throwaway shift/multiply probes,
// recorded in the cajeta-value-type-codegen-gotchas memory.)
//
// All host-verifiable: reproducibility is exact equality; the unit-float range
// and a quartile-uniformity check are statistical-but-deterministic (fixed
// seed); reservoir convergence counts selections over many trials and checks
// the empirical frequencies track the weights.
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

const char* IMP = "import cajeta.xpu.Rng;\n";

} // namespace

// 3b — reproducible per seed: the same seed yields the same stream; a different
// seed yields a different first output; consecutive outputs differ from each
// other (the generator isn't stuck on a short cycle).
TEST(XpuRngTests, reproducibleAndDistinct) {
    EXPECT_EQ(runI32(IMP,
        "        int64 a = Rng.seedState(42);\n"
        "        int64 r1 = Rng.output(a);\n"
        "        a = Rng.next(a);\n"
        "        int64 r2 = Rng.output(a);\n"
        "        a = Rng.next(a);\n"
        "        int64 r3 = Rng.output(a);\n"
        "        int64 b = Rng.seedState(42);\n"
        "        int64 q1 = Rng.output(b);\n"
        "        b = Rng.next(b);\n"
        "        int64 q2 = Rng.output(b);\n"
        "        b = Rng.next(b);\n"
        "        int64 q3 = Rng.output(b);\n"
        "        if (r1 != q1) { return -1; }\n"
        "        if (r2 != q2) { return -2; }\n"
        "        if (r3 != q3) { return -3; }\n"
        "        int64 c = Rng.seedState(43);\n"
        "        int64 p1 = Rng.output(c);\n"
        "        if (p1 == r1) { return -4; }\n"
        "        if (r1 == r2) { return -5; }\n"
        "        if (r2 == r3) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 3b — every toUnitFloat result is in [0, 1): scan a stream of 200 outputs.
TEST(XpuRngTests, unitFloatInRange) {
    EXPECT_EQ(runI32(IMP,
        "        int64 s = Rng.seedState(7);\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"
        "            s = Rng.next(s);\n"
        "            float32 u = Rng.toUnitFloat(Rng.output(s));\n"
        "            if (u < 0.0f) { return -1; }\n"
        "            if (u >= 1.0f) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3b — rough uniformity: 2000 unit floats binned into quartiles each land near
// 500 (every bin in [350, 650]) and account for all samples (no out-of-range).
TEST(XpuRngTests, quartileUniformity) {
    EXPECT_EQ(runI32(IMP,
        "        int64 s = Rng.seedState(99);\n"
        "        int32 b0 = 0;\n"
        "        int32 b1 = 0;\n"
        "        int32 b2 = 0;\n"
        "        int32 b3 = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 2000) {\n"
        "            s = Rng.next(s);\n"
        "            float32 u = Rng.toUnitFloat(Rng.output(s));\n"
        "            if (u < 0.25f) { b0 = b0 + 1; }\n"
        "            if (u >= 0.25f) { if (u < 0.5f) { b1 = b1 + 1; } }\n"
        "            if (u >= 0.5f) { if (u < 0.75f) { b2 = b2 + 1; } }\n"
        "            if (u >= 0.75f) { b3 = b3 + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (b0 < 350) { return -1; }\n"
        "        if (b0 > 650) { return -2; }\n"
        "        if (b1 < 350) { return -3; }\n"
        "        if (b1 > 650) { return -4; }\n"
        "        if (b2 < 350) { return -5; }\n"
        "        if (b2 > 650) { return -6; }\n"
        "        if (b3 < 350) { return -7; }\n"
        "        if (b3 > 650) { return -8; }\n"
        "        int32 sum = b0 + b1 + b2 + b3;\n"
        "        if (sum != 2000) { return -9; }\n"
        "        return 0;\n"), 0);
}

// 3b — weighted reservoir sampling converges to the target distribution: with
// weights {1, 2, 3} (sum 6) the empirical selection frequencies over 6000 trials
// track 1/6, 2/6, 3/6. Each trial reseeds from a counter scrambled by a POSITIVE
// LCG step (positive-constant multiply is safe codegen; the negative-constant
// form crashes — see the file header) so adjacent trials draw decorrelated
// streams. -1 must never be returned.
TEST(XpuRngTests, weightedReservoirConverges) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] w = {1.0f, 2.0f, 3.0f};\n"
        "        int32 c0 = 0;\n"
        "        int32 c1 = 0;\n"
        "        int32 c2 = 0;\n"
        "        int32 t = 0;\n"
        "        while (t < 6000) {\n"
        "            int64 tt = t;\n"
        "            int64 sd = tt * 2654435761L + 1013904223L;\n"
        "            int32 pick = Rng.weightedReservoirChoice(w, 3, sd);\n"
        "            if (pick < 0) { return -10; }\n"
        "            if (pick == 0) { c0 = c0 + 1; }\n"
        "            if (pick == 1) { c1 = c1 + 1; }\n"
        "            if (pick == 2) { c2 = c2 + 1; }\n"
        "            t = t + 1;\n"
        "        }\n"
        // expected 1000 / 2000 / 3000; generous ±300 windows (~8 sigma)
        "        if (c0 < 700) { return -1; }\n"
        "        if (c0 > 1300) { return -2; }\n"
        "        if (c1 < 1700) { return -3; }\n"
        "        if (c1 > 2300) { return -4; }\n"
        "        if (c2 < 2700) { return -5; }\n"
        "        if (c2 > 3300) { return -6; }\n"
        "        int32 sum = c0 + c1 + c2;\n"
        "        if (sum != 6000) { return -7; }\n"
        "        return 0;\n"), 0);
}
