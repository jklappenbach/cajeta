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

    // Fallback occupancy constants per GPU family, used ONLY when the live query
    // can't supply them (the driver reports regsPerMP / threadsPerMP / ldsPerMP
    // directly on any queryable device — see buildDeviceModel). The bank geometry
    // and the CU-per-WGP factor are the only genuinely arch-only values.
    // gfx11xx (RDNA3/3.5) per WGP: 196608 VGPRs, 64 waves, 64 KB LDS, 32 banks x 4 B.
    struct ArchRow {
        const char* name;
        unsigned    waveSize;
        unsigned    maxThreadsPerBlock;
        unsigned    regsPerMP;
        unsigned    maxWavesPerMP;
        unsigned    ldsBytesPerMP;
        unsigned    ldsBankCount;
        unsigned    ldsBankWidth;
        unsigned    cuPerMultiprocessor;   // physical CUs per driver multiprocessor
    };

    constexpr std::array<ArchRow, 2> kArchTable = {{
        // gfx1151 (RDNA3.5, Strix Halo) — verified on-device (40 CU = 20 WGP).
        {"gfx1151", 32, 1024, 196608, 64, 65536, 32, 4, 2},
        // gfx1100 (RDNA3 discrete) — same wave32 geometry; CU count is per-SKU.
        {"gfx1100", 32, 1024, 196608, 64, 65536, 32, 4, 2},
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
            out.archName            = token;
            out.waveSize            = row.waveSize;
            out.maxThreadsPerBlock  = row.maxThreadsPerBlock;
            out.regsPerMP           = row.regsPerMP;
            out.maxWavesPerMP       = row.maxWavesPerMP;
            out.ldsBytesPerMP       = row.ldsBytesPerMP;
            out.ldsBankCount        = row.ldsBankCount;
            out.ldsBankWidth        = row.ldsBankWidth;
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

    // Overlay what the driver reports live (trusted over the table). The occupancy
    // inputs (register file, wave cap, LDS) come straight from hipDeviceGetAttribute
    // when present — that is what lets an UNKNOWN-arch device be modeled without a
    // table row. CU count is the multiprocessor count scaled by the arch's CUs/WGP
    // (RDNA reports WGPs = CUs/2); unknown arch -> factor 1 (raw multiprocessors).
    bool liveOccupancy = false;
    if (props.valid) {
        if (props.waveSize)           m.waveSize = props.waveSize;
        if (props.maxThreadsPerBlock) m.maxThreadsPerBlock = props.maxThreadsPerBlock;
        if (props.regsPerMP)          m.regsPerMP = props.regsPerMP;
        if (props.threadsPerMP && m.waveSize)
            m.maxWavesPerMP = props.threadsPerMP / m.waveSize;
        if (props.ldsBytesPerMP)      m.ldsBytesPerMP = props.ldsBytesPerMP;
        liveOccupancy = props.regsPerMP && props.threadsPerMP;
        if (props.multiprocessorCount) {
            m.mpCount = props.multiprocessorCount;
            m.cuCount = props.multiprocessorCount *
                        (archKnown ? m.cuPerMultiprocessor : 1u);
        }
        // Tier-B geometry passes through verbatim. Nothing is substituted when
        // a runtime does not report a fact: 0 means "unknown", and every
        // consumer is written to say so rather than to fall back to the other
        // vendor's number.
        m.ldsBytesPerBlock      = props.ldsBytesPerBlock;
        m.ldsBytesPerBlockOptin = props.ldsBytesPerBlockOptin;
        m.maxBlocksPerMP        = props.maxBlocksPerMP;
        m.l2CacheBytes          = props.l2CacheBytes;
        m.memoryClockKHz        = props.memoryClockKHz;
        m.memoryBusWidthBits    = props.memoryBusWidthBits;
        m.clockRateKHz          = props.clockRateKHz;
        m.maxGridDimX           = props.maxGridDimX;
        m.maxBlockDimX          = props.maxBlockDimX;
        m.totalGlobalMemBytes   = props.totalGlobalMemBytes;
        m.integrated            = props.integrated;
        // The shared derivation in cajeta_xpu_abi.h, so the runtime serving
        // cajeta.xpu.Device and this model cannot drift apart. 0 = the arch is
        // neither spelling; keep the table/default value in that case.
        if (unsigned s = cajeta_xpu_simds_per_mp(m.archName.c_str()))
            m.simdsPerMP = s;
    }

    m.queried = props.valid;
    // Modelable (not estimated) when a real device gave us the occupancy inputs,
    // either from a known arch row OR live attributes (the portable path).
    m.estimated = !(props.valid && (archKnown || liveOccupancy));
    return m;
}

// Spec §3 L2. AMD's per-block ceiling IS its per-MP budget, and HIP's per-block
// ordinal is unverified from this box (spec Q1), so an unreported ceiling falls
// back to the per-MP figure — which is the number AMD has always effectively
// used. NVIDIA reports both and they differ by roughly half; reading
// ldsBytesPerMP as "what one block may use" is right on AMD only by accident.
unsigned ldsCeilingPerBlock(const DeviceModel& m) {
    return m.ldsBytesPerBlock ? m.ldsBytesPerBlock : m.ldsBytesPerMP;
}

// Spec §3 L1 — the dispatch law. The device half is every SIMD on the part;
// the kernel half is how many waves one block puts on one SIMD. Reproduces
// gfx1151's frozen TARGET_BLOCKS = 80 at wavesPerBlock = 2 (20 WGP x 8 SIMD /
// 2), and yields 256 on an sm_89 (128 SM x 4 / 2).
//
// This is a SATURATION target, not an occupancy maximum: Linear.cajeta:167
// records that waves past this point evicted each other from L1/L2, so the
// cache footprint — not residency — is what bounds it.
//
// 0 when the model was never queried or the kernel half is unknown. A guess
// here would be indistinguishable from a measurement, which is precisely the
// failure mode the gfx1151 defaults had.
unsigned dispatchBlocks(const DeviceModel& m, unsigned wavesPerBlock) {
    if (!m.queried || wavesPerBlock == 0) return 0;
    if (m.mpCount == 0 || m.simdsPerMP == 0) return 0;
    return (m.mpCount * m.simdsPerMP) / wavesPerBlock;
}

// Spec §3 L4. The reported memory clock is the DDR half-rate, so the factor of
// two is the data rate, not a fudge: 2 x 10.501 GHz x 384/8 = 1008.096 GB/s,
// which is NVIDIA's published 4090 figure exactly. Existing beside the measured
// probe is the point — one number alone cannot tell a slow probe from a slow
// part. 0.0 when either attribute is absent.
double theoreticalGBps(unsigned busWidthBits, unsigned memClockKHz) {
    if (busWidthBits == 0 || memClockKHz == 0) return 0.0;
    return 2.0 * (double) memClockKHz * 1.0e3 * ((double) busWidthBits / 8.0) / 1.0e9;
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
            props.multiprocessorCount = raw.multiprocessorCount;
            props.regsPerMP           = raw.regsPerMP;
            props.threadsPerMP        = raw.threadsPerMP;
            props.ldsBytesPerMP       = raw.ldsBytesPerMP;
            props.ldsBytesPerBlock      = raw.ldsBytesPerBlock;
            props.ldsBytesPerBlockOptin = raw.ldsBytesPerBlockOptin;
            props.maxBlocksPerMP        = raw.maxBlocksPerMP;
            props.l2CacheBytes          = raw.l2CacheBytes;
            props.memoryClockKHz        = raw.memoryClockKHz;
            props.memoryBusWidthBits    = raw.memoryBusWidthBits;
            props.clockRateKHz          = raw.clockRateKHz;
            props.maxGridDimX           = raw.maxGridDimX;
            props.maxBlockDimX          = raw.maxBlockDimX;
            props.totalGlobalMemBytes   = raw.totalGlobalMemBytes;
            props.integrated            = raw.integrated != 0;
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
        // Derived from attributes alone, so it exists even when the probe is
        // skipped — and cross-checks the probe when both are present.
        p.theoreticalBwGBps = theoreticalGBps(p.model.memoryBusWidthBits,
                                              p.model.memoryClockKHz);
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
    unsigned wavesPerBlock = (block + m.waveSize - 1) / m.waveSize;
    if (wavesPerBlock == 0) return 0;

    // Per-multiprocessor limits, all topology-free. A wave consumes
    // kernelVgpr * waveSize 32-bit registers; the MP holds regsPerMP of them.
    unsigned wavesByReg = kernelVgpr == 0
        ? m.maxWavesPerMP
        : m.regsPerMP / (kernelVgpr * m.waveSize);
    unsigned blocksByReg = wavesByReg / wavesPerBlock;
    unsigned blocksByWave = m.maxWavesPerMP / wavesPerBlock;
    // The per-BLOCK ceiling is a hard rejection, not a divisor: a tile larger
    // than one block may hold does not "fit fewer blocks", it fails to
    // assemble. Ada's 55296-byte coop tile fits the SM's 102400-byte budget
    // and still draws `ptxas error: uses too much shared data (0xd800, 0xc000
    // max)`. Modelling only the per-MP figure cannot tell those apart.
    if (ldsBytes != 0 && ldsBytes > ldsCeilingPerBlock(m)) return 0;
    unsigned blocksByLds = ldsBytes == 0
        ? blocksByWave
        : m.ldsBytesPerMP / std::max(ldsBytes, 1u);
    unsigned blocks = std::min({blocksByReg, blocksByWave, blocksByLds});
    // A reported resident-block cap (24 on Ada) is a fourth limiter. 0 means
    // the runtime did not report one, so no clamp — AMD is unaffected.
    if (m.maxBlocksPerMP) blocks = std::min(blocks, m.maxBlocksPerMP);
    if (blocks == 0) return 0;   // does not fit -> pruned
    return std::min(m.maxWavesPerMP, blocks * wavesPerBlock);
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
    p.needsSweep = shouldSweep(profile.model);
    return p;
}

bool shouldSweep(const DeviceModel& m) {
    return m.queried && m.estimated;   // queryable but unknown arch -> measure
}

unsigned sweepBlocks(const std::vector<unsigned>& candidates,
                     const std::function<double(unsigned)>& timeBlock) {
    unsigned best = 0;
    double bestTime = 0.0;
    for (unsigned block : candidates) {
        double t = timeBlock(block);
        if (t <= 0.0) continue;
        if (best == 0 || t < bestTime) { best = block; bestTime = t; }
    }
    return best;
}

std::string formatDeviceProfileJson(const DeviceProfile& p) {
    std::ostringstream o;
    o << "{\"arch\":\"" << p.model.archName << "\""
      << ",\"cu\":" << p.model.cuCount
      << ",\"wave_size\":" << p.model.waveSize
      << ",\"regs_per_mp\":" << p.model.regsPerMP
      << ",\"max_waves_per_mp\":" << p.model.maxWavesPerMP
      << ",\"lds_bytes_per_mp\":" << p.model.ldsBytesPerMP
      << ",\"lds_bytes_per_block\":" << p.model.ldsBytesPerBlock
      << ",\"lds_bytes_per_block_optin\":" << p.model.ldsBytesPerBlockOptin
      << ",\"max_blocks_per_mp\":" << p.model.maxBlocksPerMP
      << ",\"simds_per_mp\":" << p.model.simdsPerMP
      << ",\"l2_cache_bytes\":" << p.model.l2CacheBytes
      << ",\"total_vram_bytes\":" << p.model.totalGlobalMemBytes
      << ",\"integrated\":" << (p.model.integrated ? "true" : "false")
      << ",\"estimated\":" << (p.model.estimated ? "true" : "false")
      << ",\"roofline_measured\":" << (p.rooflineMeasured ? "true" : "false");
    if (p.rooflineMeasured) o << ",\"bandwidth_gbps\":" << p.bandwidthGBps;
    // The attribute-derived ceiling, so a probe that silently under-reports is
    // distinguishable from a part that is simply slow.
    const double theo = theoreticalGBps(p.model.memoryBusWidthBits,
                                        p.model.memoryClockKHz);
    if (theo > 0.0) o << ",\"theoretical_bandwidth_gbps\":" << theo;
    o << "}";
    return o.str();
}

} // namespace xpu
} // namespace cajeta
