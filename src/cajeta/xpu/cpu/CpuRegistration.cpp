//
// CPU kernel registration pass — see header.
//
// For each @Kernel: lower it to a host function in a fresh module that shares
// the host module's LLVMContext (so the two can be linked), decorate the symbol
// to avoid clashing with the kernel's host stub, link it into the host module,
// then append an llvm.global_ctors entry calling
// __cajeta_xpu_register_cpu_kernel(entryName, &fn). A mid-lowering XPU-N01
// throw discards the fresh module, leaving the host module untouched.
//

#include "CpuRegistration.h"
#include "CpuKernelLowering.h"

#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace cajeta {
namespace xpu {
namespace cpu {

    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& /*arch*/) {
        if (kernels.empty()) return 0;

        llvm::LLVMContext& ctx = hostModule.getContext();
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::IRBuilder<> b(ctx);

        // void __cajeta_xpu_register_cpu_kernel(i8* name, i8* fn)
        llvm::FunctionType* regTy =
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
        llvm::FunctionCallee regFn = hostModule.getOrInsertFunction(
            "__cajeta_xpu_register_cpu_kernel", regTy);

        int emitted = 0;
        for (auto& method : kernels) {
            if (!method || !isKernel(*method)) continue;
            const std::string entryName = method->getName();
            const std::string sym = "__cajeta_xpu_cpu." + entryName;

            // Lower into a fresh module sharing the host context (so it can be
            // linked). A throw here destroys `mod` with the host untouched.
            auto mod = std::make_unique<llvm::Module>("xpu.cpu." + entryName, ctx);
            mod->setDataLayout(hostModule.getDataLayout());
            mod->setTargetTriple(hostModule.getTargetTriple());
            llvm::Function* kfn = nullptr;
            try {
                kfn = lowerKernel(method, *mod);
            } catch (cajeta::Exception&) {
                continue;  // unsupported construct → leave to the host path
            }
            if (!kfn) continue;
            kfn->setName(sym);
            kfn->setLinkage(llvm::GlobalValue::ExternalLinkage);

            if (llvm::Linker::linkModules(hostModule, std::move(mod))) {
                continue;  // link error (logged by the linker)
            }
            llvm::Function* linked = hostModule.getFunction(sym);
            if (!linked) continue;

            // ctor: __cajeta_xpu_register_cpu_kernel(entryName, &fn)
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_cpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.cpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, linked});
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
