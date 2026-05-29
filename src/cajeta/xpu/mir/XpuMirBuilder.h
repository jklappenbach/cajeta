//
// XpuMirBuilder — AST → XpuMirModule for @Kernel / @Device methods.
//
// v1 builds the structural envelope only:
//
//   - per-kernel record with canonical name, parameter list (with
//     address-space-qualified types), and the @Wave / @Backend co-
//     annotation values via XpuKernelAttr
//   - placeholder body-ops vector (populated in step 5 when the
//     leaf-builtin walker lands)
//   - placeholder launch-site list (populated in step 6 when the
//     LaunchSiteResolver lands)
//
// The builder is pure — it inspects the parsed CajetaModule /
// Method state and constructs XpuMir* structures without mutating
// the input. Callers persist the result wherever they need (tests
// hold them locally; the compile pipeline will hand the module to
// the lowering passes later).
//

#pragma once

#include <memory>

#include "XpuMir.h"

namespace cajeta {
    class CajetaModule;
    using CajetaModulePtr = std::shared_ptr<CajetaModule>;

    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace mir {

    class XpuMirBuilder {
    public:
        // Build a per-method kernel record. Returns nullptr if the
        // method isn't a @Kernel.
        static XpuMirKernelPtr buildKernelForMethod(const MethodPtr& method);

        // Build a per-module envelope by walking every @Kernel method
        // in the given module (one XpuMirKernel each, with bodyOps
        // populated) and every method body for host-side launch sites
        // (one XpuMirLaunchSite each).
        static XpuMirModulePtr buildForModule(const CajetaModulePtr& module);

        // Walk a @Kernel body for leaf builtins (Thread / Workgroup /
        // Barrier calls) and append the matching XpuMirOp leaves to
        // `out`. Public so tests and the backend pass can run it
        // standalone. No-op for a method with no body.
        static void collectBodyOps(const MethodPtr& method,
                                   std::vector<XpuMirOpPtr>& out);
    };

} // namespace mir
} // namespace xpu
} // namespace cajeta
