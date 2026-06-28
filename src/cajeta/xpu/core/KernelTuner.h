//
// KernelTuner — runtime config selection for @Kernel launches
// (kernel-occupancy-autotune §4).
//
// Three-tier lookup so the common path costs nothing and only genuinely-new
// shapes pay to learn:
//   1. runtime cache   — a config already chosen this run for this key
//   2. shipped guidance — a static tuning DB authored offline from measurements
//                         (the hipBLASLt/Tensile model); a hit needs NO measurement
//   3. analysis sweep  — on a miss, time a small candidate set and cache the winner
//
// The selection is pure decision logic with the timer INJECTED (MeasureFn), so it
// is validated GPU-free; the real launch path supplies an on-device HIP-event timer.
//

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {
namespace xpu {

    // The tunable launch logistics. v1 knob is the workgroup (block) size;
    // extensible to grid strategy / shared-memory split without touching the
    // lookup logic.
    struct TuningConfig {
        unsigned blockX = 0;
        bool operator==(const TuningConfig& o) const { return blockX == o.blockX; }
    };

    // (architecture, kernel, problem-shape signature). `shape` buckets the
    // significant launch dimensions so near-identical shapes share a key.
    struct TuningKey {
        std::string arch;
        std::string kernel;
        uint64_t shape = 0;
        bool operator==(const TuningKey& o) const {
            return shape == o.shape && kernel == o.kernel && arch == o.arch;
        }
    };

    struct TuningKeyHash {
        size_t operator()(const TuningKey& k) const;
    };

    class KernelTuner {
    public:
        // Returns a relative cost for a config (lower = faster). Injected so the
        // selection is testable without a GPU; the launcher supplies a real timer.
        using MeasureFn = std::function<double(const TuningConfig&)>;

        // Pick a config for `key` via cache → shipped guidance → analysis sweep.
        // `maxThreadsClamp` (0 = none) is the @Occupancy / §2 bound: candidates and
        // any cache/guidance result with blockX > clamp are rejected. `measure` is
        // called ONLY on the sweep path (a cache/guidance hit never measures).
        TuningConfig selectConfig(const TuningKey& key,
                                  const std::vector<TuningConfig>& candidates,
                                  unsigned maxThreadsClamp,
                                  const MeasureFn& measure);

        // Test/introspection: the runtime-cache entry for `key`, if any.
        std::optional<TuningConfig> cached(const TuningKey& key) const;

        // Shipped guidance DB lookup (static, authored offline). nullopt on miss.
        static std::optional<TuningConfig> guidance(const TuningKey& key);

        // Default candidate block sizes for a kernel that opts into autotuning
        // (@Autotune) without naming its own set. The sweep clamps these to the
        // kernel's compile-time bound.
        static const std::vector<unsigned>& defaultCandidateBlocks();

    private:
        std::unordered_map<TuningKey, TuningConfig, TuningKeyHash> cache_;
    };

} // namespace xpu
} // namespace cajeta
