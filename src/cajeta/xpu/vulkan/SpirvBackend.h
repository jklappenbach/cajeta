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
    // isn't registered. Self-initializes the LLVM target registry. Equivalent to
    // createSpirvTargetMachineForStage(ShaderStage::Compute, arch).
    std::unique_ptr<llvm::TargetMachine>
    createSpirvTargetMachine(const std::string& arch = "vulkan1.3");

    // Set the SPIR-V triple + the TargetMachine's DataLayout on `m`.
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // --- Per-stage knob (cajeta-gfx §4.a) ----------------------------------
    //
    // The SPIR-V backend is no longer compute-only: each shader stage rides its
    // OWN triple environment (the trailing "-<env>" of the triple), so each is
    // emitted as its own module / .spv — exactly how Vulkan binds shader stages
    // (proven feasible in test/gfx/GfxSpirvEmitProbeTests). The execution model
    // is selected by BOTH the triple env AND the entry function's `hlsl.shader`
    // attribute, which the in-tree backend turns into the OpEntryPoint model.

    // The shader stages emitted as standalone SPIR-V modules. Compute is the
    // existing path; Vertex/Fragment are §4.a; the rest generalize the knob and
    // land their emission goldens under §4.e.
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

    // The triple ENVIRONMENT token for a stage — the trailing field of
    // "spirv-unknown-<arch>-<env>": compute / vertex / pixel / geometry / hull /
    // domain / mesh / amplification (the LLVM/HLSL stage spellings).
    const char* spirvStageEnv(ShaderStage stage);

    // The full per-stage SPIR-V triple, "spirv-unknown-<arch>-<env>" (arch e.g.
    // "vulkan1.3"). For (Compute, "vulkan1.3") this equals kSpirvTriple.
    std::string spirvStageTriple(ShaderStage stage,
                                 const std::string& arch = "vulkan1.3");

    // The `hlsl.shader` function-attribute value the in-tree SPIR-V backend
    // turns into the OpEntryPoint execution model. Set this on the entry
    // function (e.g. fn->addFnAttr("hlsl.shader", hlslShaderAttr(stage))). The
    // spelling coincides with the triple env token.
    const char* hlslShaderAttr(ShaderStage stage);

    // Create a SPIR-V TargetMachine for `stage` (its per-stage triple). Returns
    // nullptr if that stage's target env isn't available in this LLVM build.
    std::unique_ptr<llvm::TargetMachine>
    createSpirvTargetMachineForStage(ShaderStage stage,
                                     const std::string& arch = "vulkan1.3");

    // Set the per-stage SPIR-V triple + the TargetMachine's DataLayout on `m`.
    void configureDeviceModuleForStage(llvm::Module& m, llvm::TargetMachine& tm,
                                       ShaderStage stage,
                                       const std::string& arch = "vulkan1.3");

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
