// DeviceProfile — the live GPU as an in-memory machine model, built once per
// process from a device query plus an arch table of constants the driver does
// not expose. Nothing is persisted, and it is testable with no device present.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {

    // Raw device facts, POD so the runtime fills them and a test injects them.
    struct RawDeviceProps {
        char     archName[64]        = {0}; // gcnArchName / cuda name (e.g. "gfx1151")
        unsigned waveSize            = 0;   // warpSize
        unsigned maxThreadsPerBlock  = 0;   // maxThreadsPerBlock
        unsigned multiprocessorCount = 0;   // RDNA: WGPs (= physical CUs / 2)
        // Live occupancy inputs: they model an unknown arch with no table row.
        unsigned regsPerMP           = 0;   // MaxRegistersPerMultiprocessor
        unsigned threadsPerMP        = 0;   // MaxThreadsPerMultiProcessor
        unsigned ldsBytesPerMP       = 0;   // MaxSharedMemoryPerMultiprocessor
        // Live attributes, where 0 means unreported — never a substituted default.
        unsigned ldsBytesPerBlock    = 0;   // per-BLOCK shared cap (CUDA attr 8)
        unsigned ldsBytesPerBlockOptin = 0; // raised cap, opt-in only (attr 97)
        unsigned maxBlocksPerMP      = 0;   // resident block cap  (attr 106)
        unsigned l2CacheBytes        = 0;   // L2 size             (attr 38)
        unsigned memoryClockKHz      = 0;   // memory clock, kHz   (attr 36)
        unsigned memoryBusWidthBits  = 0;   // bus width, bits     (attr 37)
        unsigned clockRateKHz        = 0;   // core clock, kHz     (attr 13)
        unsigned maxGridDimX         = 0;   // grid clamp          (attr 5)
        unsigned maxBlockDimX        = 0;   // block clamp         (attr 2)
        uint64_t totalGlobalMemBytes = 0;   // cuDeviceTotalMem
        bool     integrated          = false; // APU (attr 18): a copy is not a transfer
        bool     valid               = false; // false -> query failed / disabled
    };

    // The occupancy model per multiprocessor (per WGP on RDNA), live or tabled.
    struct DeviceModel {
        std::string archName = "unknown";
        unsigned waveSize           = 32;
        unsigned maxThreadsPerBlock = 1024;
        unsigned regsPerMP          = 196608; // 32-bit VGPRs per multiprocessor
        unsigned maxWavesPerMP      = 64;     // wave residency cap per MP
        unsigned ldsBytesPerMP      = 65536;  // LDS per MP
        unsigned ldsBankCount       = 32;     // arch-only (swizzle/diagnostics)
        unsigned ldsBankWidth       = 4;      // arch-only
        unsigned cuPerMultiprocessor = 2;     // RDNA WGP = 2 CUs; for CU reporting
        unsigned cuCount            = 0;      // PHYSICAL CUs = mpCount * cuPerMp
        unsigned mpCount            = 0;      // driver multiprocessors, UNSCALED
        // An ARCH constant: threadsPerMP/waveSize gives 1.5 on Ada, a cap not a count.
        unsigned simdsPerMP         = 8;
        // The per-BLOCK ceiling a static tile is checked against; 0 = per-MP.
        unsigned ldsBytesPerBlock      = 0;
        unsigned ldsBytesPerBlockOptin = 0;
        unsigned maxBlocksPerMP     = 0;      // 0 = not reported (no clamp)
        unsigned l2CacheBytes       = 0;
        unsigned memoryClockKHz     = 0;
        unsigned memoryBusWidthBits = 0;
        unsigned clockRateKHz       = 0;
        unsigned maxGridDimX        = 0;
        unsigned maxBlockDimX       = 0;
        uint64_t totalGlobalMemBytes = 0;
        bool     integrated         = false;
        bool     queried            = false;  // a real device responded to the query
        bool     estimated          = true;   // true until modelable (live or known arch)
    };

    // The ceiling to size a tile against: the per-block cap, else the per-MP one.
    unsigned ldsCeilingPerBlock(const DeviceModel& m);

    // Blocks filling every SIMD at `wavesPerBlock`; 0 — never a guess — if unknown.
    unsigned dispatchBlocks(const DeviceModel& m, unsigned wavesPerBlock);

    // The roofline the ATTRIBUTES imply (double-data-rate folded in), to
    // cross-check the measured probe; 0.0 on 0 inputs.
    double theoreticalGBps(unsigned busWidthBits, unsigned memClockKHz);

    // Fills `out` for a known arch, keyed on the leading token; false on a miss.
    bool lookupArch(const std::string& archName, DeviceModel& out);

    // Conservative default model (no device queried / profiling disabled).
    DeviceModel defaultDeviceModel();

    // Table constants overlaid with live props; `estimated` false IFF both hold.
    DeviceModel buildDeviceModel(const RawDeviceProps& props);

    // Queries the live device and builds the model, or an estimated default.
    DeviceModel queryLiveDeviceModel();

    // The model plus the measured roofline, memory-bound throughput's denominator.
    struct DeviceProfile {
        DeviceModel model;
        double bandwidthGBps    = 0.0;    // measured; 0 = unmeasured
        double theoreticalBwGBps = 0.0;   // from the attributes; 0 = underivable
        double peakGFLOPs       = 0.0;    // 0 = unknown (FLOP probe deferred)
        bool   rooflineMeasured = false;
    };

    // Bandwidth-probe sizing, from the environment, clamped to sane bounds.
    struct BandwidthProbeParams { uint64_t bytes; unsigned passes; };
    BandwidthProbeParams bandwidthProbeParams();

    // False when profiling is disabled or the model is not a measured device.
    bool shouldProbeRoofline(const DeviceModel& model);

    // Bytes per ns IS GB/s; classifyBound answers Unknown without a ceiling.
    double achievedGBps(uint64_t bytesMoved, double nanos);
    enum class Bound { Memory, Compute, Unknown };
    Bound classifyBound(double flops, double bytes, double bwGBps, double peakGFLOPs);

    // The live model plus, when warranted, the measured roofline; cached once.
    DeviceProfile queryLiveDeviceProfile();

    // The profile as one line of JSON — the `cajeta gpu-profile` payload.
    std::string formatDeviceProfileJson(const DeviceProfile& profile);

    // Resident waves/MP at this block, VGPR and LDS demand; 0 = does not fit.
    unsigned occupancy(const DeviceModel& m, unsigned block,
                       unsigned kernelVgpr, unsigned ldsBytes);

    // Which budget binds occupancy(), by the same arithmetic, kept beside it.
    const char* occupancyLimiterName(const DeviceModel& m, unsigned block,
                                     unsigned kernelVgpr, unsigned ldsBytes);

    // The wave-multiple blocks that fit, best occupancy first; `clamp` 0 = none.
    std::vector<unsigned> candidateBlocks(const DeviceModel& m, unsigned kernelVgpr,
                                          unsigned ldsBytes, unsigned clamp = 0);

    // The picker's verdict: the optimal `block` (0 = nothing fits) and its
    // waves/MP, plus the honest negative that geometry cannot help at all.
    struct LaunchPick {
        unsigned block = 0;
        unsigned occupancyWaves = 0;
        Bound    bound = Bound::Unknown;
        bool     geometryWontHelp = false;
        bool     advisoryOnly = false;
        bool     needsSweep = false;   // unmodelable device: verify `block` empirically
    };

    // Queried but of unknown arch: the ONLY case the bounded sweep is for.
    bool shouldSweep(const DeviceModel& m);

    // Times each candidate through the injected timer and returns the fastest.
    unsigned sweepBlocks(const std::vector<unsigned>& candidates,
                         const std::function<double(unsigned)>& timeBlock);

    // The pick from the profile, demand and work; `fixedGeometry` = advisory.
    LaunchPick pickLaunch(const DeviceProfile& profile, unsigned kernelVgpr,
                          unsigned ldsBytes, double flops, double bytes,
                          unsigned clamp = 0, bool fixedGeometry = false);

} // namespace xpu
} // namespace cajeta
