// SPIR-V (Vulkan) kernel lowering - @Kernel AST -> device llvm::Function, the Vulkan twin
// of nvidia/amd lowerKernel over the shared AST walk. Vulkan forks the kernel SIGNATURE and
// BUFFER ACCESS too, because its only buffer model is descriptor-bound storage buffers.
#pragma once

#include "SpirvBackend.h"   // ShaderStage

#include <memory>
#include <string>

namespace llvm {
    class Module;
    class Function;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace vulkan {

    // Lower `method` (a @Kernel) into the already-configured `deviceModule`, returning the
    // GLCompute entry. `softwareRayQuery` selects the software-BVH target for the "<name>$sw"
    // variant; `entryName` overrides the entry symbol. Throws XPU-N01 on an unsupported node.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule,
                                bool softwareRayQuery = false,
                                const std::string& entryName = "");

    // Lower a @Vertex/@Fragment stage into `deviceModule`, returning the `void main()` entry
    // for `stage`: params become Input interface variables and the return an Output one.
    // `entryName` overrides the entry symbol. Throws XPU-N01 on an unsupported construct.
    llvm::Function* lowerGraphicsShader(const MethodPtr& method,
                                        llvm::Module& deviceModule,
                                        ShaderStage stage,
                                        const std::string& entryName = "");

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
