//
// KernelTuner — see header. The lookup chain (cache → guidance → sweep) and the
// shipped guidance database.
//

#include "KernelTuner.h"

namespace cajeta {
namespace xpu {

size_t TuningKeyHash::operator()(const TuningKey& k) const {
    size_t h = std::hash<std::string>{}(k.arch);
    h ^= std::hash<std::string>{}(k.kernel) + 0x9e3779b97f4a7c15ULL
         + (h << 6) + (h >> 2);
    h ^= std::hash<uint64_t>{}(k.shape) + 0x9e3779b97f4a7c15ULL
         + (h << 6) + (h >> 2);
    return h;
}

namespace {

inline bool withinClamp(const TuningConfig& c, unsigned clamp) {
    return clamp == 0 || c.blockX <= clamp;
}

// Shipped guidance database — known best configs authored offline from on-device
// measurements (the hipBLASLt/Tensile model). Seeded with what we have measured;
// grows as the analysis sweep's findings are promoted here. Keyed exactly by
// (arch, kernel, shape).
const std::vector<std::pair<TuningKey, TuningConfig>>& guidanceTable() {
    static const std::vector<std::pair<TuningKey, TuningConfig>> table = {
        // f16 WMMA GEMM on gfx1151: the shipped kernel is a fixed 128-thread tile
        // (measured best across n); recorded as guidance so its launches never
        // pay an analysis sweep.
        {{"gfx1151", "matmul-f16", 0}, {128}},
    };
    return table;
}

} // namespace

std::optional<TuningConfig> KernelTuner::guidance(const TuningKey& key) {
    for (const auto& [k, cfg] : guidanceTable())
        if (k == key) return cfg;
    return std::nullopt;
}

const std::vector<unsigned>& KernelTuner::defaultCandidateBlocks() {
    static const std::vector<unsigned> blocks = {64, 128, 256, 512};
    return blocks;
}

std::optional<TuningConfig> KernelTuner::cached(const TuningKey& key) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

TuningConfig KernelTuner::selectConfig(
        const TuningKey& key,
        const std::vector<TuningConfig>& candidates,
        unsigned maxThreadsClamp,
        const MeasureFn& measure) {
    // 1. Runtime cache — used immediately if it still satisfies the clamp.
    if (auto it = cache_.find(key);
        it != cache_.end() && withinClamp(it->second, maxThreadsClamp)) {
        return it->second;
    }
    // 2. Shipped guidance — no measurement; seeds the cache.
    if (auto g = guidance(key); g && withinClamp(*g, maxThreadsClamp)) {
        cache_[key] = *g;
        return *g;
    }
    // 3. Analysis sweep — time each in-clamp candidate, keep the fastest.
    bool have = false;
    TuningConfig best;
    double bestCost = 0.0;
    for (const auto& c : candidates) {
        if (!withinClamp(c, maxThreadsClamp)) continue;
        double cost = measure(c);
        if (!have || cost < bestCost) { have = true; best = c; bestCost = cost; }
    }
    if (have) {
        cache_[key] = best;
        return best;
    }
    // No in-clamp candidate (e.g. clamp below every default) — fall back to the
    // largest candidate that fits the clamp, else an empty config (caller's
    // launch-site block stands).
    return TuningConfig{};
}

} // namespace xpu
} // namespace cajeta
