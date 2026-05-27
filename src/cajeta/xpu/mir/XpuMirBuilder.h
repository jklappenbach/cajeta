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
        // in the given module. Returns an XpuMirModule whose `kernels`
        // vector contains one record per @Kernel found. `launchSites`
        // is empty in v1 — populated in step 6.
        static XpuMirModulePtr buildForModule(const CajetaModulePtr& module);
    };

} // namespace mir
} // namespace xpu
} // namespace cajeta
