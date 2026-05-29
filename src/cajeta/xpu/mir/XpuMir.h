//
// XPU MIR containers.
//
// Per CajetaXPU.md §2 (architecture diagram), the MIR sits between
// the type-checked / borrow-checked AST and the per-backend LLVM
// lowering. The shape here is intentionally thin: one XpuMirKernel
// per @Kernel function, one XpuMirLaunchSite per host-side launch
// call, both held by an XpuMirModule per compilation unit.
//
// Kernel bodies are NOT lifted into MIR ops — the existing
// CajetaLlvmVisitor still walks the AST for normal statements. MIR
// only carries the structural envelope (signature with address-
// space-qualified types, capability/backend attrs, the launch
// site records) and the leaf-builtin lookups (Op_ThreadId etc.)
// that need per-backend resolution.
//

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

    // Forward declaration matching the canonical typedef in
    // asn/expression/Expression.h — launch sites hold AST expression
    // references (stream / grid / block / kernel args) that the host
    // launch codegen re-walks later.
    class Expression;
    using ExpressionPtr = std::shared_ptr<Expression>;
}

namespace cajeta {
namespace xpu {
namespace mir {

    // One parameter of a kernel signature. Carries the address-space
    // qualified type plus the Cajeta-side parameter name (for the
    // printer and for diagnostics).
    struct XpuKernelParam {
        std::string name;
        XpuMirType type;
    };

    // The per-kernel record. Built once per @Kernel method by
    // XpuMirBuilder; consumed by the backend lowering passes in
    // steps 5-11.
    struct XpuMirKernel {
        // Owning back-reference to the AST method. Body codegen
        // continues through the existing CajetaLlvmVisitor against
        // this method; MIR doesn't replace the body, only the
        // structural envelope around it.
        MethodPtr method;

        // Fully-qualified kernel name (`pkg.Class.method`).
        std::string canonicalName;

        // Parameter list with address-space-qualified types. Mirrors
        // method->getParameterList() in order; `this` is dropped
        // because @Kernel methods are static.
        std::vector<XpuKernelParam> params;

        // From the @Wave / @Backend co-annotations on the method.
        // Empty backends() vector means "no @Backend annotation,
        // emit for every configured backend".
        std::optional<int> waveWidth;
        std::vector<XpuBackend> backends;

        // Op leaves discovered inside the kernel body. v1 leaves
        // this empty — the leaf-builtin walker lands in step 5.
        std::vector<XpuMirOpPtr> bodyOps;
    };

    using XpuMirKernelPtr = std::shared_ptr<XpuMirKernel>;

    // Per-call-site record for a host-side
    // `kernel.launch(stream, grid: [...], block: [...])(args)` call.
    // Recognized and populated by XpuMirBuilder by walking host method
    // bodies. The dimension and argument expressions are held as AST
    // references; the host launch codegen (CajetaXPU step ~E) re-walks
    // them to compute grid/block and marshal the cuLaunchKernel arg
    // array. `kernelCanonicalName` resolves to the matching @Kernel's
    // canonical when one is found in the module, else the bare receiver
    // name from the call site.
    struct XpuMirLaunchSite {
        std::string kernelCanonicalName;
        ExpressionPtr stream;                  // first positional launch() arg
        std::vector<ExpressionPtr> grid;       // `grid:  [...]` element exprs
        std::vector<ExpressionPtr> block;      // `block: [...]` element exprs
        std::vector<ExpressionPtr> kernelArgs; // the trailing `(args)`
    };

    using XpuMirLaunchSitePtr = std::shared_ptr<XpuMirLaunchSite>;

    // Per-compilation-unit container. One module's worth of XPU
    // metadata. Lives alongside the user's CajetaModule; the
    // CajetaModule itself doesn't gain a direct pointer in v1
    // (looked up via XpuMirRegistry below).
    struct XpuMirModule {
        std::vector<XpuMirKernelPtr> kernels;
        std::vector<XpuMirLaunchSitePtr> launchSites;
    };

    using XpuMirModulePtr = std::shared_ptr<XpuMirModule>;

} // namespace mir
} // namespace xpu
} // namespace cajeta
