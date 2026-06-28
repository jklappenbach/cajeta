//
// Typed view over the XPU annotation cluster on a single declaration.
//
// A @Kernel-bearing method may also carry @Wave(width = N) and
// @Backend("nvidia") (single or list). XpuKernelAttr collects those
// related annotations into one read-only value so downstream consumers
// (MIR builder, codegen routing, target selection) don't each re-walk
// the AnnotationInstance list.
//
// Construction is cheap — no allocations beyond the small backends
// vector — so call sites build a fresh view per access rather than
// caching on the Annotatable.
//

#pragma once

#include "XpuAttributes.h"

#include <optional>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {

    // Which backend(s) a kernel is restricted to. An empty backends()
    // vector on XpuKernelAttr means "no @Backend present" — emit for
    // every backend the build is configured to target (the variance-
    // discipline default per CajetaXPU.md §5.6).
    enum class XpuBackend {
        Nvidia,
        Amd,
        Vulkan,
    };

    // Parses a backend short name ("nvidia" / "amd" / "vulkan",
    // case-insensitive). Returns nullopt on unrecognized input.
    std::optional<XpuBackend> parseBackend(const std::string& name);

    // Inverse of parseBackend, returning the canonical short name
    // ("nvidia" / "amd" / "vulkan"). For diagnostics and round-trip
    // tests.
    const char* backendName(XpuBackend b);

    class XpuKernelAttr {
    public:
        // Build from an Annotatable carrying @Kernel (and optionally
        // @Wave / @Backend). Returns nullopt if @Kernel is absent —
        // callers can use isKernel() if they only need the boolean.
        static std::optional<XpuKernelAttr> from(const Annotatable& a);

        // Wave width from @Wave(width = N). nullopt when @Wave is
        // absent — the lowering pass picks a target default.
        std::optional<int> waveWidth() const { return waveWidth_; }

        // Backend restriction. Empty when @Backend is absent (emit
        // for all configured backends). Multi-backend supported via
        // @Backend({"nvidia", "amd"}).
        const std::vector<XpuBackend>& backends() const { return backends_; }

        // Convenience: does this kernel emit for a given backend? An
        // empty restriction set returns true (no restriction).
        bool emitsFor(XpuBackend b) const;

        // @Occupancy override (kernel-occupancy-autotune §3) — portable,
        // vendor-neutral resource logistics. Each is nullopt when absent.
        //   maxThreads   — max threads per workgroup (launch bound)
        //   minResident  — min workgroups co-resident per compute unit
        //   maxRegisters — max registers per thread
        std::optional<unsigned> maxThreads() const { return maxThreads_; }
        std::optional<unsigned> minResident() const { return minResident_; }
        std::optional<unsigned> maxRegisters() const { return maxRegisters_; }
        bool hasOccupancy() const {
            return maxThreads_ || minResident_ || maxRegisters_;
        }

        // @Autotune (kernel-occupancy-autotune §4): block-flexible, runtime-tunable.
        bool autotune() const { return autotune_; }
        // Candidate block sizes from @Autotune(blocks = {...}); empty when @Autotune
        // is absent or carries no explicit list (the tuner's defaults then apply).
        const std::vector<unsigned>& autotuneBlocks() const { return autotuneBlocks_; }
        // The compile-time workgroup bound for an autotuned kernel = the largest
        // candidate (explicit list, else the tuner defaults), so every swept block
        // is a legal launch. 0 when not autotuned.
        unsigned autotuneMaxThreads() const;

    private:
        std::optional<int> waveWidth_;
        std::vector<XpuBackend> backends_;
        std::optional<unsigned> maxThreads_;
        std::optional<unsigned> minResident_;
        std::optional<unsigned> maxRegisters_;
        bool autotune_ = false;
        std::vector<unsigned> autotuneBlocks_;
    };

} // namespace xpu
} // namespace cajeta
