//
// DeviceProfile — interrogate the live GPU into an in-memory machine model
// (xpu-device-profile U1). Replaces the deleted DeviceModel's hardcoded gfx1151
// constants with a per-arch table populated PARTLY from a live device query
// (RawDeviceProps: warpSize / maxThreadsPerBlock / sharedMemPerBlock /
// multiProcessorCount) and PARTLY from arch-derived constants the driver does
// not expose (VGPR file per SIMD, LDS bank geometry, max waves/SIMD). Adding a
// GPU is one table row, not a code change. Nothing here is persisted; the
// profile is built once per process, in memory (xpu-device-profile §7).
//
// GPU-free: buildDeviceModel takes the queried facts as plain data, so the
// model + arch table are unit-tested without a device (inject RawDeviceProps).
//

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cajeta {
namespace xpu {

    // Raw device facts a runtime query can read from hipDeviceProp /
    // cudaDeviceProp. POD + a fixed arch buffer so the C runtime can fill it and
    // a test can inject it. `valid` is false when the query failed or profiling
    // is disabled — buildDeviceModel then yields conservative, flagged defaults.
    struct RawDeviceProps {
        char     archName[64]        = {0}; // gcnArchName / cuda name (e.g. "gfx1151")
        unsigned waveSize            = 0;   // warpSize
        unsigned maxThreadsPerBlock  = 0;   // maxThreadsPerBlock
        unsigned ldsBytesPerBlock    = 0;   // sharedMemPerBlock
        unsigned multiprocessorCount = 0;   // RDNA: WGPs (= physical CUs / 2)
        bool     valid               = false; // false -> query failed / disabled
    };

    // The occupancy-relevant machine model. Defaults are a conservative,
    // single-wave-friendly baseline; a known arch + a valid query overwrite them
    // and clear `estimated`.
    struct DeviceModel {
        std::string archName = "unknown";
        unsigned waveSize           = 32;
        unsigned maxThreadsPerBlock = 1024;
        unsigned maxWavesPerCU      = 32;
        unsigned simdsPerCU         = 2;
        unsigned vgprFilePerSIMD    = 1536;
        unsigned maxWavesPerSIMD    = 16;
        unsigned ldsBytesPerCU      = 65536;
        unsigned ldsBankCount       = 32;
        unsigned ldsBankWidth       = 4;
        unsigned cuPerMultiprocessor = 2;    // RDNA WGP = 2 CUs; CDNA/NV = 1
        unsigned cuCount            = 0;     // PHYSICAL CUs = mpCount * cuPerMp
        bool     estimated          = true;  // true until a known arch + valid query
    };

    // Fill `out`'s arch-derived constants for a known arch string; return true on
    // a hit (out left at its defaults + false on a miss). Keyed on the leading
    // "gfxNNNN" / arch token, so trailing feature suffixes do not defeat it.
    bool lookupArch(const std::string& archName, DeviceModel& out);

    // Conservative default model (no device queried / profiling disabled).
    DeviceModel defaultDeviceModel();

    // Build the model: arch table for the static constants, then overlay the
    // live RawDeviceProps. `estimated` is false IFF the query is valid AND the
    // arch is known.
    DeviceModel buildDeviceModel(const RawDeviceProps& props);

    // Query the live device through the runtime (hipDeviceGetAttribute + the
    // gfx-arch scan) and build the model. Yields an estimated default when no
    // GPU is reachable or profiling is disabled.
    DeviceModel queryLiveDeviceModel();

    // The full profile: the machine model plus the measured roofline. A peak
    // FLOP ceiling is left 0 (unknown) until the optional FLOP probe ships;
    // `bandwidthGBps` is the measured device-memory ceiling, the honest
    // denominator for memory-bound throughput.
    struct DeviceProfile {
        DeviceModel model;
        double bandwidthGBps    = 0.0;    // measured; 0 = unmeasured
        double peakGFLOPs       = 0.0;    // 0 = unknown (FLOP probe deferred)
        bool   rooflineMeasured = false;
    };

    // Bandwidth-probe sizing, from the environment, clamped to sane bounds.
    struct BandwidthProbeParams { uint64_t bytes; unsigned passes; };
    BandwidthProbeParams bandwidthProbeParams();

    // True when profiling is disabled (CAJETA_XPU_DEVICE_PROFILE_DISABLE) or the
    // model is not a measured device — i.e. the roofline probe should be skipped.
    bool shouldProbeRoofline(const DeviceModel& model);

    // Roofline math (GPU-free). `bytesMoved`/`nanos` -> GB/s (bytes per ns ==
    // GB/s). classifyBound compares the kernel's arithmetic intensity to the
    // ridge point (peakGFLOPs / bwGBps); Unknown when a ceiling is missing.
    double achievedGBps(uint64_t bytesMoved, double nanos);
    enum class Bound { Memory, Compute, Unknown };
    Bound classifyBound(double flops, double bytes, double bwGBps, double peakGFLOPs);

    // Build the full profile: the live model + (when warranted) the measured
    // bandwidth roofline. Cached once per process; nothing persisted.
    DeviceProfile queryLiveDeviceProfile();

    // Render the profile as a compact one-line JSON object (GPU-free) — the
    // payload of `cajeta gpu-profile`, consumed by env-capture.sh.
    std::string formatDeviceProfileJson(const DeviceProfile& profile);

    // ---- Analytic launch-config picker (spec §4) -------------------------- //
    // Resident waves/CU for `block` threads given the kernel's compiled per-thread
    // VGPR demand and per-workgroup LDS bytes — the closed form (min over the
    // register, LDS, and hardware-wave limiters). 0 means the config does not fit.
    unsigned occupancy(const DeviceModel& m, unsigned block,
                       unsigned kernelVgpr, unsigned ldsBytes);

    // Feasible block sizes (wave multiples that fit the budgets), best-first by
    // predicted occupancy (larger block breaks ties). `clamp` (0 = none) caps the
    // block (an @Occupancy / §2 bound). Empty if nothing fits.
    std::vector<unsigned> candidateBlocks(const DeviceModel& m, unsigned kernelVgpr,
                                          unsigned ldsBytes, unsigned clamp = 0);

    // The picker's verdict for a launch. `block` is the computed occupancy-optimal
    // launch size (0 if nothing fits); `occupancyWaves` its resident waves/CU;
    // `bound` the roofline classification; `geometryWontHelp` is true when the
    // kernel is memory-bound (a geometry change cannot raise throughput — the
    // honest negative). `advisoryOnly` flags that the caller's kernel has fixed,
    // hand-tuned geometry so the pick is informational, not to be applied.
    struct LaunchPick {
        unsigned block = 0;
        unsigned occupancyWaves = 0;
        Bound    bound = Bound::Unknown;
        bool     geometryWontHelp = false;
        bool     advisoryOnly = false;
    };

    // Compute the launch pick from the profile + the kernel's resource demand +
    // the launch's FLOP/byte work. Pure decision logic — no GPU calls. `clamp`
    // caps the block; `fixedGeometry` marks a hand-tuned kernel (pick is advisory).
    LaunchPick pickLaunch(const DeviceProfile& profile, unsigned kernelVgpr,
                          unsigned ldsBytes, double flops, double bytes,
                          unsigned clamp = 0, bool fixedGeometry = false);

} // namespace xpu
} // namespace cajeta
