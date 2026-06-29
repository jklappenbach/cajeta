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

} // namespace xpu
} // namespace cajeta
