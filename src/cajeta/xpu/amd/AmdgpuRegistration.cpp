//
// AMDGPU kernel registration pass — see header.
//
// Structurally identical to NvptxRegistration: for each @Kernel it lowers a
// device module, assembles a binary (hsaco here, cubin there), embeds the
// bytes as a private host-module constant, and appends an llvm.global_ctors
// entry calling the backend-neutral __cajeta_xpu_register_module(entryName,
// bytes, len). The runtime keys modules by entry name, so the same launch
// path resolves an AMD kernel exactly as it does an NVIDIA one — only the
// binary format behind it changed.
//

#include "AmdgpuRegistration.h"
#include "AmdgpuBackend.h"
#include "AmdgpuKernelLowering.h"

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
namespace amd {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch) {
        if (kernels.empty()) return 0;

        // One AMDGPU TargetMachine for all kernels (arch-, not kernel-,
        // specific). If the amdgcn target isn't in this LLVM build there's
        // nothing to emit.
        auto tm = createAmdgpuTargetMachine(arch);
        if (!tm) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // void __cajeta_xpu_register_module(i8* name, i8* image, i64 len)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty}, false);
        llvm::FunctionCallee regFn =
            hostModule.getOrInsertFunction("__cajeta_xpu_register_module", regTy);

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();

            // Lower this kernel into a fresh device module + assemble an hsaco.
            // The device lowerer builds types in its own context, so this never
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

            std::vector<uint8_t> hsaco = assembleHsaco(devMod, *tm, arch);
            if (hsaco.empty()) continue;  // lld missing or errored

            // Embed the hsaco as a private host-module constant.
            llvm::Constant* dataInit = llvm::ConstantDataArray::get(
                ctx, llvm::ArrayRef<uint8_t>(hsaco.data(), hsaco.size()));
            auto* hsacoGV = new llvm::GlobalVariable(
                hostModule, dataInit->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, dataInit,
                "xpu.hsaco." + entryName);
            hsacoGV->setAlignment(llvm::MaybeAlign(8));

            // ctor: __cajeta_xpu_register_module(entryName, hsacoGV, len)
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, hsacoGV,
                                 llvm::ConstantInt::get(i64Ty, hsaco.size())});
            b.CreateRetVoid();

            // Run at module-init time (LLJIT: jit->initialize; native: startup).
            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace amd
} // namespace xpu
} // namespace cajeta
