// CPU kernel registration. A CPU kernel IS host code, so there is no binary to
// embed: the kernel is lowered into the host module as __cajeta_xpu_cpu.<name>,
// wrapped in a launcher thunk, and registered by a global ctor.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelManifest;

namespace cpu {

    // Lower each @Kernel into `hostModule` with a registration ctor, returning
    // how many. `arch` is ignored, present only for a uniform backend signature;
    // a CPU `manifests` entry is identity-only, its footprint absent, never zero.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "",
                               std::vector<KernelManifest>* manifests = nullptr);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
