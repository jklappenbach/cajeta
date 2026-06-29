//
// DeviceProfile — see header. The per-arch constant table + the live-query
// overlay that builds the machine model.
//

#include "DeviceProfile.h"

#include "cajeta_xpu_abi.h"   // CajetaXpuRawDevice — the live runtime query

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>

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

namespace {
    // Parse a non-negative integer env var; 0 / unset / malformed -> fallback.
    uint64_t envU64(const char* name, uint64_t fallback) {
        const char* v = std::getenv(name);
        if (!v || !*v) return fallback;
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(v, &end, 10);
        if (end == v || parsed == 0) return fallback;
        return (uint64_t) parsed;
    }
} // namespace

BandwidthProbeParams bandwidthProbeParams() {
    uint64_t bytes  = envU64("CAJETA_XPU_PROFILE_BW_BYTES", 256ull << 20);
    uint64_t passes = envU64("CAJETA_XPU_PROFILE_BW_PASSES", 5);
    if (bytes  < (1ull << 20)) bytes  = 1ull << 20;     // >= 1 MB
    if (bytes  > (2ull << 30)) bytes  = 2ull << 30;     // <= 2 GB
    if (passes < 1)  passes = 1;
    if (passes > 50) passes = 50;
    return { bytes, (unsigned) passes };
}

bool shouldProbeRoofline(const DeviceModel& model) {
    const char* dis = std::getenv("CAJETA_XPU_DEVICE_PROFILE_DISABLE");
    if (dis && dis[0] && dis[0] != '0') return false;
    return !model.estimated;   // only measure a real, identified device
}

double achievedGBps(uint64_t bytesMoved, double nanos) {
    if (nanos <= 0.0) return 0.0;
    return (double) bytesMoved / nanos;   // bytes/ns == GB/s
}

Bound classifyBound(double flops, double bytes, double bwGBps, double peakGFLOPs) {
    if (bwGBps <= 0.0 || peakGFLOPs <= 0.0 || bytes <= 0.0) return Bound::Unknown;
    double ridge = peakGFLOPs / bwGBps;    // FLOP per byte at the knee
    double intensity = flops / bytes;
    return intensity < ridge ? Bound::Memory : Bound::Compute;
}

DeviceProfile queryLiveDeviceProfile() {
    static const DeviceProfile cached = [] {
        DeviceProfile p;
        p.model = queryLiveDeviceModel();
        if (shouldProbeRoofline(p.model)) {
            BandwidthProbeParams bp = bandwidthProbeParams();
            double gbps = cajeta_xpu_measure_bandwidth_gbps(bp.bytes, (int) bp.passes);
            if (gbps > 0.0) { p.bandwidthGBps = gbps; p.rooflineMeasured = true; }
        }
        return p;
    }();
    return cached;
}

unsigned occupancy(const DeviceModel& m, unsigned block,
                   unsigned kernelVgpr, unsigned ldsBytes) {
    if (block == 0 || block > m.maxThreadsPerBlock || m.waveSize == 0) return 0;
    unsigned wavesPerWG = (block + m.waveSize - 1) / m.waveSize;
    if (wavesPerWG == 0) return 0;

    // Register limiter: waves a SIMD holds for the per-thread VGPR demand, capped
    // by the hardware wave limit, scaled to the CU.
    unsigned vg = kernelVgpr == 0 ? 1 : kernelVgpr;
    unsigned wavesPerSIMD = std::min(m.maxWavesPerSIMD, m.vgprFilePerSIMD / vg);
    unsigned wavesPerCUByVgpr = wavesPerSIMD * m.simdsPerCU;

    unsigned wgByVgpr = wavesPerCUByVgpr / wavesPerWG;
    unsigned wgByLds = ldsBytes == 0 ? wgByVgpr
                                     : m.ldsBytesPerCU / std::max(ldsBytes, 1u);
    unsigned wg = std::min(wgByVgpr, wgByLds);
    if (wg == 0) return 0;
    return std::min(m.maxWavesPerCU, wg * wavesPerWG);
}

std::vector<unsigned> candidateBlocks(const DeviceModel& m, unsigned kernelVgpr,
                                      unsigned ldsBytes, unsigned clamp) {
    std::vector<std::pair<unsigned, unsigned>> scored;   // (block, occupancy)
    for (unsigned block = m.waveSize; block <= m.maxThreadsPerBlock;
         block += m.waveSize) {
        if (clamp != 0 && block > clamp) break;
        unsigned occ = occupancy(m, block, kernelVgpr, ldsBytes);
        if (occ == 0) continue;
        scored.push_back({block, occ});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first > b.first;
    });
    std::vector<unsigned> out;
    out.reserve(scored.size());
    for (auto& [block, occ] : scored) out.push_back(block);
    return out;
}

LaunchPick pickLaunch(const DeviceProfile& profile, unsigned kernelVgpr,
                      unsigned ldsBytes, double flops, double bytes,
                      unsigned clamp, bool fixedGeometry) {
    LaunchPick p;
    p.advisoryOnly = fixedGeometry;
    auto blocks = candidateBlocks(profile.model, kernelVgpr, ldsBytes, clamp);
    if (!blocks.empty()) {
        p.block = blocks.front();
        p.occupancyWaves = occupancy(profile.model, p.block, kernelVgpr, ldsBytes);
    }
    double peak = profile.peakGFLOPs;   // 0 (unknown) -> Bound::Unknown
    p.bound = classifyBound(flops, bytes, profile.bandwidthGBps, peak);
    p.geometryWontHelp = (p.bound == Bound::Memory);
    return p;
}

std::string formatDeviceProfileJson(const DeviceProfile& p) {
    std::ostringstream o;
    o << "{\"arch\":\"" << p.model.archName << "\""
      << ",\"cu\":" << p.model.cuCount
      << ",\"wave_size\":" << p.model.waveSize
      << ",\"lds_bytes_per_cu\":" << p.model.ldsBytesPerCU
      << ",\"vgpr_per_simd\":" << p.model.vgprFilePerSIMD
      << ",\"estimated\":" << (p.model.estimated ? "true" : "false")
      << ",\"roofline_measured\":" << (p.rooflineMeasured ? "true" : "false");
    if (p.rooflineMeasured) o << ",\"bandwidth_gbps\":" << p.bandwidthGBps;
    o << "}";
    return o.str();
}

} // namespace xpu
} // namespace cajeta
