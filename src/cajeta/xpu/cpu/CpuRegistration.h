//
// CPU kernel registration — lower each @Kernel to host code, link it into the
// host module, and register its function pointer with the runtime.
//
// Unlike the GPU registrations (Nvptx/Amdgpu/Vulkan), there is no device binary
// to embed: a CPU kernel *is* host code. So this lowers the kernel into the host
// module under a decorated symbol (__cajeta_xpu_cpu.<name>) and emits a global
// ctor calling __cajeta_xpu_register_cpu_kernel(entryName, fnptr) — the CPU
// analog of the neutral __cajeta_xpu_register_module(name, bytes, len). The
// runtime dispatcher (Increment 4) resolves a launch to this pointer.
//
// Kernels whose body uses an unsupported construct (XPU-N01, e.g. a workgroup
// barrier) are skipped — never throws on a per-kernel basis.
//

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
namespace cpu {

    // Lower each @Kernel in `kernels` to host code, link into `hostModule`, and
    // emit a registration ctor per kernel. Returns the number embedded. `arch`
    // is ignored (the CPU backend has no arch); it is present for a uniform
    // backend-registration signature.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "");

} // namespace cpu
} // namespace xpu
} // namespace cajeta
