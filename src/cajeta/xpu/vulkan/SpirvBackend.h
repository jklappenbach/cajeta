//
// SPIR-V backend — LLVM device-module → Vulkan compute SPIR-V.
//
// The Vulkan analog of NvptxBackend / AmdgpuBackend (cajeta-xpu.md "Vulkan
// third backend"). Where NVPTX goes LLVM → PTX → ptxas → cubin and AMDGPU goes
// LLVM → AMDGCN object → lld → hsaco, SPIR-V is the SIMPLEST of the three:
// LLVM 23's in-tree SPIR-V backend emits the final Khronos SPIR-V binary
// directly — no external assembler or linker. SPIR-V text (emitSpirvText) is
// the GPU-free Tier-0 hook the emit tests assert against; the binary
// (emitSpirv) is what the on-device launch hands to vkCreateShaderModule.
//
// The triple targets the Vulkan-flavor SPIR-V (OpCapability Shader, GLCompute,
// Logical GLSL450 memory model) — NOT the OpenCL-Kernel flavor. That choice is
// why the kernel signature + buffer access fork (see SpirvKernelLowering): in
// the Logical model buffers are descriptor-bound storage buffers, not raw
// pointers.
//

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

    // Compute workgroup size baked into every kernel's `OpExecutionMode
    // LocalSize` (via the hlsl.numthreads attribute). Unlike CUDA/HIP, Vulkan
    // fixes the local size at SPIR-V compile time, not per-dispatch — so this
    // is a backend constant the driver must match as the launch block dim. (A
    // spec-constant LocalSizeId is a later refinement; see cajeta-xpu-matrix.md.)
    inline constexpr unsigned kVulkanLocalSizeX = 64;

    // Create a SPIR-V TargetMachine. `arch` is the SPIR-V target env (e.g.
    // "vulkan1.3"); unused by the in-tree backend today but kept for parity
    // with the nvptx/amdgpu arch knob. Returns nullptr if the spirv target
    // isn't registered. Self-initializes the LLVM target registry.
    std::unique_ptr<llvm::TargetMachine>
    createSpirvTargetMachine(const std::string& arch = "vulkan1.3");

    // Set the SPIR-V triple + the TargetMachine's DataLayout on `m`.
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // Emit SPIR-V assembly (disassembly) text for `deviceModule`. GPU-free;
    // what the emit tests grep for OpEntryPoint / OpExecutionMode. Empty on
    // failure.
    std::string emitSpirvText(llvm::Module& deviceModule, llvm::TargetMachine& tm);

    // Emit the Khronos SPIR-V binary for `deviceModule` (the .spv handed to
    // vkCreateShaderModule). No external tool. Empty on failure.
    std::vector<uint8_t> emitSpirv(llvm::Module& deviceModule,
                                   llvm::TargetMachine& tm);

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
