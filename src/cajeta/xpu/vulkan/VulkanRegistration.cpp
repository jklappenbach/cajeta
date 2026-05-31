//
// Vulkan/SPIR-V kernel registration pass — see header.
//
// Structurally identical to Nvptx/AmdgpuRegistration: for each @Kernel it
// lowers a device module, emits the SPIR-V binary, embeds the bytes as a
// private host-module constant, and appends an llvm.global_ctors entry calling
// the backend-neutral __cajeta_xpu_register_module(entryName, bytes, len). The
// runtime keys modules by entry name, so the same launch path resolves a
// Vulkan kernel exactly as it does an NVIDIA or AMD one — only the binary
// format behind it (cubin/hsaco → SPIR-V) changed.
//

#include "VulkanRegistration.h"
#include "SpirvBackend.h"
#include "SpirvKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace cajeta {
namespace xpu {
namespace vulkan {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch) {
        if (kernels.empty()) return 0;

        // One SPIR-V TargetMachine for all kernels. If the spirv target isn't
        // in this LLVM build there's nothing to emit.
        auto tm = createSpirvTargetMachine(arch);
        if (!tm) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // void __cajeta_xpu_register_module(i8* name, i8* image, i64 len)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module", regTy);

        // void __cajeta_xpu_register_kernel_params(i8* name, i32 count,
        //                                          i8* isBuffer, i32* byteSize)
        // The Vulkan rung of the runtime dispatcher needs this to turn the
        // uniform kernelParams argv into descriptor bindings (scalars -> SSBOs).
        llvm::FunctionType* kpTy = llvm::FunctionType::get(
            voidTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
        llvm::FunctionCallee kpFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_params", kpTy);

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();

            // Lower this kernel into a fresh device module + emit SPIR-V. The
            // device lowerer builds types in its own context, so this never
            // touches the host module until we have bytes.
            llvm::LLVMContext devCtx;
            llvm::Module devMod("xpu.dev." + entryName, devCtx);
            configureDeviceModule(devMod, *tm);
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, devMod);
            } catch (cajeta::Exception&) {
                // Unsupported construct (XPU-N01) — leave this kernel to the
                // host path; don't fail the whole compile.
                continue;
            }
            if (!kfn) continue;

            std::vector<uint8_t> spirv = emitSpirv(devMod, *tm);
            if (spirv.empty()) continue;  // codegen error (logged)

            // Embed the SPIR-V as a private host-module constant.
            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(spirv.data(), spirv.size()));
            auto* spvGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.spirv." + entryName);
            spvGV->setAlignment(llvm::MaybeAlign(8));

            // ctor: __cajeta_xpu_register_module(entryName, spvGV, len)
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, spvGV,
                                 llvm::ConstantInt::get(i64Ty, spirv.size())});

            // Per-kernel parameter metadata: which args are buffers vs scalars,
            // and the scalar byte sizes — so the runtime can bind buffers and
            // wrap scalars in single-element SSBOs at launch.
            std::vector<KernelParamInfo> info =
                collectKernelParamInfo(method, ctx);
            if (!info.empty()) {
                std::vector<uint8_t> isBuf;
                std::vector<uint32_t> sizes;
                isBuf.reserve(info.size());
                sizes.reserve(info.size());
                for (auto& pi : info) {
                    isBuf.push_back(pi.isBuffer ? 1 : 0);
                    sizes.push_back(pi.byteSize);
                }
                llvm::Constant* isBufInit = llvm::ConstantDataArray::get(
                    ctx, llvm::ArrayRef<uint8_t>(isBuf.data(), isBuf.size()));
                auto* isBufGV = new llvm::GlobalVariable(
                    hostModule, isBufInit->getType(), /*isConstant=*/true,
                    llvm::GlobalValue::PrivateLinkage, isBufInit,
                    "xpu.kpbuf." + entryName);
                llvm::Constant* szInit = llvm::ConstantDataArray::get(
                    ctx, llvm::ArrayRef<uint32_t>(sizes.data(), sizes.size()));
                auto* szGV = new llvm::GlobalVariable(
                    hostModule, szInit->getType(), /*isConstant=*/true,
                    llvm::GlobalValue::PrivateLinkage, szInit,
                    "xpu.kpsz." + entryName);
                b.CreateCall(kpFn, {nameStr,
                                    llvm::ConstantInt::get(i32Ty,
                                                           (uint32_t) info.size()),
                                    isBufGV, szGV});
            }
            b.CreateRetVoid();

            // Run at module-init time (LLJIT: jit->initialize; native: startup).
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
