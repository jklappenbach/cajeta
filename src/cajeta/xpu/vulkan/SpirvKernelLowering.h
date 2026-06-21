//
// SPIR-V (Vulkan) kernel lowering — @Kernel AST → device llvm::Function.
//
// The Vulkan twin of nvidia::lowerKernel / amd::lowerKernel. Like them it is a
// thin wrapper over the shared xpu/lowering/KernelLowering.cpp AST walk; it
// supplies the SPIR-V LoweringTarget. But Vulkan forks MORE than AMD did
// (cajeta-xpu-matrix.md): not just the coordinate leaf reads, but the kernel
// SIGNATURE and BUFFER ACCESS, because LLVM 23's SPIR-V backend has no
// raw-pointer kernel ABI and no PhysicalStorageBuffer/BDA path — the only
// Vulkan buffer model is descriptor-bound storage buffers.
//
// SpirvTarget therefore overrides:
//   - the three leaf coordinate reads → llvm.spv.{thread.id.in.group,group.id,
//     workgroup.size}; globalId → llvm.spv.thread.id (native, overrides default)
//   - workgroupBarrier → llvm.spv.group.memory.barrier.with.group.sync
//   - allocaAddressSpace → 0 (Function storage)
//   - createKernel → `void main`-style entry: void(), no params, with the
//     hlsl.shader="compute" + hlsl.numthreads markers (LocalSize baked in)
//   - materializeParam → each Buffer<T>/scalar arg becomes a descriptor binding
//     (set 0, binding = param index) via llvm.spv.resource.handlefrombinding
//   - bufferElementPtr → llvm.spv.resource.getpointer for descriptor handles
//     (shared-mem globals still GEP)
//
// Unsupported constructs raise XPU-N01, exactly as the other backends.
//

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

    // Lower `method` (a @Kernel) into `deviceModule` (already SPIR-V-configured
    // via SpirvBackend::configureDeviceModule). Returns the created GLCompute
    // entry function. `softwareRayQuery` selects the SpirvSoftwareTarget (the
    // AccelerationStructure noun built as a software BVH → SoftwareRayQuery walk
    // in plain SPIR-V, AS bound as a storage buffer) for the "<name>$sw" variant;
    // `entryName` overrides the entry/registry symbol (default = method name).
    // Throws XPU-N01 on an unsupported construct.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule,
                                bool softwareRayQuery = false,
                                const std::string& entryName = "");

    // Lower a graphics shader method (a @Vertex/@Fragment/… stage) into
    // `deviceModule` (configured for `stage` via configureDeviceModuleForStage),
    // returning the created `void main()` entry with the stage's execution model.
    // The parallel of lowerKernel for the rasterization pipeline (gfx §4.b): the
    // shared body walk is reused; params become Input interface variables and the
    // return is stored to an Output interface variable. `entryName` overrides the
    // entry symbol (default = method name). Throws XPU-N01 on an unsupported
    // construct. `stage` is SpirvBackend's ShaderStage.
    llvm::Function* lowerGraphicsShader(const MethodPtr& method,
                                        llvm::Module& deviceModule,
                                        ShaderStage stage,
                                        const std::string& entryName = "");

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
