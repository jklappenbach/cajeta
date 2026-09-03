//
// CajetaXPU — DeviceProfile live-query validation on NVIDIA (the CUDA twin of
// XpuDeviceProfileAmdDeviceTests). The runtime query was HIP-only, so on an
// NVIDIA box `cajeta_xpu_query_raw_device` returned 0 and the model fell back
// to conservative defaults that are gfx1151-SHAPED — arch "unknown", cu 0,
// regs_per_mp 196608, lds 65536. Those are not merely missing, they are wrong
// numbers wearing an NVIDIA badge, and `estimated: true` was the only signal.
//
// Skips cleanly when no CUDA driver/device is present.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/core/DeviceProfile.h"
#include "cajeta_xpu_abi.h"
#include "XpuDeviceTestUtil.h"

#include <iostream>
#include <string>

using cajeta::xpu::DeviceModel;
using cajeta::xpu::DeviceProfile;
using cajeta::xpu::queryLiveDeviceModel;
using cajeta::xpu::queryLiveDeviceProfile;

// The runtime answers at all on CUDA — the bit that was missing entirely.
TEST(XpuDeviceProfileNvidiaDeviceTests, rawQueryAnswersOnCuda) {
    CAJETA_SKIP_IF_NO_CUDA();
    CajetaXpuRawDevice raw;
    ASSERT_EQ(cajeta_xpu_query_raw_device(&raw), 1)
        << "the raw device query must answer on a CUDA device";
    EXPECT_TRUE(raw.valid);
    EXPECT_EQ(std::string(raw.archName).rfind("sm_", 0), 0u)
        << "arch should be the sm_XY the nvptx backend also spells: "
        << raw.archName;
    EXPECT_EQ(raw.waveSize, 32u);
    EXPECT_GT(raw.multiprocessorCount, 0u);
    EXPECT_GT(raw.regsPerMP, 0u);
    EXPECT_GT(raw.threadsPerMP, 0u);
    EXPECT_GT(raw.ldsBytesPerMP, 0u);
}

// A reachable CUDA device yields a MEASURED model, not an estimated one.
TEST(XpuDeviceProfileNvidiaDeviceTests, liveQueryPopulatesModel) {
    CAJETA_SKIP_IF_NO_CUDA();
    DeviceModel m = queryLiveDeviceModel();
    EXPECT_FALSE(m.estimated) << "a reachable GPU should yield a measured model";
    EXPECT_FALSE(m.archName.empty());
    EXPECT_NE(m.archName, "unknown");
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_GT(m.maxThreadsPerBlock, 0u);
    EXPECT_GT(m.cuCount, 0u);
    EXPECT_GT(m.regsPerMP, 0u);
    EXPECT_GT(m.maxWavesPerMP, 0u);
    EXPECT_GT(m.ldsBytesPerMP, 0u);
}

// The specific Ada constants, measured on this box's RTX 4090 with a dlopen
// probe against libcuda.so.1 before any of this was written.
TEST(XpuDeviceProfileNvidiaDeviceTests, ada4090KnownConstants) {
    CAJETA_SKIP_IF_NO_CUDA();
    CajetaXpuRawDevice raw;
    ASSERT_EQ(cajeta_xpu_query_raw_device(&raw), 1);
    if (std::string(raw.archName) != "sm_89")
        GTEST_SKIP() << "not sm_89: " << raw.archName;
    DeviceModel m = queryLiveDeviceModel();
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.regsPerMP, 65536u);       // MAX_REGISTERS_PER_MULTIPROCESSOR
    EXPECT_EQ(m.maxWavesPerMP, 48u);      // 1536 threads / 32 per warp
    EXPECT_EQ(m.ldsBytesPerMP, 102400u);  // 100 KB shared per SM
    EXPECT_EQ(m.maxThreadsPerBlock, 1024u);
}

// The honest-negative guard: whatever the model says on NVIDIA, it must not be
// the gfx1151-shaped default. Without this a regression in the query is
// invisible — the profile still prints, and every number in it is plausible.
TEST(XpuDeviceProfileNvidiaDeviceTests, modelIsNotTheAmdShapedDefault) {
    CAJETA_SKIP_IF_NO_CUDA();
    DeviceModel m = queryLiveDeviceModel();
    EXPECT_NE(m.regsPerMP, 196608u) << "that is the gfx1151 register file";
    EXPECT_NE(m.cuCount, 0u);
}

// The roofline probe — HIP-only alongside the query, so NVIDIA reported
// rooflineMeasured:false and a 0 GB/s denominator.
TEST(XpuDeviceProfileNvidiaDeviceTests, bandwidthProbeMeasures) {
    CAJETA_SKIP_IF_NO_CUDA();
    double gbps = cajeta_xpu_measure_bandwidth_gbps(64ull << 20, 3);
    std::cout << "[ device   ] measured device bandwidth: " << gbps << " GB/s\n";
    EXPECT_GT(gbps, 10.0) << "measured " << gbps << " GB/s";
    EXPECT_LT(gbps, 5000.0) << "implausibly high: " << gbps << " GB/s";
}

TEST(XpuDeviceProfileNvidiaDeviceTests, profileCarriesRoofline) {
    CAJETA_SKIP_IF_NO_CUDA();
    DeviceProfile p = queryLiveDeviceProfile();
    EXPECT_FALSE(p.model.estimated);
    EXPECT_TRUE(p.rooflineMeasured);
    EXPECT_GT(p.bandwidthGBps, 10.0) << "measured " << p.bandwidthGBps << " GB/s";
}

// ---- device-geometry-parameterization, on the live part ---------------- //

// Plan 1.1.5 — every Tier-B fact the standalone probe read must arrive through
// the ABI unchanged. Asserted as RELATIONS, not as this SKU's literals, so the
// test stays true on another NVIDIA part while still catching a field that
// silently stops being filled.
TEST(XpuDeviceProfileNvidiaDeviceTests, tierBGeometryIsQueriedLive) {
    CAJETA_SKIP_IF_NO_CUDA();
    const cajeta::xpu::DeviceModel m = cajeta::xpu::queryLiveDeviceModel();
    ASSERT_TRUE(m.queried);
    EXPECT_GT(m.ldsBytesPerBlock, 0u);
    EXPECT_LE(m.ldsBytesPerBlock, m.ldsBytesPerMP)
        << "a block cannot be given more shared memory than its SM has";
    EXPECT_GE(m.ldsBytesPerBlockOptin, m.ldsBytesPerBlock)
        << "the opt-in ceiling is the raised one";
    EXPECT_GT(m.maxBlocksPerMP, 0u);
    EXPECT_GT(m.l2CacheBytes, 0u);
    EXPECT_GT(m.totalGlobalMemBytes, 0ull);
    EXPECT_GT(m.memoryBusWidthBits, 0u);
    EXPECT_GT(m.memoryClockKHz, 0u);
    EXPECT_EQ(m.simdsPerMP, 4u) << "an SM has 4 scheduler partitions";
    EXPECT_EQ(m.mpCount, m.cuCount) << "an SM is the multiprocessor; no folding";
}

// The exhibit-2 gap, asserted on the real part rather than on a fixture: the
// per-block ceiling is STRICTLY below the per-MP budget here, which is what
// makes a tile sized against the per-MP figure assemble on AMD and fail on
// NVIDIA. If this ever stops holding, the distinction the model now draws has
// become unobservable and these tests would pass vacuously.
TEST(XpuDeviceProfileNvidiaDeviceTests, perBlockCeilingIsBelowThePerMpBudget) {
    CAJETA_SKIP_IF_NO_CUDA();
    const cajeta::xpu::DeviceModel m = cajeta::xpu::queryLiveDeviceModel();
    ASSERT_TRUE(m.queried);
    EXPECT_LT(m.ldsBytesPerBlock, m.ldsBytesPerMP);
    EXPECT_EQ(cajeta::xpu::ldsCeilingPerBlock(m), m.ldsBytesPerBlock);
}

// L1 on the live device: a real block count, and one that is a whole multiple
// of the part's SIMD population.
TEST(XpuDeviceProfileNvidiaDeviceTests, dispatchLawAnswersOnTheLivePart) {
    CAJETA_SKIP_IF_NO_CUDA();
    const cajeta::xpu::DeviceModel m = cajeta::xpu::queryLiveDeviceModel();
    ASSERT_TRUE(m.queried);
    const unsigned blocks = cajeta::xpu::dispatchBlocks(m, 2);
    EXPECT_GT(blocks, 0u);
    EXPECT_EQ(blocks * 2u, m.mpCount * m.simdsPerMP);
}

// L4: a measured value ABOVE theoretical would mean the probe is timing cache
// rather than memory — exactly the failure a single number cannot reveal, and
// the reason the attribute-derived ceiling exists beside the measured one.
//
// The CEILING is the invariant; there is deliberately no floor. An earlier
// revision asserted `> 0.30 * theoretical` and failed inside a full `Xpu*`
// run while passing in isolation: the probe shares the device with every
// other device test, so a low reading there measures CONTENTION, not the
// part. A floor that fires on a busy machine reports a bandwidth regression
// that did not happen, which is worse than not checking a floor at all.
// Observed spread on an idle 4090 across runs: 866.1 / 882.6 / 915.5 GB/s.
TEST(XpuDeviceProfileNvidiaDeviceTests, measuredBandwidthSitsUnderTheoretical) {
    CAJETA_SKIP_IF_NO_CUDA();
    const cajeta::xpu::DeviceProfile p = cajeta::xpu::queryLiveDeviceProfile();
    ASSERT_GT(p.theoreticalBwGBps, 0.0);
    if (!p.rooflineMeasured) GTEST_SKIP() << "roofline probe did not run";
    EXPECT_GT(p.bandwidthGBps, 0.0);
    EXPECT_LT(p.bandwidthGBps, 1.05 * p.theoreticalBwGBps)
        << "a probe reading above the attribute-derived ceiling is timing "
           "cache, not memory";
}
