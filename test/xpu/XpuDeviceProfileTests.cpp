//
// CajetaXPU — DeviceProfile machine model (xpu-device-profile U1).
// GPU-free: the per-arch table replaces the gfx1151 hardcoding, and
// buildDeviceModel overlays a live (or absent) device query onto it, flagging
// whether the result is measured or estimated.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/core/DeviceProfile.h"

#include <cstdlib>
#include <cstring>

using cajeta::xpu::RawDeviceProps;
using cajeta::xpu::DeviceModel;
using cajeta::xpu::lookupArch;
using cajeta::xpu::defaultDeviceModel;
using cajeta::xpu::buildDeviceModel;
using cajeta::xpu::bandwidthProbeParams;
using cajeta::xpu::shouldProbeRoofline;
using cajeta::xpu::achievedGBps;
using cajeta::xpu::classifyBound;
using cajeta::xpu::Bound;
using cajeta::xpu::DeviceProfile;
using cajeta::xpu::formatDeviceProfileJson;
using cajeta::xpu::occupancy;
using cajeta::xpu::candidateBlocks;
using cajeta::xpu::pickLaunch;
using cajeta::xpu::LaunchPick;
using cajeta::xpu::shouldSweep;
using cajeta::xpu::sweepBlocks;

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

// 2.1 — roofline math: GB/s derivation and bound classification.
TEST(XpuDeviceProfileTests, rooflineMath) {
    EXPECT_DOUBLE_EQ(achievedGBps(2000, 1000.0), 2.0);   // bytes/ns == GB/s
    EXPECT_DOUBLE_EQ(achievedGBps(1000, 0.0), 0.0);      // guard

    // ridge = peakGFLOPs/bw = 40000/200 = 200 FLOP/byte.
    EXPECT_EQ(classifyBound(2e9, 1e9, 200.0, 40000.0), Bound::Memory);   // AI 2
    EXPECT_EQ(classifyBound(1e9, 1.0, 200.0, 40000.0), Bound::Compute);  // AI 1e9
    EXPECT_EQ(classifyBound(1e9, 1e9, 200.0, 0.0), Bound::Unknown);      // no peak
}

// 2.2 — bandwidth probe sizing parses + clamps the environment.
TEST(XpuDeviceProfileTests, bandwidthProbeParamsClamp) {
    ::unsetenv("CAJETA_XPU_PROFILE_BW_BYTES");
    ::unsetenv("CAJETA_XPU_PROFILE_BW_PASSES");
    auto def = bandwidthProbeParams();
    EXPECT_EQ(def.bytes, 256ull << 20);
    EXPECT_EQ(def.passes, 5u);

    ::setenv("CAJETA_XPU_PROFILE_BW_BYTES", "1000", 1);     // below 1 MB floor
    ::setenv("CAJETA_XPU_PROFILE_BW_PASSES", "999", 1);     // above 50 ceiling
    auto clamped = bandwidthProbeParams();
    EXPECT_EQ(clamped.bytes, 1ull << 20);
    EXPECT_EQ(clamped.passes, 50u);
    ::unsetenv("CAJETA_XPU_PROFILE_BW_BYTES");
    ::unsetenv("CAJETA_XPU_PROFILE_BW_PASSES");
}

// 2.3 — the roofline probe is skipped when disabled or for an estimated model.
TEST(XpuDeviceProfileTests, shouldProbeRooflineGate) {
    DeviceModel measured = buildDeviceModel([] {
        RawDeviceProps p; std::strncpy(p.archName, "gfx1151", 8);
        p.waveSize = 32; p.maxThreadsPerBlock = 1024; p.multiprocessorCount = 20;
        p.valid = true; return p;
    }());
    ASSERT_FALSE(measured.estimated);

    ::unsetenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
    EXPECT_TRUE(shouldProbeRoofline(measured));
    EXPECT_FALSE(shouldProbeRoofline(defaultDeviceModel()));   // estimated

    ::setenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE", "1", 1);
    EXPECT_FALSE(shouldProbeRoofline(measured));
    ::unsetenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
}

// 3.3 — the gpu-profile JSON carries the measured ceiling; absent when unmeasured.
TEST(XpuDeviceProfileTests, deviceProfileJson) {
    DeviceProfile p;
    p.model = buildDeviceModel(gfx1151Props());
    p.bandwidthGBps = 207.9;
    p.rooflineMeasured = true;
    std::string j = formatDeviceProfileJson(p);
    EXPECT_NE(j.find("\"arch\":\"gfx1151\""), std::string::npos);
    EXPECT_NE(j.find("\"cu\":40"), std::string::npos);
    EXPECT_NE(j.find("\"roofline_measured\":true"), std::string::npos);
    EXPECT_NE(j.find("\"bandwidth_gbps\":207.9"), std::string::npos);

    DeviceProfile est;   // estimated default, no roofline
    std::string je = formatDeviceProfileJson(est);
    EXPECT_NE(je.find("\"estimated\":true"), std::string::npos);
    EXPECT_NE(je.find("\"roofline_measured\":false"), std::string::npos);
    EXPECT_EQ(je.find("bandwidth_gbps"), std::string::npos);   // omitted
}

// 4.1 — closed-form occupancy matches a hand computation; a config that cannot
// fit the register budget reports 0.
TEST(XpuDeviceProfileTests, occupancyClosedForm) {
    DeviceModel m = buildDeviceModel(gfx1151Props());
    // block 256 (8 waves/WG), 128 VGPR/thread: wavesPerSIMD=min(16,1536/128=12)=12,
    // per-CU 24, wg=24/8=3, occ=min(32, 3*8)=24.
    EXPECT_EQ(occupancy(m, 256, 128, 0), 24u);
    // block 1024 (32 waves), 256 VGPR: per-CU 12 waves -> 12/32 = 0 -> does not fit.
    EXPECT_EQ(occupancy(m, 1024, 256, 0), 0u);
}

// 4.2 — candidateBlocks is feasible, best-first, and honors the clamp.
TEST(XpuDeviceProfileTests, candidateBlocksOrderedAndClamped) {
    DeviceModel m = buildDeviceModel(gfx1151Props());
    auto all = candidateBlocks(m, 128, 0, 0);
    ASSERT_FALSE(all.empty());
    for (size_t i = 1; i < all.size(); ++i)        // non-increasing occupancy
        EXPECT_GE(occupancy(m, all[i - 1], 128, 0), occupancy(m, all[i], 128, 0));
    auto clamped = candidateBlocks(m, 128, 0, 128);
    for (unsigned b : clamped) EXPECT_LE(b, 128u);
}

// 4.3 — a low-arithmetic-intensity launch is memory-bound -> geometry won't help;
// without a peak-FLOP ceiling (the deferred probe) the bound is Unknown.
TEST(XpuDeviceProfileTests, pickerMemoryBound) {
    DeviceProfile p;
    p.model = buildDeviceModel(gfx1151Props());
    p.bandwidthGBps = 200.0;
    p.peakGFLOPs = 40000.0;          // ridge = 200 FLOP/byte
    p.rooflineMeasured = true;

    LaunchPick mem = pickLaunch(p, 128, 0, /*flops*/2e9, /*bytes*/1e9);   // AI 2
    EXPECT_GT(mem.block, 0u);
    EXPECT_EQ(mem.bound, cajeta::xpu::Bound::Memory);
    EXPECT_TRUE(mem.geometryWontHelp);

    p.peakGFLOPs = 0.0;              // peak unknown (deferred probe)
    LaunchPick unk = pickLaunch(p, 128, 0, 2e9, 1e9);
    EXPECT_EQ(unk.bound, cajeta::xpu::Bound::Unknown);
    EXPECT_FALSE(unk.geometryWontHelp);
}

// 4.4 — an @Occupancy/§2 clamp caps the picked block; a fixed-geometry kernel is
// flagged advisory (the pick is informational, not to be applied).
TEST(XpuDeviceProfileTests, pickerClampAndAdvisory) {
    DeviceProfile p;
    p.model = buildDeviceModel(gfx1151Props());
    LaunchPick clamped = pickLaunch(p, 64, 0, 0.0, 0.0, /*clamp*/128);
    EXPECT_LE(clamped.block, 128u);
    EXPECT_GT(clamped.block, 0u);

    LaunchPick fixed = pickLaunch(p, 64, 0, 0.0, 0.0, 0, /*fixedGeometry*/true);
    EXPECT_TRUE(fixed.advisoryOnly);
}

// U6 — the bounded sweep fires ONLY for a queried-but-unknown-arch device; a
// known device and a non-queried (no-GPU) device never sweep.
TEST(XpuDeviceProfileTests, shouldSweepOnlyWhenUnmodelable) {
    DeviceModel known = buildDeviceModel(gfx1151Props());     // queried + known
    EXPECT_FALSE(shouldSweep(known));

    RawDeviceProps up = gfx1151Props();
    std::strncpy(up.archName, "gfx9999", sizeof(up.archName) - 1);
    DeviceModel unknown = buildDeviceModel(up);               // queried + unknown
    EXPECT_TRUE(shouldSweep(unknown));

    DeviceModel none = buildDeviceModel(RawDeviceProps{});    // not queried
    EXPECT_FALSE(shouldSweep(none));
}

// U6 — the picker flags needsSweep for an unmodelable device, and the empirical
// sweep returns the fastest-timed candidate from an injected timer.
TEST(XpuDeviceProfileTests, sweepPicksFastestCandidate) {
    RawDeviceProps up = gfx1151Props();
    std::strncpy(up.archName, "gfx9999", sizeof(up.archName) - 1);
    DeviceProfile p;
    p.model = buildDeviceModel(up);
    LaunchPick pick = pickLaunch(p, 64, 0, 0.0, 0.0);
    EXPECT_TRUE(pick.needsSweep);

    // injected timer: a V whose minimum (fastest) is at block 256.
    auto timer = [](unsigned block) {
        return 1.0 + (block > 256 ? block - 256 : 256 - block);
    };
    EXPECT_EQ(sweepBlocks({64, 128, 256, 512}, timer), 256u);
    EXPECT_EQ(sweepBlocks({}, timer), 0u);                    // nothing to sweep
}
