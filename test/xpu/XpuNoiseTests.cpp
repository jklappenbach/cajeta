//
// XpuNoiseTests — cajeta-gfx plan §3 (slice 3b): procedural gradient noise
// (cajeta.xpu.Noise).
//
// TDD 3.c's second clause: "noise is deterministic per seed". Noise.perlin3 is
// Ken Perlin's improved 3-D gradient noise: each integer-lattice corner is
// assigned a pseudo-random gradient (hashed from its coords + the seed), and
// the value at a point is the fade-weighted trilinear blend of each corner's
// gradient dotted with the offset to it. Because the offset to a corner is the
// zero vector exactly at an integer point, the dot products there are all zero
// and the noise is EXACTLY 0 on the integer lattice — a clean analytic anchor.
// It is a pure deterministic function of (x, y, z, seed), so the host can check
// determinism, seed-dependence, the lattice zeros, and the bounded range.
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

const char* IMP = "import cajeta.xpu.Noise;\n";

} // namespace
// (The throwaway GfxNoiseProbe bisection suite that localized a chained-int64-
// multiply codegen SIGILL to Noise.hashCorner has been removed; the finding is
// recorded in the value-type-codegen-gotchas memory and Noise.cajeta's comments.)


// 3c — Perlin noise is exactly 0 on the integer lattice (the offset to the
// containing corner is zero there, so every gradient dot is zero), for any seed
// and including negative coordinates.
TEST(XpuNoiseTests, zeroAtLattice) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Noise.perlin3(2.0f, 3.0f, 5.0f, 42);\n"
        "        if (a < -0.0005f) { return -1; }\n"
        "        if (a > 0.0005f) { return -2; }\n"
        "        float32 b = Noise.perlin3(0.0f, 0.0f, 0.0f, 42);\n"
        "        if (b < -0.0005f) { return -3; }\n"
        "        if (b > 0.0005f) { return -4; }\n"
        "        float32 c = Noise.perlin3(-3.0f, 7.0f, -1.0f, 99);\n"
        "        if (c < -0.0005f) { return -5; }\n"
        "        if (c > 0.0005f) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 3c — deterministic per seed: the same (x,y,z,seed) replays exactly; and the
// field varies across space (two different points differ).
TEST(XpuNoiseTests, deterministicAndVaries) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Noise.perlin3(0.3f, 0.7f, 0.1f, 42);\n"
        "        float32 b = Noise.perlin3(0.3f, 0.7f, 0.1f, 42);\n"
        "        if (a != b) { return -1; }\n"
        "        float32 p = Noise.perlin3(0.6f, 0.2f, 0.8f, 42);\n"
        "        if (a == p) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 3c — the seed selects a different field: the same point under two seeds gives
// different values.
TEST(XpuNoiseTests, seedChangesField) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Noise.perlin3(0.5f, 0.5f, 0.5f, 1);\n"
        "        float32 b = Noise.perlin3(0.5f, 0.5f, 0.5f, 2);\n"
        "        if (a == b) { return -1; }\n"
        "        return 0;\n"), 0);
}

// 3c — bounded, non-trivial range: scanning a grid of non-lattice points every
// value stays within [-1.5, 1.5] (catches NaN/inf/garbage) and at least one is
// substantially nonzero (the field isn't degenerate).
TEST(XpuNoiseTests, boundedRange) {
    EXPECT_EQ(runI32(IMP,
        "        int32 i = 0;\n"
        "        int32 sawBig = 0;\n"
        "        while (i < 512) {\n"
        "            int32 a = i % 8;\n"
        "            int32 b = (i / 8) % 8;\n"
        "            int32 c = (i / 64) % 8;\n"
        "            float32 fa = a;\n"
        "            float32 fb = b;\n"
        "            float32 fc = c;\n"
        "            float32 x = fa * 0.37f - 1.3f;\n"
        "            float32 y = fb * 0.41f + 0.13f;\n"
        "            float32 z = fc * 0.29f - 0.7f;\n"
        "            float32 v = Noise.perlin3(x, y, z, 7);\n"
        "            if (v < -1.5f) { return -1; }\n"
        "            if (v > 1.5f) { return -2; }\n"
        "            float32 av = v;\n"
        "            if (av < 0.0f) { av = 0.0f - av; }\n"
        "            if (av > 0.1f) { sawBig = 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (sawBig == 0) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 3c-rest — Worley (cellular) F1 noise: the distance to the nearest jittered
// feature point. Over a grid it stays non-negative and bounded (a feature lives
// within the 3x3x3 neighbourhood, so F1 < ~1.8), and it is deterministic per
// (point, seed) while varying across both.
TEST(XpuNoiseTests, worleyF1BoundedDeterministic) {
    EXPECT_EQ(runI32(IMP,
        "        int32 i = 0;\n"
        "        int32 sawDiff = 0;\n"
        "        float32 prev = 0.0f - 1.0f;\n"
        "        while (i < 343) {\n"
        "            int32 a = i % 7;\n"
        "            int32 b = (i / 7) % 7;\n"
        "            int32 c = (i / 49) % 7;\n"
        "            float32 fa = a;\n"
        "            float32 fb = b;\n"
        "            float32 fc = c;\n"
        "            float32 x = fa * 0.37f - 1.0f;\n"
        "            float32 y = fb * 0.37f - 1.0f;\n"
        "            float32 z = fc * 0.37f - 1.0f;\n"
        "            float32 f = Noise.worleyF1(x, y, z, 1234);\n"
        "            if (f < 0.0f) { return -1; }\n"
        "            if (f > 1.8f) { return -2; }\n"
        // determinism: same args -> same value
        "            float32 f2 = Noise.worleyF1(x, y, z, 1234);\n"
        "            if (f != f2) { return -3; }\n"
        "            if (i > 0) { if (f != prev) { sawDiff = 1; } }\n"
        "            prev = f;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (sawDiff == 0) { return -4; }\n"           // field is non-constant
        // seed changes the field at a fixed point
        "        float32 s1 = Noise.worleyF1(0.3f, 0.7f, 0.1f, 1);\n"
        "        float32 s2 = Noise.worleyF1(0.3f, 0.7f, 0.1f, 999);\n"
        "        if (s1 == s2) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 3c-rest — 3-D simplex noise: deterministic per (point, seed), bounded near
// [-1, 1], and non-constant across position and seed (the tetrahedral-lattice
// alternative to perlin3).
TEST(XpuNoiseTests, simplex3BoundedDeterministic) {
    EXPECT_EQ(runI32(IMP,
        "        int32 i = 0;\n"
        "        int32 sawBig = 0;\n"
        "        int32 sawDiff = 0;\n"
        "        float32 prev = 0.0f;\n"
        "        while (i < 343) {\n"
        "            int32 a = i % 7;\n"
        "            int32 b = (i / 7) % 7;\n"
        "            int32 c = (i / 49) % 7;\n"
        "            float32 fa = a;\n"
        "            float32 fb = b;\n"
        "            float32 fc = c;\n"
        "            float32 x = fa * 0.43f - 1.5f;\n"
        "            float32 y = fb * 0.43f - 1.5f;\n"
        "            float32 z = fc * 0.43f - 1.5f;\n"
        "            float32 v = Noise.simplex3(x, y, z, 7);\n"
        "            float32 av = v;\n"
        "            if (av < 0.0f) { av = 0.0f - av; }\n"
        "            if (av > 1.2f) { return -1; }\n"
        "            if (av > 0.05f) { sawBig = 1; }\n"
        "            float32 v2 = Noise.simplex3(x, y, z, 7);\n"
        "            if (v != v2) { return -2; }\n"
        "            if (i > 0) { if (v != prev) { sawDiff = 1; } }\n"
        "            prev = v;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (sawBig == 0) { return -3; }\n"
        "        if (sawDiff == 0) { return -4; }\n"
        "        float32 s1 = Noise.simplex3(0.3f, 0.7f, 0.1f, 1);\n"
        "        float32 s2 = Noise.simplex3(0.3f, 0.7f, 0.1f, 999);\n"
        "        if (s1 == s2) { return -5; }\n"
        "        return 0;\n"), 0);
}
