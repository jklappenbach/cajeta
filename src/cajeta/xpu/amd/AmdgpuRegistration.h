// AMDGPU kernel registration — embed each @Kernel's hsaco into the host module
// and emit a global ctor registering it with the runtime. Needs no GPU, only the
// AMDGPU target and lld; an unsupported kernel is skipped, never fatal.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelManifest;

namespace amd {

    // Kernel canonical name -> the largest workgroup size any launch site asks
    // for, which sets amdgpu-flat-work-group-size so registers are budgeted right.
    using KernelMaxThreads = std::unordered_map<std::string, unsigned>;

    // Emit hsaco constants + registration ctors for each @Kernel in `kernels`,
    // returning how many were embedded. `arch` is a GFX target; `manifests`, when
    // given, receives one entry per (kernel, arch) actually embedded.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "gfx1151",
                               const KernelMaxThreads& maxThreads = {},
                               std::vector<KernelManifest>* manifests = nullptr);

} // namespace amd
} // namespace xpu
} // namespace cajeta
