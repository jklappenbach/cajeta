// NVPTX kernel registration — embed each @Kernel's cubin into the host module as
// a private constant and append an llvm.global_ctors entry registering it under
// the kernel's simple method name, the same string the launch site resolves by.

#pragma once

#include <string>
#include <vector>
#include <memory>

namespace llvm { class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelManifest;

namespace nvidia {

    // Emit cubin constants + registration ctors for each @Kernel in `kernels`,
    // returning how many were embedded. Never throws: an unsupported kernel, or
    // all of them when ptxas is absent, is skipped. `manifests` takes one per kernel.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "sm_89",
                               std::vector<KernelManifest>* manifests = nullptr);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
