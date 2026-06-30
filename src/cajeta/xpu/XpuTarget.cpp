//
// XPU backend dispatch — see header.
//

#include "XpuTarget.h"

#include "nvidia/NvptxRegistration.h"
#include "amd/AmdgpuRegistration.h"
#include "vulkan/VulkanRegistration.h"
#include "cpu/CpuRegistration.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <string>

namespace cajeta {
namespace xpu {

    int emitKernelRegistration(Backend backend,
                               const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch,
                               const std::unordered_map<std::string, unsigned>&
                                   kernelMaxThreads) {
        switch (backend) {
            case Backend::Nvptx:
                return nvidia::emitKernelRegistration(kernels, hostModule, arch);
            case Backend::Amdgpu:
                // Only AMDGPU consumes the launch workgroup sizes today
                // (amdgpu-flat-work-group-size); other backends ignore them.
                return amd::emitKernelRegistration(kernels, hostModule, arch,
                                                   kernelMaxThreads);
            case Backend::Spirv:
                return vulkan::emitKernelRegistration(kernels, hostModule, arch);
            case Backend::Cpu:
                return cpu::emitKernelRegistration(kernels, hostModule, arch);
        }
        return 0;
    }

    int emitGraphicsRegistration(Backend backend,
                                 const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch) {
        // Rasterization is a SPIR-V/Vulkan-only capability; the other device
        // backends have no graphics pipeline, so they simply embed nothing.
        if (backend == Backend::Spirv)
            return vulkan::emitGraphicsRegistration(shaders, hostModule, arch);
        return 0;
    }

    void emitBackendManifest(const std::vector<Backend>& backends,
                             llvm::Module& hostModule) {
        if (backends.empty()) return;
        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);

        // void __cajeta_xpu_register_backend(i32 id)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {i32}, false);
        llvm::FunctionCallee regFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_backend", regTy);

        llvm::IRBuilder<> b(ctx);
        for (Backend be : backends) {
            const int id = (int) be;   // enum aligned with the runtime ids
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                std::string("__cajeta_xpu_backend_manifest_ctor.") +
                    std::to_string(id),
                &hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            b.CreateCall(regFn, {llvm::ConstantInt::get(i32, id)});
            b.CreateRetVoid();
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
        }
    }

} // namespace xpu
} // namespace cajeta
