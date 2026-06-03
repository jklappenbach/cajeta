//
// Shared @Kernel AST → device llvm::Function lowering.
//
// This is the backend-neutral ~90% of kernel lowering (cajeta-amd.md §2):
// the AST walk, the mutable-slot scalar model, buffer/array indexing in
// addrspace(1), the `shared` keyword in addrspace(3), the full int/float
// operator set, control flow, casts. It was the NVPTX-only DeviceLowerer
// until a second backend (AMDGPU) was threaded through it; everything that
// stayed here is the measured shared surface, everything that forked moved
// behind LoweringTarget.
//
// The per-backend lowerers (nvidia::lowerKernel, amd::lowerKernel) are thin
// wrappers that construct their LoweringTarget and call this.
//
// Supported subset (unchanged from the NVPTX origin):
//   - params: primitives by value, Buffer<T> / T[] as ptr addrspace(1)
//   - Thread / Workgroup coordinate builtins, Barrier.workgroup()
//   - mutable locals (entry-block allocas in the target's alloca AS; mem2reg'd
//     before emit), scalar + buffer-element assignment + compound assignment
//   - if/else, for / while / do-while, unlabeled break / continue
//   - buffer/array index load & store, workgroup-shared memory (`shared`)
//   - full integer + float operator set, unary/prefix/postfix, numeric casts
// Unsupported constructs raise XPU-N01.
//

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace llvm {
    class Module;
    class Function;
    class LLVMContext;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    class LoweringTarget;

    // Lower `method` (a @Kernel) into `deviceModule` (already configured for
    // `target`'s backend) using `target` for the divergent decisions. Returns
    // the created kernel function (symbol = simple method name). Throws
    // XPU-N01 on an unsupported construct.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule,
                                LoweringTarget& target);

    // One kernel parameter's runtime-launch shape, in declaration order — the
    // metadata the Vulkan rung of the runtime dispatcher needs to translate the
    // uniform kernelParams argv into descriptor bindings (buffers map to
    // existing storage buffers; scalars become single-element SSBOs sized by
    // byteSize; textures bind sampled images; samplers bind VkSamplers).
    // Backend-neutral classification, same as lowerKernel uses.
    struct KernelParamInfo {
        // kind: 0 = scalar, 1 = buffer, 2 = texture, 3 = sampler, 4 =
        // acceleration structure (matches the runtime's CAJETA_KP_* constants
        // in cajeta_runtime.c).
        enum Kind : uint8_t {
            Scalar = 0, Buffer = 1, Texture = 2, Sampler = 3, AccelStruct = 4
        };
        uint8_t kind;
        unsigned byteSize;   // scalar/POD byte size (0 for buffer/texture/sampler)
    };
    std::vector<KernelParamInfo> collectKernelParamInfo(const MethodPtr& method,
                                                        llvm::LLVMContext& ctx);

} // namespace xpu
} // namespace cajeta
