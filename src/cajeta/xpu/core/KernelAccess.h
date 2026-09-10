// KernelAccess — what a lowered kernel does to each buffer-like parameter,
// derived off the LOWERED IR by walking every load, store and atomic back
// through GEPs, casts, phis, selects, slots and resource intrinsics.

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
        bool restartable = false;    // no parameter is readwrite or accumulate
        bool drainsDevice = false;   // every global write hits a constant element
    };

    // Classify `kfn` (the function lowerKernel returned, before any backend
    // assembly mutates the module) against `method`'s parameter list. Pure.
    KernelAccessSummary classifyKernelAccess(llvm::Function& kfn, const MethodPtr& method);

    // Apply @Streaming (tag loads/stores !nontemporal when supported) and check
    // @Access against the body, throwing CAJETA_ERROR_XPU_ACCESS_CONTRADICTED or
    // _UNKNOWN. Runs at the end of lowerKernel, before the backend assembles.
    void applyAccessDeclarations(llvm::Function& kfn, const MethodPtr& method,
                                 bool nontemporalSupported);

} // namespace xpu
} // namespace cajeta
