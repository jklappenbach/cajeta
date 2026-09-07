//
// KernelAccess — what a lowered kernel does to each buffer-like parameter
// (specs/xpu-tile-manifest-spec.md §6), read off the LOWERED IR by pointer
// provenance: every load, store and atomic is walked back through GEPs,
// casts, phis, selects, reloaded slots and the SPIR-V resource intrinsics to
// the kernel parameter it addresses. One walk covers every access shape the
// lowering emits (scalar, vector, cooperative-matrix, atomic) on every
// backend, so the manifest's modes are right by construction — no author
// restates them (§6.6).
//
//   mode     read | write | readwrite | accumulate      (`indirect` arrives
//                                                       with the ragged
//                                                       surface, Unit 6)
//   origin   derived (from the body) | declared (`@Access(...)`, checked)
//   streaming  every load/store of the parameter carries !nontemporal
//   restartable  no parameter is readwrite or accumulate (§6.5)
//   drainsDevice every global write is to a compile-time-constant element:
//                the kernel is a global reduction whose scalar result the
//                host (or a scalar decision) consumes (§6.7)
//

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace llvm {
    class Function;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelAccessEntry {
        std::string param;     // parameter name, declaration order
        std::string kind;      // kernelBuffer | image
        std::string mode;      // read | write | readwrite | accumulate
        std::string origin;    // derived | declared
        bool streaming = false;
    };

    struct KernelAccessSummary {
        std::vector<KernelAccessEntry> entries;   // buffer-like params only
        bool restartable = false;
        bool drainsDevice = false;
    };

    // Classify `kfn` (the function lowerKernel returned, before any backend
    // assembly mutates the module) against `method`'s parameter list. Pure.
    KernelAccessSummary classifyKernelAccess(llvm::Function& kfn, const MethodPtr& method);

    // Apply the author's declarations to the lowered body — called at the end
    // of lowerKernel, before the backend assembles:
    //   @Streaming p  — when `nontemporalSupported`, tag every load/store of p
    //                   with !nontemporal (the manifest then records streaming
    //                   because the IR says so, not because the author asked).
    //   @Access(m) p  — the body must not contradict m (§6.2): a declared
    //                   `read` with a store, a `write` with a load, ... throws
    //                   CAJETA_ERROR_XPU_ACCESS_CONTRADICTED; an unknown mode
    //                   throws CAJETA_ERROR_XPU_ACCESS_UNKNOWN.
    void applyAccessDeclarations(llvm::Function& kfn, const MethodPtr& method,
                                 bool nontemporalSupported);

} // namespace xpu
} // namespace cajeta
