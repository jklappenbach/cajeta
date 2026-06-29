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

#include <string>

using cajeta::xpu::DeviceModel;
using cajeta::xpu::queryLiveDeviceModel;

// 1.10 — a reachable device yields a measured model with sane fields.
TEST(XpuDeviceProfileAmdDeviceTests, liveQueryPopulatesModel) {
    CAJETA_SKIP_IF_NO_HIP();
    DeviceModel m = queryLiveDeviceModel();
    EXPECT_FALSE(m.estimated) << "a reachable GPU should yield a measured model";
    EXPECT_FALSE(m.archName.empty());
    EXPECT_TRUE(m.waveSize == 32 || m.waveSize == 64);
    EXPECT_GT(m.maxThreadsPerBlock, 0u);
    EXPECT_GT(m.cuCount, 0u) << "multiprocessorCount should be reported";
    EXPECT_GT(m.vgprFilePerSIMD, 0u);   // arch-table derived
    EXPECT_GT(m.ldsBytesPerCU, 0u);
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
    EXPECT_EQ(m.vgprFilePerSIMD, 1536u);
    EXPECT_EQ(m.ldsBytesPerCU, 65536u);
    EXPECT_EQ(m.ldsBankCount, 32u);
    EXPECT_EQ(m.cuCount, 40u);   // Strix Halo: 20 WGPs reported * 2 = 40 CUs
}
