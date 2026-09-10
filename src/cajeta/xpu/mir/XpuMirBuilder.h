// XpuMirBuilder — AST → XpuMirModule for @Kernel / @Device methods. Pure: it
// reads parsed CajetaModule / Method state and constructs XpuMir* structures
// without mutating the input, leaving the caller to persist the result.

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
        // A per-method kernel record, or nullptr if the method isn't a @Kernel.
        static XpuMirKernelPtr buildKernelForMethod(const MethodPtr& method);

        // A per-module envelope: one XpuMirKernel per @Kernel method (bodyOps
        // populated) and one XpuMirLaunchSite per host-side launch site.
        static XpuMirModulePtr buildForModule(const CajetaModulePtr& module);

        // Append an XpuMirOp leaf to `out` per leaf builtin (Thread / Workgroup
        // / Barrier) in a @Kernel body. Public so it can run standalone.
        static void collectBodyOps(const MethodPtr& method,
                                   std::vector<XpuMirOpPtr>& out);
    };

} // namespace mir
} // namespace xpu
} // namespace cajeta
