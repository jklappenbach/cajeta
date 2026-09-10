// Vulkan/SPIR-V kernel registration — embed each @Kernel's SPIR-V binary into
// the host module and emit a global ctor registering it with the runtime. Needs
// no GPU, only the SPIR-V target; an unsupported kernel is skipped, never fatal.

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

namespace vulkan {

    // Emit SPIR-V constants + registration ctors for each @Kernel in `kernels`,
    // returning how many were embedded. `arch` is the SPIR-V target env;
    // `manifests`, when given, receives identity + SPIR-V hash per kernel.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "vulkan1.3",
                               std::vector<KernelManifest>* manifests = nullptr);

    // The rasterization parallel for @Vertex/@Fragment/… methods: each is
    // lowered on its own per-stage SPIR-V TargetMachine and registered under its
    // entry name, so graphics shaders ride the normal build like @Kernels.
    int emitGraphicsRegistration(const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch = "vulkan1.3");

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
