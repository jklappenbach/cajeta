//
// CajetaXPU — DeviceProfile machine model (xpu-device-profile U1).
// GPU-free: the per-arch table replaces the gfx1151 hardcoding, and
// buildDeviceModel overlays a live (or absent) device query onto it, flagging
// whether the result is measured or estimated.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/core/DeviceProfile.h"

#include <cstring>

using cajeta::xpu::RawDeviceProps;
using cajeta::xpu::DeviceModel;
using cajeta::xpu::lookupArch;
using cajeta::xpu::defaultDeviceModel;
using cajeta::xpu::buildDeviceModel;

namespace {

RawDeviceProps gfx1151Props() {
    RawDeviceProps p;
    std::strncpy(p.archName, "gfx1151", sizeof(p.archName) - 1);
    p.waveSize = 32;
    p.maxThreadsPerBlock = 1024;
    p.ldsBytesPerBlock = 65536;
    p.multiprocessorCount = 20;   // gfx1151 driver reports 20 WGPs (= 40 CUs)
    p.valid = true;
    return p;
}

} // namespace

// 1.1 — the arch table returns the known gfx1151 constants.
TEST(XpuDeviceProfileTests, archTableKnownGfx1151) {
    DeviceModel m;
    ASSERT_TRUE(lookupArch("gfx1151", m));
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.ldsBytesPerCU, 65536u);
    EXPECT_EQ(m.vgprFilePerSIMD, 1536u);
    EXPECT_EQ(m.ldsBankCount, 32u);
    EXPECT_EQ(m.ldsBankWidth, 4u);
    EXPECT_EQ(m.simdsPerCU, 2u);
}

// 1.1b — a trailing feature suffix on the arch token still resolves.
TEST(XpuDeviceProfileTests, archTableToleratesSuffix) {
    DeviceModel m;
    EXPECT_TRUE(lookupArch("gfx1151:sramecc-:xnack-", m));
    EXPECT_EQ(m.vgprFilePerSIMD, 1536u);
}

// 1.2 — an unknown arch is a miss: the model is left at conservative defaults.
TEST(XpuDeviceProfileTests, archTableUnknownIsMiss) {
    DeviceModel m;
    EXPECT_FALSE(lookupArch("gfx9999", m));
    // unchanged defaults
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.ldsBytesPerCU, 65536u);
}

// 1.3 — buildDeviceModel overlays a valid live query onto a known arch and
// reports a measured (not estimated) model.
TEST(XpuDeviceProfileTests, buildFromLivePropsKnownArch) {
    DeviceModel m = buildDeviceModel(gfx1151Props());
    EXPECT_FALSE(m.estimated);
    EXPECT_EQ(m.archName, "gfx1151");
    EXPECT_EQ(m.waveSize, 32u);          // from the query
    EXPECT_EQ(m.maxThreadsPerBlock, 1024u);
    EXPECT_EQ(m.cuCount, 40u);           // 20 WGPs * 2 = 40 physical CUs
    EXPECT_EQ(m.vgprFilePerSIMD, 1536u); // arch table (driver does not expose)
    EXPECT_EQ(m.ldsBankCount, 32u);
}

// 1.3b — a valid query whose driver fields differ from the arch baseline takes
// the live values for the fields the driver actually reports.
TEST(XpuDeviceProfileTests, livePropsOverrideDriverReportedFields) {
    RawDeviceProps p = gfx1151Props();
    p.waveSize = 64;            // hypothetical wave64 report
    p.maxThreadsPerBlock = 512;
    p.multiprocessorCount = 16;
    DeviceModel m = buildDeviceModel(p);
    EXPECT_EQ(m.waveSize, 64u);
    EXPECT_EQ(m.maxThreadsPerBlock, 512u);
    EXPECT_EQ(m.cuCount, 32u);   // 16 WGPs * 2 (gfx1151 factor)
    EXPECT_FALSE(m.estimated);
}

// 1.4 — an invalid query (failed / profiling disabled) yields the conservative
// default model, flagged estimated; no field is garbage.
TEST(XpuDeviceProfileTests, invalidQueryYieldsEstimatedDefault) {
    RawDeviceProps p;   // valid == false
    DeviceModel m = buildDeviceModel(p);
    EXPECT_TRUE(m.estimated);
    DeviceModel def = defaultDeviceModel();
    EXPECT_EQ(m.waveSize, def.waveSize);
    EXPECT_EQ(m.ldsBytesPerCU, def.ldsBytesPerCU);
    EXPECT_EQ(m.vgprFilePerSIMD, def.vgprFilePerSIMD);
    EXPECT_TRUE(def.estimated);
}

// 1.4b — a valid query for an UNKNOWN arch is still estimated (we lack the
// arch-derived constants) but keeps the driver-reported fields.
TEST(XpuDeviceProfileTests, validQueryUnknownArchIsEstimated) {
    RawDeviceProps p = gfx1151Props();
    std::strncpy(p.archName, "gfx9999", sizeof(p.archName) - 1);
    p.multiprocessorCount = 99;
    DeviceModel m = buildDeviceModel(p);
    EXPECT_TRUE(m.estimated);
    EXPECT_EQ(m.cuCount, 99u);   // unknown arch -> factor 1, raw count kept
    EXPECT_EQ(m.archName, "gfx9999");
}
