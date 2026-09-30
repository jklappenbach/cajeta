//
// CajetaXPU — DeviceProfile live-query validation (xpu-device-profile U1, 1.10).
// On a real GPU the runtime query (hipDeviceGetAttribute + the gfx-arch scan)
// must fill the machine model and report a measured (not estimated) profile;
// on this gfx1151 box the known RDNA3.5 constants must match. Skips cleanly
// when no ROCm/HIP device is present.
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

// 1.10 — a reachable device yields a measured model with sane fields.
TEST(XpuDeviceProfileAmdDeviceTests, liveQueryPopulatesModel) {
    CAJETA_SKIP_IF_NO_HIP();
    DeviceModel m = queryLiveDeviceModel();
    EXPECT_FALSE(m.estimated) << "a reachable GPU should yield a measured model";
    EXPECT_FALSE(m.archName.empty());
    EXPECT_TRUE(m.waveSize == 32 || m.waveSize == 64);
    EXPECT_GT(m.maxThreadsPerBlock, 0u);
    EXPECT_GT(m.cuCount, 0u) << "multiprocessorCount should be reported";
    EXPECT_GT(m.regsPerMP, 0u);    // live occupancy attr
    EXPECT_GT(m.maxWavesPerMP, 0u);
    EXPECT_GT(m.ldsBytesPerMP, 0u);
}

// 1.10 — on gfx1151 (this box) the specific RDNA3.5 constants match.
TEST(XpuDeviceProfileAmdDeviceTests, gfx1151KnownConstants) {
    CAJETA_SKIP_IF_NO_HIP();
    CajetaXpuRawDevice raw;
    ASSERT_EQ(cajeta_xpu_query_raw_device(&raw), 1);
    if (std::string(raw.archName).rfind("gfx1151", 0) != 0)
        GTEST_SKIP() << "not gfx1151: " << raw.archName;
    DeviceModel m = queryLiveDeviceModel();
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.regsPerMP, 196608u);     // live MaxRegistersPerMultiprocessor
    EXPECT_EQ(m.maxWavesPerMP, 64u);     // 2048 threads / 32 wave
    EXPECT_EQ(m.ldsBytesPerMP, 65536u);
    EXPECT_EQ(m.ldsBankCount, 32u);
    EXPECT_EQ(m.cuCount, 40u);   // Strix Halo: 20 WGPs reported * 2 = 40 CUs
}

// 2.7 — the bandwidth probe returns a plausible device-memory ceiling.
TEST(XpuDeviceProfileAmdDeviceTests, bandwidthProbeMeasures) {
    CAJETA_SKIP_IF_NO_HIP();
    double gbps = cajeta_xpu_measure_bandwidth_gbps(64ull << 20, 3);
    std::cout << "[ device   ] measured device bandwidth: " << gbps << " GB/s\n";
    EXPECT_GT(gbps, 10.0) << "measured " << gbps << " GB/s";
    EXPECT_LT(gbps, 5000.0) << "implausibly high: " << gbps << " GB/s";
}

// 2.7 — the full profile carries a measured roofline on a real device.
TEST(XpuDeviceProfileAmdDeviceTests, profileCarriesRoofline) {
    CAJETA_SKIP_IF_NO_HIP();
    DeviceProfile p = queryLiveDeviceProfile();
    EXPECT_FALSE(p.model.estimated);
    EXPECT_TRUE(p.rooflineMeasured);
    EXPECT_GT(p.bandwidthGBps, 10.0) << "measured " << p.bandwidthGBps << " GB/s";
}

// The Tier-B fields the NVIDIA twin asserts, on HIP. Until 2026-09-30 the HIP
// query left L2, integrated, per-block shared memory and the launch limits at
// 0, so the parity table read every cajeta row's residency as unknown.
TEST(XpuDeviceProfileAmdDeviceTests, tierBGeometryIsQueriedLive) {
    CAJETA_SKIP_IF_NO_HIP();
    const DeviceModel m = queryLiveDeviceModel();
    ASSERT_TRUE(m.queried);
    EXPECT_GT(m.l2CacheBytes, 0u);
    EXPECT_GT(m.ldsBytesPerBlock, 0u);
    EXPECT_LE(m.ldsBytesPerBlock, m.ldsBytesPerMP);
    EXPECT_GT(m.maxGridDimX, 0u);
    EXPECT_GT(m.maxBlockDimX, 0u);
    EXPECT_GT(m.memoryBusWidthBits, 0u);
    EXPECT_GT(m.memoryClockKHz, 0u);
}

// gfx1151 is an APU on 256-bit LPDDR5X-8000: 256 GB/s theoretical. HIP reports
// the LPDDR5 command clock, which read 64 GB/s through the DDR formula.
TEST(XpuDeviceProfileAmdDeviceTests, gfx1151IsAnApuAtItsRealPeak) {
    CAJETA_SKIP_IF_NO_HIP();
    CajetaXpuRawDevice raw;
    ASSERT_EQ(cajeta_xpu_query_raw_device(&raw), 1);
    if (std::string(raw.archName).rfind("gfx1151", 0) != 0)
        GTEST_SKIP() << "not gfx1151: " << raw.archName;
    const DeviceProfile p = queryLiveDeviceProfile();
    EXPECT_TRUE(p.model.integrated);
    EXPECT_NEAR(p.theoreticalBwGBps, 256.0, 1.0);
}

// The ceiling the NVIDIA twin holds: a measured copy never beats the part's
// theoretical bandwidth. 64 GB/s against a measured ~200 failed it.
TEST(XpuDeviceProfileAmdDeviceTests, measuredBandwidthSitsUnderTheoretical) {
    CAJETA_SKIP_IF_NO_HIP();
    const DeviceProfile p = queryLiveDeviceProfile();
    ASSERT_GT(p.theoreticalBwGBps, 0.0);
    if (!p.rooflineMeasured) GTEST_SKIP() << "roofline probe did not run";
    EXPECT_LT(p.bandwidthGBps, 1.05 * p.theoreticalBwGBps)
        << "measured " << p.bandwidthGBps << " GB/s against a theoretical "
        << p.theoreticalBwGBps;
}
