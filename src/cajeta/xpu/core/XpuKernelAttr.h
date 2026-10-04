// A typed, read-only view over the XPU annotation cluster (@Kernel plus any
// @Wave / @Backend / @Occupancy) on one declaration, so consumers need not
// re-walk the AnnotationInstance list. Build a fresh view per access.

#pragma once

#include "XpuAttributes.h"

#include <optional>
#include <string>
#include <vector>

namespace cajeta {
class Method;
class CajetaClass;
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
        // A kernel method: annotation expressions are evaluated against its
        // class's template values when the class is an instantiation.
        static std::optional<XpuKernelAttr> from(const Method& m);

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
        // @Occupancy(maxWaves = N): the workgroup bound in the kernel's own
        // waves, for a kernel built as N waves per workgroup. Each backend
        // resolves it against the wave it gave the kernel (maxThreadsAt), so
        // one declaration is 256 threads at wave 32 and 512 at wave 64.
        // Declaring it beside maxThreads is refused at lowering.
        std::optional<unsigned> maxWaves() const { return maxWaves_; }
        // The thread bound at wave width `wave`: maxThreads when declared,
        // else maxWaves x wave, else nullopt.
        std::optional<unsigned> maxThreadsAt(unsigned wave) const {
            if (maxThreads_) return maxThreads_;
            if (maxWaves_) return *maxWaves_ * wave;
            return std::nullopt;
        }
        // Either form of the workgroup bound is declared.
        bool hasThreadBound() const { return maxThreads_ || maxWaves_; }
        // Non-empty when an @Occupancy argument is an expression that is not
        // a compile-time constant: the lowering refuses the kernel with it.
        const std::string& occupancyError() const { return occupancyError_; }
        bool hasOccupancy() const {
            return maxThreads_ || maxWaves_ || minResident_ || maxRegisters_;
        }

    private:
        static std::optional<XpuKernelAttr> fromWith(const Annotatable& a,
                                                     const CajetaClass* cls);
        std::optional<int> waveWidth_;
        std::vector<XpuBackend> backends_;
        std::optional<unsigned> maxThreads_;
        std::optional<unsigned> minResident_;
        std::optional<unsigned> maxRegisters_;
        std::optional<unsigned> maxWaves_;
        std::string occupancyError_;
    };

} // namespace xpu
} // namespace cajeta
