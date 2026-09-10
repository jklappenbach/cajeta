// Shared @Kernel AST -> device llvm::Function lowering: the backend-neutral part of
// kernel lowering (AST walk, mutable-slot scalars, addrspace(1) buffers, addrspace(3)
// `shared`, operators, control flow, casts). Unsupported constructs raise XPU-N01.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cajeta_xpu_abi.h"   // CajetaXpuParamKind — the frozen XPU FFI contract

namespace llvm {
    class Module;
    class Function;
    class LLVMContext;
    class DataLayout;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    class LoweringTarget;

    // Lower `method` (a @Kernel) into `deviceModule`, already configured for `target`'s
    // backend, and return the created kernel function. `entryName` overrides the kernel
    // symbol (default = the simple method name). Throws XPU-N01 on an unsupported node.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule,
                                LoweringTarget& target,
                                const std::string& entryName = "");

    // One kernel parameter's runtime-launch shape, in declaration order: the metadata
    // the Vulkan dispatcher needs to turn the uniform kernelParams argv into bindings.
    struct KernelParamInfo {
        // Derived from the frozen XPU FFI contract (CajetaXpuParamKind in
        // runtime/native/cajeta_xpu_abi.h), static-asserted below so it cannot drift.
        enum Kind : uint8_t {
            Scalar      = CAJETA_XPU_KP_SCALAR,
            Buffer      = CAJETA_XPU_KP_BUFFER,
            Texture     = CAJETA_XPU_KP_TEXTURE,
            Sampler     = CAJETA_XPU_KP_SAMPLER,
            AccelStruct = CAJETA_XPU_KP_ACCEL,
            Image       = CAJETA_XPU_KP_IMAGE,
            BufferArray = CAJETA_XPU_KP_BUFFER_ARRAY
        };
        uint8_t kind;
        unsigned byteSize;   // scalar/POD byte size (0 for buffer/texture/sampler)
    };
    static_assert(KernelParamInfo::Scalar == 0 && KernelParamInfo::Buffer == 1 &&
                  KernelParamInfo::Texture == 2 && KernelParamInfo::Sampler == 3 &&
                  KernelParamInfo::AccelStruct == 4 && KernelParamInfo::Image == 5 &&
                  KernelParamInfo::BufferArray == 6,
                  "KernelParamInfo::Kind must match the frozen CajetaXpuParamKind "
                  "values in cajeta_xpu_abi.h (the XPU FFI contract)");
    // One KernelParamInfo per parameter, in declaration order. `dl` MUST be the host
    // module's DataLayout: the by-value byteSize it computes has to match the footprint
    // the launch site packs argv with, or the device reads past the SSBO (H11).
    std::vector<KernelParamInfo> collectKernelParamInfo(const MethodPtr& method,
                                                        llvm::LLVMContext& ctx,
                                                        const llvm::DataLayout& dl);

} // namespace xpu
} // namespace cajeta
