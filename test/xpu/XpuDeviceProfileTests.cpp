//
// CajetaXPU — DeviceProfile machine model (xpu-device-profile U1).
// GPU-free: the per-arch table replaces the gfx1151 hardcoding, and
// buildDeviceModel overlays a live (or absent) device query onto it, flagging
// whether the result is measured or estimated.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/core/DeviceProfile.h"
#include "../PortableEnv.h"   // setenv/unsetenv shim (absent on mingw)

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
    p.multiprocessorCount = 20;   // gfx1151 driver reports 20 WGPs (= 40 CUs)
    p.regsPerMP = 196608;         // live occupancy attrs (MaxRegistersPerMP)
    p.threadsPerMP = 2048;        // MaxThreadsPerMultiProcessor (64 waves/MP)
    p.ldsBytesPerMP = 65536;
    p.valid = true;
    return p;
}

// An NVIDIA-shaped device (Ada / sm_89, measured on an RTX 4090). There is no
// arch-table row and there does not need to be: the CUDA driver reports every
// occupancy input live, which is the portable path buildDeviceModel already
// has. The numbers are the ones cuDeviceGetAttribute returns on this box.
RawDeviceProps sm89Props() {
    RawDeviceProps p;
    std::strncpy(p.archName, "sm_89", sizeof(p.archName) - 1);
    p.waveSize = 32;                // warp
    p.maxThreadsPerBlock = 1024;
    p.multiprocessorCount = 128;    // SMs, reported directly (no WGP folding)
    p.regsPerMP = 65536;            // MAX_REGISTERS_PER_MULTIPROCESSOR
    p.threadsPerMP = 1536;          // MAX_THREADS_PER_MULTIPROCESSOR -> 48 warps
    p.ldsBytesPerMP = 102400;       // MAX_SHARED_MEMORY_PER_MULTIPROCESSOR
    p.valid = true;
    return p;
}

// Queryable but UNMODELABLE: a real device responded, but the driver gave no
// occupancy attributes (regs/threads/lds per MP) AND the arch is unknown — the
// only case that warrants the bounded sweep.
RawDeviceProps unmodelableProps() {
    RawDeviceProps p;
    std::strncpy(p.archName, "gfx9999", sizeof(p.archName) - 1);
    p.waveSize = 32;
    p.maxThreadsPerBlock = 1024;
    p.multiprocessorCount = 20;
    p.valid = true;   // regsPerMP / threadsPerMP / ldsBytesPerMP stay 0
    return p;
}

} // namespace

// 1.1 — the arch table returns the known gfx1151 fallback constants.
TEST(XpuDeviceProfileTests, archTableKnownGfx1151) {
    DeviceModel m;
    ASSERT_TRUE(lookupArch("gfx1151", m));
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.regsPerMP, 196608u);
    EXPECT_EQ(m.maxWavesPerMP, 64u);
    EXPECT_EQ(m.ldsBytesPerMP, 65536u);
    EXPECT_EQ(m.ldsBankCount, 32u);
    EXPECT_EQ(m.ldsBankWidth, 4u);
    EXPECT_EQ(m.cuPerMultiprocessor, 2u);
}

// 1.1b — a trailing feature suffix on the arch token still resolves.
TEST(XpuDeviceProfileTests, archTableToleratesSuffix) {
    DeviceModel m;
    EXPECT_TRUE(lookupArch("gfx1151:sramecc-:xnack-", m));
    EXPECT_EQ(m.regsPerMP, 196608u);
}

// 1.2 — an unknown arch is a miss: the model is left at conservative defaults.
TEST(XpuDeviceProfileTests, archTableUnknownIsMiss) {
    DeviceModel m;
    EXPECT_FALSE(lookupArch("gfx9999", m));
    // unchanged defaults
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.ldsBytesPerMP, 65536u);
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
    EXPECT_EQ(m.regsPerMP, 196608u);     // live occupancy attr
    EXPECT_EQ(m.maxWavesPerMP, 64u);     // 2048 threads / 32 wave
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
    EXPECT_EQ(m.ldsBytesPerMP, def.ldsBytesPerMP);
    EXPECT_EQ(m.regsPerMP, def.regsPerMP);
    EXPECT_TRUE(def.estimated);
}

// 1.4b (PIVOT) — a valid query for an UNKNOWN arch is MODELABLE when the driver
// reports the occupancy attributes live: estimated=false, constants from live.
TEST(XpuDeviceProfileTests, validQueryUnknownArchModelableViaLive) {
    RawDeviceProps p = gfx1151Props();   // carries live regs/threads/lds per MP
    std::strncpy(p.archName, "gfx9999", sizeof(p.archName) - 1);
    DeviceModel m = buildDeviceModel(p);
    EXPECT_FALSE(m.estimated) << "live occupancy attrs make an unknown arch modelable";
    EXPECT_EQ(m.regsPerMP, 196608u);     // from the live query, no table row
    EXPECT_EQ(m.maxWavesPerMP, 64u);
    EXPECT_EQ(m.archName, "gfx9999");
}

// 1.4c — a valid query for an unknown arch WITHOUT occupancy attrs stays
// estimated (truly unmodelable) but keeps the driver-reported fields.
TEST(XpuDeviceProfileTests, validQueryUnknownArchNoAttrsEstimated) {
    RawDeviceProps p = unmodelableProps();
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
    EXPECT_NE(j.find("\"regs_per_mp\":196608"), std::string::npos);
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
    // block 256 (8 waves), 128 VGPR/thread: wavesByReg=196608/(128*32)=48,
    // blocksByReg=48/8=6, blocksByWave=64/8=8, blocks=6, occ=min(64,6*8)=48.
    EXPECT_EQ(occupancy(m, 256, 128, 0), 48u);
    // block 1024 (32 waves), 256 VGPR: wavesByReg=196608/(256*32)=24 -> 24/32=0
    // blocks -> does not fit.
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

// U6 (post-pivot) — the bounded sweep fires ONLY when a queried device is truly
// unmodelable (no live occupancy attrs AND unknown arch). A known device, an
// unknown arch WITH live attrs, and a non-queried device all skip the sweep.
TEST(XpuDeviceProfileTests, shouldSweepOnlyWhenUnmodelable) {
    DeviceModel known = buildDeviceModel(gfx1151Props());     // queried + known
    EXPECT_FALSE(shouldSweep(known));

    RawDeviceProps liveUnknown = gfx1151Props();              // live attrs present
    std::strncpy(liveUnknown.archName, "gfx9999", sizeof(liveUnknown.archName) - 1);
    EXPECT_FALSE(shouldSweep(buildDeviceModel(liveUnknown)))
        << "live occupancy attrs make an unknown arch modelable -> no sweep";

    DeviceModel unmodelable = buildDeviceModel(unmodelableProps());  // no attrs
    EXPECT_TRUE(shouldSweep(unmodelable));

    DeviceModel none = buildDeviceModel(RawDeviceProps{});    // not queried
    EXPECT_FALSE(shouldSweep(none));
}

// U6 — the picker flags needsSweep for an unmodelable device, and the empirical
// sweep returns the fastest-timed candidate from an injected timer.
TEST(XpuDeviceProfileTests, sweepPicksFastestCandidate) {
    DeviceProfile p;
    p.model = buildDeviceModel(unmodelableProps());
    LaunchPick pick = pickLaunch(p, 64, 0, 0.0, 0.0);
    EXPECT_TRUE(pick.needsSweep);

    // injected timer: a V whose minimum (fastest) is at block 256.
    auto timer = [](unsigned block) {
        return 1.0 + (block > 256 ? block - 256 : 256 - block);
    };
    EXPECT_EQ(sweepBlocks({64, 128, 256, 512}, timer), 256u);
    EXPECT_EQ(sweepBlocks({}, timer), 0u);                    // nothing to sweep
}

// 1.12 — an NVIDIA device is modelable with NO arch-table row: the live
// occupancy attributes alone take it off the estimated path. This is the whole
// NVIDIA story for the machine model — nothing arch-specific is required.
TEST(XpuDeviceProfileTests, nvidiaLiveAttributesAreModelable) {
    DeviceModel m = buildDeviceModel(sm89Props());
    EXPECT_FALSE(m.estimated) << "live occupancy attrs should make it measured";
    EXPECT_TRUE(m.queried);
    EXPECT_EQ(m.archName, "sm_89");
    EXPECT_EQ(m.waveSize, 32u);
    EXPECT_EQ(m.regsPerMP, 65536u);
    EXPECT_EQ(m.maxWavesPerMP, 48u);      // 1536 threads / 32 per warp
    EXPECT_EQ(m.ldsBytesPerMP, 102400u);
}

// 1.13 — the RDNA "WGP = 2 CUs" doubling must NOT leak onto NVIDIA. An SM is
// the multiprocessor, so the reported count passes through unscaled. The guard
// matters because cuPerMultiprocessor DEFAULTS to 2: only the arch-table hit
// is allowed to apply it, and sm_89 is deliberately not a row.
TEST(XpuDeviceProfileTests, nvidiaMultiprocessorCountIsNotDoubled) {
    DeviceModel m = buildDeviceModel(sm89Props());
    EXPECT_EQ(m.cuCount, 128u) << "an SM is the multiprocessor; 128, not 256";
}

// 1.14 — the regression this whole exercise exists to prevent: an NVIDIA
// device must not be described by the gfx1151-shaped defaults. Pin each field
// the default model would have supplied, so a query that silently stops
// working fails here rather than shipping a plausible-looking wrong profile.
TEST(XpuDeviceProfileTests, nvidiaModelIsNotTheGfx1151Default) {
    DeviceModel d = defaultDeviceModel();
    DeviceModel m = buildDeviceModel(sm89Props());
    EXPECT_NE(m.regsPerMP,      d.regsPerMP);       // 65536 vs 196608
    EXPECT_NE(m.maxWavesPerMP,  d.maxWavesPerMP);   // 48 vs 64
    EXPECT_NE(m.ldsBytesPerMP,  d.ldsBytesPerMP);   // 102400 vs 65536
    EXPECT_NE(m.archName,       d.archName);        // sm_89 vs "unknown"
    EXPECT_NE(m.cuCount,        d.cuCount);         // 128 vs 0
}
