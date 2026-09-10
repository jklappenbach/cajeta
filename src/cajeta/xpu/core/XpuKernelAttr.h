// A typed, read-only view over the XPU annotation cluster (@Kernel plus any
// @Wave / @Backend / @Occupancy) on one declaration, so consumers need not
// re-walk the AnnotationInstance list. Build a fresh view per access.

#pragma once

#include "XpuAttributes.h"

#include <optional>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {

    // Which backend(s) a kernel is restricted to.
    enum class XpuBackend {
        Nvidia,
        Amd,
        Vulkan,
    };

    // Parses a case-insensitive short name; nullopt if unrecognized.
    std::optional<XpuBackend> parseBackend(const std::string& name);

    // Inverse of parseBackend: the canonical short name, for diagnostics.
    const char* backendName(XpuBackend b);

    class XpuKernelAttr {
    public:
        // Build from an Annotatable; nullopt when @Kernel is absent.
        static std::optional<XpuKernelAttr> from(const Annotatable& a);

        // @Wave(width = N); nullopt lets the lowering pass pick a target default.
        std::optional<int> waveWidth() const { return waveWidth_; }

        // Backend restriction; empty means emit for every configured backend.
        const std::vector<XpuBackend>& backends() const { return backends_; }

        // Does this kernel emit for `b`? An empty restriction set returns true.
        bool emitsFor(XpuBackend b) const;

        // @Occupancy overrides: threads per workgroup, workgroups co-resident per
        // compute unit, registers per thread. Each is nullopt when absent.
        std::optional<unsigned> maxThreads() const { return maxThreads_; }
        std::optional<unsigned> minResident() const { return minResident_; }
        std::optional<unsigned> maxRegisters() const { return maxRegisters_; }
        bool hasOccupancy() const {
            return maxThreads_ || minResident_ || maxRegisters_;
        }

    private:
        std::optional<int> waveWidth_;
        std::vector<XpuBackend> backends_;
        std::optional<unsigned> maxThreads_;
        std::optional<unsigned> minResident_;
        std::optional<unsigned> maxRegisters_;
    };

} // namespace xpu
} // namespace cajeta
