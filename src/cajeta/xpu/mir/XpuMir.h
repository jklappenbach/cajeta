// XPU MIR containers, sitting between the checked AST and the per-backend LLVM
// lowering: one XpuMirKernel per @Kernel, one XpuMirLaunchSite per host launch call.
// Bodies are NOT lifted; MIR carries only the structural envelope and leaf builtins.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "XpuMirOp.h"
#include "XpuMirType.h"

#include "../core/XpuKernelAttr.h"

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;

    // Launch sites hold AST expression references the host codegen re-walks later.
    class Expression;
    using ExpressionPtr = std::shared_ptr<Expression>;
}

namespace cajeta {
namespace xpu {
namespace mir {

    // One kernel-signature parameter: address-space-qualified type plus source name.
    struct XpuKernelParam {
        std::string name;
        XpuMirType type;
    };

    // The per-kernel record, built once per @Kernel method by XpuMirBuilder and
    // consumed by the backend lowering passes.
    struct XpuMirKernel {
        // Back-reference to the AST method whose body codegen still runs unchanged.
        MethodPtr method;

        // Fully-qualified kernel name (`pkg.Class.method`).
        std::string canonicalName;

        // Mirrors method->getParameterList() in order, less `this`: kernels are static.
        std::vector<XpuKernelParam> params;

        // From @Wave / @Backend; an empty `backends` means every configured backend.
        std::optional<int> waveWidth;
        std::vector<XpuBackend> backends;

        // Leaf builtin ops in the body that need per-backend resolution.
        std::vector<XpuMirOpPtr> bodyOps;
    };

    using XpuMirKernelPtr = std::shared_ptr<XpuMirKernel>;

    // Per-call-site record for a host-side `kernel.launch(stream, grid:, block:)(args)`.
    // Dimension and argument expressions stay as AST references for the launch codegen.
    // `kernelCanonicalName` falls back to the bare receiver name when no @Kernel matches.
    struct XpuMirLaunchSite {
        std::string kernelCanonicalName;
        ExpressionPtr stream;                  // first positional launch() arg
        std::vector<ExpressionPtr> grid;       // `grid:  [...]` element exprs
        std::vector<ExpressionPtr> block;      // `block: [...]` element exprs
        std::vector<ExpressionPtr> kernelArgs; // the trailing `(args)`
    };

    using XpuMirLaunchSitePtr = std::shared_ptr<XpuMirLaunchSite>;

    // One compilation unit's XPU metadata, looked up via XpuMirRegistry rather than
    // held by a pointer on CajetaModule.
    struct XpuMirModule {
        std::vector<XpuMirKernelPtr> kernels;
        std::vector<XpuMirLaunchSitePtr> launchSites;
    };

    using XpuMirModulePtr = std::shared_ptr<XpuMirModule>;

} // namespace mir
} // namespace xpu
} // namespace cajeta
