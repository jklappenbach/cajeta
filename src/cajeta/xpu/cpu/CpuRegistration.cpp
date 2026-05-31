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
#include "CpuBackend.h"
#include "cajeta/compile/Optimizer.h"

#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
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

        // Host TargetMachine — supplies TargetTransformInfo so LoopVectorize can
        // cost-model the per-block wrapper's work-item loop (Inc 5B). One TM for
        // all kernels; null is tolerated (vectorization just won't fire).
        std::unique_ptr<llvm::TargetMachine> hostTm = createCpuTargetMachine();

        // The per-block wrapper takes 6 coordinate params (ctaid.{x,y,z},
        // ntid.{x,y,z}); the 3 tid coords become its internal work-item loop var.
        const unsigned kNumBlockCoordParams = 6;

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
            linked->addFnAttr(llvm::Attribute::AlwaysInline);   // inline into loop

            llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
            llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
            llvm::FunctionType* kfnTy = linked->getFunctionType();
            const unsigned total = kfnTy->getNumParams();
            const unsigned nReal = total - kNumCoordParams;     // buffers + scalars

            // --- Per-block wrapper (POCL model, Inc 5B) ---------------------
            // void __cajeta_xpu_cpu_block.<name>(real params...,
            //        i32 ctaid.x, ctaid.y, ctaid.z, ntid.x, ntid.y, ntid.z)
            // Loops the work-item index tid.x over [0, ntid.x) calling the
            // per-work-item kernel; the kernel is then inlined and the loop is
            // handed to LoopVectorize → SIMD. 1-D over tid.x (the launch ABI is
            // 1-D: ntid.y/z == 1); tid.y/z are 0.
            std::vector<llvm::Type*> wtys;
            wtys.reserve(nReal + kNumBlockCoordParams);
            for (unsigned i = 0; i < nReal; ++i)
                wtys.push_back(kfnTy->getParamType(i));
            for (unsigned i = 0; i < kNumBlockCoordParams; ++i)
                wtys.push_back(i32);
            llvm::Function* wrapper = llvm::Function::Create(
                llvm::FunctionType::get(voidTy, wtys, false),
                llvm::GlobalValue::ExternalLinkage,
                "__cajeta_xpu_cpu_block." + entryName, hostModule);

            llvm::Value* ctaidX = wrapper->getArg(nReal + 0);
            llvm::Value* ctaidY = wrapper->getArg(nReal + 1);
            llvm::Value* ctaidZ = wrapper->getArg(nReal + 2);
            llvm::Value* ntidX  = wrapper->getArg(nReal + 3);
            llvm::Value* ntidY  = wrapper->getArg(nReal + 4);
            llvm::Value* ntidZ  = wrapper->getArg(nReal + 5);

            llvm::BasicBlock* wEntry =
                llvm::BasicBlock::Create(ctx, "entry", wrapper);
            llvm::BasicBlock* wHead =
                llvm::BasicBlock::Create(ctx, "wi.head", wrapper);
            llvm::BasicBlock* wBody =
                llvm::BasicBlock::Create(ctx, "wi.body", wrapper);
            llvm::BasicBlock* wExit =
                llvm::BasicBlock::Create(ctx, "wi.exit", wrapper);

            b.SetInsertPoint(wEntry);
            b.CreateBr(wHead);

            b.SetInsertPoint(wHead);
            llvm::PHINode* tid = b.CreatePHI(i32, 2, "tid.x");
            tid->addIncoming(llvm::ConstantInt::get(i32, 0), wEntry);
            b.CreateCondBr(b.CreateICmpSLT(tid, ntidX, "wi.cond"), wBody, wExit);

            b.SetInsertPoint(wBody);
            llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
            std::vector<llvm::Value*> kArgs;
            kArgs.reserve(total);
            for (unsigned i = 0; i < nReal; ++i) kArgs.push_back(wrapper->getArg(i));
            kArgs.push_back(tid);                     // tid.x
            kArgs.push_back(zero);                    // tid.y
            kArgs.push_back(zero);                    // tid.z
            kArgs.push_back(ctaidX); kArgs.push_back(ctaidY); kArgs.push_back(ctaidZ);
            kArgs.push_back(ntidX);  kArgs.push_back(ntidY);  kArgs.push_back(ntidZ);
            llvm::CallInst* kcall = b.CreateCall(linked, kArgs);
            llvm::Value* tidNext =
                b.CreateAdd(tid, llvm::ConstantInt::get(i32, 1), "tid.next");
            tid->addIncoming(tidNext, wBody);
            b.CreateBr(wHead);

            b.SetInsertPoint(wExit);
            b.CreateRetVoid();

            // Inline the per-work-item kernel into the loop body, then vectorize
            // the loop (mem2reg + LoopVectorize). This is what turns the CPU
            // backend's wave from width-1 to native SIMD width.
            llvm::InlineFunctionInfo ifi;
            llvm::InlineFunction(*kcall, ifi);
            vectorizeFunction(*wrapper, hostTm.get());

            // --- Uniform launcher thunk → the per-block wrapper -------------
            // void __cajeta_xpu_cpu_launch.<name>(ptr argv, ptr coord). The
            // runtime calls it ONCE PER BLOCK; coord still carries 9 i32s, but
            // the wrapper consumes only ctaid.{x,y,z} (coord[3..5]) and
            // ntid.{x,y,z} (coord[6..8]) — the work-item loop lives in the
            // wrapper. argv[i] points to the i-th real argument's value (the
            // cuLaunch/hipModuleLaunch kernelParams convention).
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
            std::vector<llvm::Value*> callArgs;
            callArgs.reserve(nReal + kNumBlockCoordParams);
            for (unsigned i = 0; i < nReal; ++i) {
                llvm::Value* slotPtr = b.CreateInBoundsGEP(
                    ptrTy, argvArg, llvm::ConstantInt::get(i64, i), "argv.slot");
                llvm::Value* slot = b.CreateLoad(ptrTy, slotPtr, "argv.ptr");
                callArgs.push_back(b.CreateLoad(kfnTy->getParamType(i), slot, "arg"));
            }
            for (unsigned j = 3; j < kNumCoordParams; ++j) {   // ctaid+ntid xyz
                llvm::Value* cPtr = b.CreateInBoundsGEP(
                    i32, coordArg, llvm::ConstantInt::get(i64, j), "coord.slot");
                callArgs.push_back(b.CreateLoad(i32, cPtr, "coord.val"));
            }
            b.CreateCall(wrapper, callArgs);
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
