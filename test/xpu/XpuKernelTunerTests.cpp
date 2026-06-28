//
// KernelTuner — GPU-free tests for the three-tier config lookup
// (kernel-occupancy-autotune U3a, §4): runtime cache → shipped guidance →
// analysis sweep, with the timer injected.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/core/KernelTuner.h"

#include <vector>

using cajeta::xpu::KernelTuner;
using cajeta::xpu::TuningConfig;
using cajeta::xpu::TuningKey;
using cajeta::xpu::TuningKeyHash;

namespace {
const std::vector<TuningConfig> kCandidates = {{64}, {128}, {256}, {512}};
}

// 3a.1.a — on a miss, the sweep picks the minimum-cost candidate and caches it.
TEST(XpuKernelTunerTests, sweepPicksMinAndCaches) {
    KernelTuner tuner;
    TuningKey key{"gfx1151", "userKernel", 4096};
    std::vector<unsigned> measured;
    // Cost minimized at blockX == 256.
    auto measure = [&](const TuningConfig& c) -> double {
        measured.push_back(c.blockX);
        return c.blockX == 256 ? 1.0 : 10.0;
    };
    TuningConfig got = tuner.selectConfig(key, kCandidates, 0, measure);
    EXPECT_EQ(got.blockX, 256u);
    EXPECT_EQ(measured.size(), 4u) << "all in-clamp candidates timed";
    ASSERT_TRUE(tuner.cached(key).has_value());
    EXPECT_EQ(tuner.cached(key)->blockX, 256u);
}

// 3a.1.b — a runtime-cache hit returns immediately; the timer is never called.
TEST(XpuKernelTunerTests, cacheHitDoesNotMeasure) {
    KernelTuner tuner;
    TuningKey key{"gfx1151", "userKernel", 4096};
    int calls = 0;
    auto count = [&](const TuningConfig& c) -> double {
        ++calls; return c.blockX == 128 ? 1.0 : 5.0;
    };
    tuner.selectConfig(key, kCandidates, 0, count);   // miss -> sweep populates cache
    ASSERT_GT(calls, 0);
    calls = 0;
    TuningConfig got = tuner.selectConfig(key, kCandidates, 0, count);
    EXPECT_EQ(calls, 0) << "a cache hit must not measure";
    EXPECT_EQ(got.blockX, 128u);
}

// 3a.1.c — a shipped-guidance hit returns with no measurement and seeds the cache.
TEST(XpuKernelTunerTests, guidanceHitDoesNotMeasureAndSeedsCache) {
    KernelTuner tuner;
    TuningKey key{"gfx1151", "matmul-f16", 0};   // present in the shipped DB
    ASSERT_TRUE(KernelTuner::guidance(key).has_value());
    auto fail = [](const TuningConfig&) -> double {
        ADD_FAILURE() << "guidance hit must not measure"; return 0.0;
    };
    TuningConfig got = tuner.selectConfig(key, kCandidates, 0, fail);
    EXPECT_EQ(got.blockX, 128u);
    ASSERT_TRUE(tuner.cached(key).has_value()) << "guidance seeds the cache";
    EXPECT_EQ(tuner.cached(key)->blockX, 128u);
}

// 3a.1.d — an @Occupancy/§2 clamp removes over-budget candidates from the sweep.
TEST(XpuKernelTunerTests, clampRemovesOverBudgetCandidates) {
    KernelTuner tuner;
    TuningKey key{"gfx1151", "userKernel", 7};
    std::vector<unsigned> measured;
    // Bigger is "better" — but the clamp must keep us at <= 128.
    auto measure = [&](const TuningConfig& c) -> double {
        measured.push_back(c.blockX);
        return -static_cast<double>(c.blockX);
    };
    TuningConfig got = tuner.selectConfig(key, kCandidates, /*clamp=*/128, measure);
    EXPECT_EQ(got.blockX, 128u) << "256/512 excluded by the clamp";
    for (unsigned m : measured)
        EXPECT_LE(m, 128u) << "no over-clamp candidate is ever timed";
}

// 3a.1.d (guidance variant) — a guidance config exceeding the clamp is rejected,
// falling through to a clamped sweep.
TEST(XpuKernelTunerTests, clampRejectsOverBudgetGuidance) {
    KernelTuner tuner;
    TuningKey key{"gfx1151", "matmul-f16", 0};   // guidance = {128}
    auto measure = [&](const TuningConfig& c) -> double { return c.blockX; };
    TuningConfig got = tuner.selectConfig(key, {{32}, {64}}, /*clamp=*/64, measure);
    EXPECT_LE(got.blockX, 64u) << "guidance 128 > clamp 64 -> swept under the clamp";
}

// 3a.1.e — TuningKey equality + hashing: same fields collide, distinct shapes don't.
TEST(XpuKernelTunerTests, keyEqualityAndHash) {
    TuningKey a{"gfx1151", "k", 100};
    TuningKey b{"gfx1151", "k", 100};
    TuningKey c{"gfx1151", "k", 101};
    TuningKeyHash h;
    EXPECT_TRUE(a == b);
    EXPECT_EQ(h(a), h(b));
    EXPECT_FALSE(a == c);
}
