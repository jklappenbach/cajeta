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
