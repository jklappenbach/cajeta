// SPIR-V backend: an LLVM device module emitted straight to Vulkan-flavor SPIR-V
// (Shader/GLCompute/Logical GLSL450) by LLVM's in-tree backend, with no external tool.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
    class Module;
    class TargetMachine;
}

namespace cajeta {
namespace xpu {
namespace vulkan {

    // The device triple: 32-bit logical SPIR-V for a Vulkan 1.3 compute env.
    inline constexpr const char* kSpirvTriple = "spirv-unknown-vulkan1.3-compute";

    // Vulkan fixes the local size at SPIR-V compile time rather than per-dispatch,
    // so this constant is what the driver must use as the launch block dim.
    inline constexpr unsigned kVulkanLocalSizeX = 64;

    // Create a SPIR-V TargetMachine for `arch` (the SPIR-V target env), registering
    // the LLVM target on first use; nullptr when the spirv target is not built in.
    std::unique_ptr<llvm::TargetMachine>
    createSpirvTargetMachine(const std::string& arch = "vulkan1.3");

    // Set the SPIR-V triple + the TargetMachine's DataLayout on `m`.
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // --- Per-stage knob: each shader stage rides its own triple env and module ---

    // The shader stages emitted as standalone SPIR-V modules.
    enum class ShaderStage {
        Compute,
        Vertex,
        Fragment,
        Geometry,
        TessControl,
        TessEval,
        Mesh,
        Task,
    };

    // The triple ENVIRONMENT token of "spirv-unknown-<arch>-<env>" for `stage`.
    const char* spirvStageEnv(ShaderStage stage);

    // The full per-stage triple; for (Compute, "vulkan1.3") this is kSpirvTriple.
    std::string spirvStageTriple(ShaderStage stage,
                                 const std::string& arch = "vulkan1.3");

    // The `hlsl.shader` attribute value the backend turns into the OpEntryPoint
    // execution model; set it on the entry function. Spelled like the triple env.
    const char* hlslShaderAttr(ShaderStage stage);

    // Per-stage counterpart of createSpirvTargetMachine; nullptr if unavailable.
    std::unique_ptr<llvm::TargetMachine>
    createSpirvTargetMachineForStage(ShaderStage stage,
                                     const std::string& arch = "vulkan1.3");

    // Set the per-stage SPIR-V triple + the TargetMachine's DataLayout on `m`.
    void configureDeviceModuleForStage(llvm::Module& m, llvm::TargetMachine& tm,
                                       ShaderStage stage,
                                       const std::string& arch = "vulkan1.3");

    // SPIR-V assembly text for `deviceModule`, GPU-free; empty on failure.
    std::string emitSpirvText(llvm::Module& deviceModule, llvm::TargetMachine& tm);

    // The Khronos SPIR-V binary for `deviceModule`; empty on failure.
    std::vector<uint8_t> emitSpirv(llvm::Module& deviceModule,
                                   llvm::TargetMachine& tm);

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
