//
// DeviceProfile — see header. The per-arch constant table + the live-query
// overlay that builds the machine model.
//

#include "DeviceProfile.h"

#include "cajeta_xpu_abi.h"   // CajetaXpuRawDevice — the live runtime query

#include <array>
#include <cstring>

namespace cajeta {
namespace xpu {

namespace {

    // Arch-derived constants the driver does not expose. One row per GPU family;
    // adding a part is a row, not a code change. gfx11xx are RDNA3/3.5 wave32:
    // 1536 VGPR/SIMD, 2 SIMD/CU, 16 waves/SIMD, 64 KB LDS/CU, 32 banks x 4 B.
    struct ArchRow {
        const char* name;
        unsigned    waveSize;
        unsigned    maxThreadsPerBlock;
        unsigned    maxWavesPerCU;
        unsigned    simdsPerCU;
        unsigned    vgprFilePerSIMD;
        unsigned    maxWavesPerSIMD;
        unsigned    ldsBytesPerCU;
        unsigned    ldsBankCount;
        unsigned    ldsBankWidth;
        unsigned    cuPerMultiprocessor;   // physical CUs per driver multiprocessor
    };

    constexpr std::array<ArchRow, 2> kArchTable = {{
        // gfx1151 (RDNA3.5, Strix Halo) — verified on-device (40 CU = 20 WGP).
        {"gfx1151", 32, 1024, 32, 2, 1536, 16, 65536, 32, 4, 2},
        // gfx1100 (RDNA3 discrete) — same wave32 geometry; CU count is per-SKU.
        {"gfx1100", 32, 1024, 32, 2, 1536, 16, 65536, 32, 4, 2},
    }};

    // The leading arch token, dropping any ":feature" suffix the driver appends.
    std::string archToken(const std::string& archName) {
        auto colon = archName.find(':');
        return colon == std::string::npos ? archName : archName.substr(0, colon);
    }

} // namespace

bool lookupArch(const std::string& archName, DeviceModel& out) {
    const std::string token = archToken(archName);
    for (const auto& row : kArchTable) {
        if (token == row.name) {
            out.archName           = token;
            out.waveSize           = row.waveSize;
            out.maxThreadsPerBlock = row.maxThreadsPerBlock;
            out.maxWavesPerCU      = row.maxWavesPerCU;
            out.simdsPerCU         = row.simdsPerCU;
            out.vgprFilePerSIMD    = row.vgprFilePerSIMD;
            out.maxWavesPerSIMD    = row.maxWavesPerSIMD;
            out.ldsBytesPerCU      = row.ldsBytesPerCU;
            out.ldsBankCount       = row.ldsBankCount;
            out.ldsBankWidth       = row.ldsBankWidth;
            out.cuPerMultiprocessor = row.cuPerMultiprocessor;
            return true;
        }
    }
    return false;
}

DeviceModel defaultDeviceModel() {
    return DeviceModel{};   // conservative baseline, estimated == true
}

DeviceModel buildDeviceModel(const RawDeviceProps& props) {
    DeviceModel m = defaultDeviceModel();
    if (props.archName[0] != '\0') m.archName = props.archName;

    const bool archKnown = lookupArch(m.archName, m);

    // Overlay the fields the driver actually reports (trusted over the table).
    // The physical CU count is the driver multiprocessor count scaled by the
    // arch's CUs-per-multiprocessor (RDNA reports WGPs = CUs/2); for an unknown
    // arch the factor is 1, so cuCount degrades to the raw multiprocessor count.
    if (props.valid) {
        if (props.waveSize)           m.waveSize = props.waveSize;
        if (props.maxThreadsPerBlock) m.maxThreadsPerBlock = props.maxThreadsPerBlock;
        if (props.multiprocessorCount)
            m.cuCount = props.multiprocessorCount *
                        (archKnown ? m.cuPerMultiprocessor : 1u);
    }

    m.estimated = !(props.valid && archKnown);
    return m;
}

DeviceModel queryLiveDeviceModel() {
    // Once per process: the query (and, later, the roofline probe) is cached.
    static const DeviceModel cached = [] {
        RawDeviceProps props;
        CajetaXpuRawDevice raw;
        if (cajeta_xpu_query_raw_device(&raw) && raw.valid) {
            std::strncpy(props.archName, raw.archName, sizeof(props.archName) - 1);
            props.waveSize            = raw.waveSize;
            props.maxThreadsPerBlock  = raw.maxThreadsPerBlock;
            props.ldsBytesPerBlock    = raw.ldsBytesPerBlock;
            props.multiprocessorCount = raw.multiprocessorCount;
            props.valid               = true;
        }
        return buildDeviceModel(props);
    }();
    return cached;
}

} // namespace xpu
} // namespace cajeta
