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

            // Uniform launcher thunk — the CPU's kernelParams ABI, mirroring
            // cuLaunch/hipModuleLaunch: argv[i] points to the i-th argument's
            // value; coord is the per-work-item i32[9] (tid/ctaid/ntid xyz). The
            // thunk unpacks argv + coord and calls the real kernel, giving the
            // driver/dispatcher one signature it can call for any kernel. Built
            // from the kernel's FunctionType alone: the last kNumCoordParams i32s
            // are coordinates; the rest are real params (pointer ⇒ buffer ptr,
            // value ⇒ scalar), so no KernelParam list is threaded through.
            //   void __cajeta_xpu_cpu_launch.<name>(ptr argv, ptr coord)
            llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            llvm::FunctionType* thunkTy =
                llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
            llvm::Function* thunk = llvm::Function::Create(
                thunkTy, llvm::GlobalValue::ExternalLinkage,
                "__cajeta_xpu_cpu_launch." + entryName, hostModule);
            thunk->getArg(0)->setName("argv");
            thunk->getArg(1)->setName("coord");
            llvm::Value* argvArg = thunk->getArg(0);
            llvm::Value* coordArg = thunk->getArg(1);

            llvm::BasicBlock* tb = llvm::BasicBlock::Create(ctx, "entry", thunk);
            b.SetInsertPoint(tb);

            llvm::FunctionType* kfnTy = linked->getFunctionType();
            const unsigned total = kfnTy->getNumParams();
            const unsigned nReal = total - kNumCoordParams;
            std::vector<llvm::Value*> callArgs;
            callArgs.reserve(total);
            // Real params: arg_i = *(paramType_i*)(argv[i]).
            for (unsigned i = 0; i < nReal; ++i) {
                llvm::Value* slotPtr = b.CreateInBoundsGEP(
                    ptrTy, argvArg, llvm::ConstantInt::get(i64, i), "argv.slot");
                llvm::Value* slot = b.CreateLoad(ptrTy, slotPtr, "argv.ptr");
                callArgs.push_back(
                    b.CreateLoad(kfnTy->getParamType(i), slot, "arg"));
            }
            // Coordinates: arg_{nReal+j} = coord[j].
            for (unsigned j = 0; j < kNumCoordParams; ++j) {
                llvm::Value* cPtr = b.CreateInBoundsGEP(
                    i32, coordArg, llvm::ConstantInt::get(i64, j), "coord.slot");
                callArgs.push_back(b.CreateLoad(i32, cPtr, "coord.val"));
            }
            b.CreateCall(linked, callArgs);
            b.CreateRetVoid();

            // ctor: __cajeta_xpu_register_cpu_kernel(entryName, &launchThunk)
            llvm::FunctionType* ctorTy = llvm::FunctionType::get(voidTy, false);
            llvm::Function* ctor = llvm::Function::Create(
                ctorTy, llvm::GlobalValue::InternalLinkage,
                "__cajeta_xpu_cpu_reg_ctor." + entryName, hostModule);
            llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", ctor);
            b.SetInsertPoint(bb);
            llvm::Value* nameStr =
                b.CreateGlobalString(entryName, "xpu.cpu.kname." + entryName);
            b.CreateCall(regFn, {nameStr, thunk});
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
