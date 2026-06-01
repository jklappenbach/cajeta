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
#include "CpuBarrierFission.h"
#include "cajeta/compile/Optimizer.h"

#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/error/Exception.h"

#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdlib>

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

// Debug seam: when CAJETA_XPU_CPU_NO_VECTORIZE is set in the environment, skip
// the work-item LoopVectorize pass on the per-block wrapper. Correctness is
// unaffected (the scalar work-item loops are already complete); this only lets
// tooling/tests inspect the fission output before loop-rotate/vectorize fold
// and rename the per-region loop blocks. Off by default.
bool cpuVectorizeDisabled() {
    return std::getenv("CAJETA_XPU_CPU_NO_VECTORIZE") != nullptr;
}

// --- Increment 5C: wave-op SIMD via the Vector Function ABI ----------------
//
// A wave op lowers (in CpuKernelLowering) to a scalar call to its
// `__cajeta_xpu_wave_*` stub. These helpers give that scalar call a SIMD
// *vector variant* and force the per-block work-item loop to vectorize at the
// host's native vector width W, so LoopVectorize substitutes the variant and
// the wave runs as W SIMD lanes. If W < 2 or the host TM is absent, nothing is
// attached and the scalar (width-1) call runs — always correct.

// The host's native i32 fixed-vector width (16 on AVX-512, 8 on AVX2, 4 on
// SSE2). 0 if no TM. This is the wave width on the CPU backend.
unsigned cpuVectorWidthI32(llvm::TargetMachine* tm, llvm::Function& f) {
    if (!tm) return 0;
    llvm::TargetTransformInfo tti = tm->getTargetTransformInfo(f);
    llvm::TypeSize bits =
        tti.getRegisterBitWidth(llvm::TargetTransformInfo::RGK_FixedWidthVector);
    if (bits.isScalable() || bits.getFixedValue() < 32) return 0;
    return (unsigned) (bits.getFixedValue() / 32);
}

// True iff `f` calls the named runtime wave stub.
bool callsRuntimeFn(llvm::Function& f, llvm::StringRef name) {
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = call->getCalledFunction())
                    if (cf->getName() == name) return true;
    return false;
}

// Create an empty `internal alwaysinline memory(none)` variant function with
// the given name + signature; the caller fills the body. alwaysinline so
// release builds fold it into the vectorized loop.
llvm::Function* makeVariantShell(llvm::Module& m, const std::string& name,
                                 llvm::FunctionType* fnTy) {
    if (auto* existing = m.getFunction(name)) return existing;
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, name, &m);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);
    fn->setDoesNotThrow();
    fn->setWillReturn();
    fn->setMemoryEffects(llvm::MemoryEffects::none());
    return fn;
}

// One wave op's width-W SIMD variants. The scalar stub gets a
// `vector-function-abi-variant` attribute naming both an unmasked (`_vW`) and a
// masked (`_Mv16`) variant. LoopVectorize uses the unmasked one in uniform code
// and the masked one when the call is in divergent (if-guarded) control flow,
// passing the active-lane predicate as the trailing mask argument — matching
// GPU active-mask semantics.
//
// The variant bodies are synthesized in IR from portable vector ops
// (reduce/select/bitcast/gather), so they legalize to real SIMD on any host.
void attachWaveVariants(llvm::Module& m, llvm::StringRef scalarName,
                        unsigned W) {
    llvm::Function* scalar = m.getFunction(scalarName);
    if (!scalar) return;
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
    auto* maskTy = llvm::FixedVectorType::get(i1, W);
    const std::string sw = std::to_string(W);

    // The VFABI parameter-token string (one char per scalar parameter); built
    // alongside each op. `_ZGV_LLVM_N<W><tokens>_<scalar>(<unmasked>)` and the
    // `M` (masked) form which appends the predicate.
    std::string tokens;
    llvm::Function* unmasked = nullptr;
    llvm::Function* masked = nullptr;

    if (scalarName == "__cajeta_xpu_wave_reduce_sum_u32") {
        tokens = "v";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        // <W x i32> v(<W x i32> x) = broadcast(reduce.add(x))
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* s = b.CreateAddReduce(unmasked->getArg(0));
            b.CreateRet(b.CreateVectorSplat(W, s));
        }
        // <W x i32> Mv(<W x i32> x, <W x i1> m) = broadcast(reduce.add(m?x:0))
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(vTy, {vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* sel = b.CreateSelect(masked->getArg(1), masked->getArg(0),
                                              llvm::Constant::getNullValue(vTy));
            llvm::Value* s = b.CreateAddReduce(sel);
            b.CreateRet(b.CreateVectorSplat(W, s));
        }
    } else if (scalarName == "__cajeta_xpu_wave_ballot_sync") {
        tokens = "v";
        auto* pTy = llvm::FixedVectorType::get(i1, W);     // per-lane predicate
        auto* rTy = llvm::FixedVectorType::get(i64, W);    // broadcast mask
        auto* iW = llvm::Type::getIntNTy(ctx, W);
        // <W x i64> v(<W x i1> p) = broadcast(zext(bitcast<W x i1>->iW))
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(rTy, {pTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", unmasked));
            llvm::Value* bits = b.CreateBitCast(unmasked->getArg(0), iW);
            llvm::Value* z = b.CreateZExt(bits, i64);
            b.CreateRet(b.CreateVectorSplat(W, z));
        }
        // masked: ballot over active lanes only → bitcast(p & m)
        masked = makeVariantShell(m, scalarName.str() + "_Mv" + sw,
                                  llvm::FunctionType::get(rTy, {pTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* act = b.CreateAnd(masked->getArg(0), masked->getArg(1));
            llvm::Value* bits = b.CreateBitCast(act, iW);
            llvm::Value* z = b.CreateZExt(bits, i64);
            b.CreateRet(b.CreateVectorSplat(W, z));
        }
    } else if (scalarName == "__cajeta_xpu_wave_shuffle_sync_u32") {
        tokens = "vv";
        auto* vTy = llvm::FixedVectorType::get(i32, W);
        // <W x i32> v(<W x i32> val, <W x i32> src): result[i] = val[src[i]] —
        // a per-lane dynamic gather within the wave, unrolled W times.
        auto build = [&](llvm::Function* fn) {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
            llvm::Value* val = fn->getArg(0);
            llvm::Value* src = fn->getArg(1);
            llvm::Value* res = llvm::PoisonValue::get(vTy);
            for (unsigned i = 0; i < W; ++i) {
                llvm::Value* lane =
                    b.CreateExtractElement(src, b.getInt32(i));   // i32 src lane
                llvm::Value* picked = b.CreateExtractElement(val, lane);
                res = b.CreateInsertElement(res, picked, b.getInt32(i));
            }
            b.CreateRet(res);
        };
        unmasked = makeVariantShell(m, scalarName.str() + "_v" + sw,
                                    llvm::FunctionType::get(vTy, {vTy, vTy}, false));
        build(unmasked);
        // masked: same gather; LoopVectorize blends inactive result lanes back
        // to their pre-call value, so the mask need not change the computation.
        masked = makeVariantShell(
            m, scalarName.str() + "_Mv" + sw,
            llvm::FunctionType::get(vTy, {vTy, vTy, maskTy}, false));
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", masked));
            llvm::Value* val = masked->getArg(0);
            llvm::Value* src = masked->getArg(1);
            llvm::Value* res = llvm::PoisonValue::get(vTy);
            for (unsigned i = 0; i < W; ++i) {
                llvm::Value* lane = b.CreateExtractElement(src, b.getInt32(i));
                llvm::Value* picked = b.CreateExtractElement(val, lane);
                res = b.CreateInsertElement(res, picked, b.getInt32(i));
            }
            b.CreateRet(res);
        }
    } else {
        return;
    }

    const std::string base = "_ZGV_LLVM_N" + sw + tokens + "_" + scalarName.str();
    const std::string mbase = "_ZGV_LLVM_M" + sw + tokens + "_" + scalarName.str();
    const std::string attr =
        base + "(" + unmasked->getName().str() + ")," +
        mbase + "(" + masked->getName().str() + ")";
    scalar->addFnAttr("vector-function-abi-variant", attr);
}

// `width()` takes no argument, so it cannot carry a VFABI variant (a 0-param
// variant is rejected by the verifier). In a vectorized wave kernel the wave
// width IS W (the architectural width, like a GPU warp size — constant across
// active and inactive lanes), so rewrite each `__cajeta_xpu_wave_width()` call
// to the constant W. Returns the number rewritten.
unsigned rewriteWaveWidth(llvm::Function& f, unsigned W) {
    llvm::SmallVector<llvm::CallInst*, 4> calls;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = c->getCalledFunction())
                    if (cf->getName() == "__cajeta_xpu_wave_width")
                        calls.push_back(c);
    llvm::Value* wConst =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.getContext()), W);
    for (auto* c : calls) {
        c->replaceAllUsesWith(wConst);
        c->eraseFromParent();
    }
    return calls.size();
}

// Force `latch` (a work-item loop's back-edge branch) to vectorize at width W
// via self-referential `!llvm.loop` metadata — so the forced VF equals the
// variant's VF and LoopVectorize substitutes it.
void forceLoopVectorWidth(llvm::BranchInst* latch, unsigned W) {
    llvm::LLVMContext& ctx = latch->getContext();
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    auto md = [&](const char* key, llvm::Constant* val) {
        return llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, key),
                                       llvm::ConstantAsMetadata::get(val)});
    };
    llvm::SmallVector<llvm::Metadata*, 4> ops;
    ops.push_back(nullptr);  // self-reference, patched below
    ops.push_back(md("llvm.loop.vectorize.width", llvm::ConstantInt::get(i32, W)));
    ops.push_back(md("llvm.loop.vectorize.enable", llvm::ConstantInt::getTrue(ctx)));
    llvm::MDNode* loopId = llvm::MDNode::getDistinct(ctx, ops);
    loopId->replaceOperandWith(0, loopId);
    latch->setMetadata("llvm.loop", loopId);
}

// The wave-op runtime stubs that carry a SIMD VFABI variant. `width()` is handled
// separately (rewritten to the constant W), and lane_id/is_first_lane lower inline.
static const char* const kWaveOps[] = {
    "__cajeta_xpu_wave_reduce_sum_u32",
    "__cajeta_xpu_wave_ballot_sync",
    "__cajeta_xpu_wave_shuffle_sync_u32",
};

// Attach SIMD variants for every wave op `f` uses and (if any, or `width()` is
// used) report it a wave kernel. Shared by the single-loop (5C) and barrier
// (fission) paths.
bool setupWaveVariants(llvm::Function& f, llvm::Module& m, unsigned waveW) {
    bool waveKernel = false;
    for (const char* op : kWaveOps)
        if (callsRuntimeFn(f, op)) {
            attachWaveVariants(m, op, waveW);
            waveKernel = true;
        }
    if (callsRuntimeFn(f, "__cajeta_xpu_wave_width")) waveKernel = true;
    return waveKernel;
}

// Fold the LoopVectorize-substituted wave variant calls (alwaysinline) into the
// loop so codegen emits the reduce/gather/ballot SIMD directly rather than a
// per-iteration call (no inliner runs at -O0).
void foldWaveVariants(llvm::Function& f) {
    llvm::SmallVector<llvm::CallInst*, 16> vcalls;
    for (auto& bb : f)
        for (auto& in : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&in))
                if (auto* cf = c->getCalledFunction())
                    if (cf->hasFnAttribute(llvm::Attribute::AlwaysInline)
                        && cf->getName().contains("__cajeta_xpu_wave_"))
                        vcalls.push_back(c);
    for (auto* c : vcalls) {
        llvm::InlineFunctionInfo vifi;
        llvm::InlineFunction(*c, vifi);
    }
}

} // namespace

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
            wtys.reserve(nReal + kNumBlockCoordParams + 1);
            for (unsigned i = 0; i < nReal; ++i)
                wtys.push_back(kfnTy->getParamType(i));
            for (unsigned i = 0; i < kNumBlockCoordParams; ++i)
                wtys.push_back(i32);
            wtys.push_back(i32);            // dynamic shared-memory byte count
            llvm::Function* wrapper = llvm::Function::Create(
                llvm::FunctionType::get(voidTy, wtys, false),
                llvm::GlobalValue::ExternalLinkage,
                "__cajeta_xpu_cpu_block." + entryName, hostModule);
            // The trailing wrapper param is the dynamic shared byte count; a
            // `shared T[runtimeN]` array allocas it per block (else unused → DCE).
            llvm::Value* dynSharedBytes = wrapper->getArg(nReal + kNumBlockCoordParams);

            llvm::Value* ctaidX = wrapper->getArg(nReal + 0);
            llvm::Value* ctaidY = wrapper->getArg(nReal + 1);
            llvm::Value* ctaidZ = wrapper->getArg(nReal + 2);
            llvm::Value* ntidX  = wrapper->getArg(nReal + 3);
            llvm::Value* ntidY  = wrapper->getArg(nReal + 4);
            llvm::Value* ntidZ  = wrapper->getArg(nReal + 5);

            // A kernel that calls Barrier.workgroup() can't run as one work-item
            // loop — it is split at each barrier into regions, each looped over
            // the block (Inc 6 fission). XPU-N02 on an unsupported construct →
            // discard + fall back to the host stub.
            // The non-barrier wrapper runs a 3-D work-item loop nest, so it
            // handles a multi-dim block. Barrier (fission) kernels are still 1-D
            // (their work-item loops iterate tid.x only) until 2D/3D stage 4, so
            // they are marked "no 3-D block" and the runtime rejects a multi-dim
            // block launch of them with a diagnostic rather than miscompiling.
            const bool isBarrierKernel = usesBarrier(*linked);
            if (isBarrierKernel) {
                std::vector<llvm::BranchInst*> wiLatches;
                try {
                    std::vector<llvm::Value*> ctaidV = {ctaidX, ctaidY, ctaidZ};
                    std::vector<llvm::Value*> ntidV  = {ntidX,  ntidY,  ntidZ};
                    fissionBarrierKernel(linked, wrapper, nReal, ctaidV, ntidV,
                                         hostModule, &wiLatches, dynSharedBytes);
                } catch (cajeta::Exception&) {
                    wrapper->eraseFromParent();
                    linked->eraseFromParent();    // has barrier markers; unusable
                    continue;                     // host-stub fallback
                }
                linked->eraseFromParent();        // body cloned into the wrapper

                // Wave + barrier composition (Inc 9): each fission region is a
                // clean counted work-item loop, so vectorize them at width W with
                // the wave SIMD variants exactly as the 5C single-loop path does —
                // a wave op inside a region becomes W SIMD lanes, the barriers
                // delimit regions. Forcing VF=W on every region loop is safe (the
                // block width is uniform across regions); only wave-bearing
                // regions carry the wave-width-divides-block assumption (as in 5C).
                const unsigned waveW = cpuVectorWidthI32(hostTm.get(), *wrapper);
                bool waveKernel = false;
                if (waveW >= 2) {
                    waveKernel = setupWaveVariants(*wrapper, hostModule, waveW);
                    if (waveKernel) {
                        rewriteWaveWidth(*wrapper, waveW);
                        for (llvm::BranchInst* latch : wiLatches)
                            forceLoopVectorWidth(latch, waveW);
                    }
                }
                if (!cpuVectorizeDisabled())
                    vectorizeFunction(*wrapper, hostTm.get());
                if (waveKernel) foldWaveVariants(*wrapper);
            } else {
            // 3-D work-item loop nest over (tid.z, tid.y, tid.x). The innermost
            // tid.x loop is the vectorizable/wave loop — so a 1-D block
            // (ntid.y/z == 1) runs identically to before (the z/y loops are
            // single-trip), and a 2-D/3-D block runs the whole nest (2D/3D launch
            // stage 3). The kernel is called once per work-item with its full 3-D
            // tid coordinate.
            llvm::BasicBlock* wEntry =
                llvm::BasicBlock::Create(ctx, "entry", wrapper);
            llvm::BasicBlock* zHead =
                llvm::BasicBlock::Create(ctx, "wi.z.head", wrapper);
            llvm::BasicBlock* yHead =
                llvm::BasicBlock::Create(ctx, "wi.y.head", wrapper);
            llvm::BasicBlock* wHead =
                llvm::BasicBlock::Create(ctx, "wi.head", wrapper);
            llvm::BasicBlock* wBody =
                llvm::BasicBlock::Create(ctx, "wi.body", wrapper);
            llvm::BasicBlock* yLatch =
                llvm::BasicBlock::Create(ctx, "wi.y.latch", wrapper);
            llvm::BasicBlock* zLatch =
                llvm::BasicBlock::Create(ctx, "wi.z.latch", wrapper);
            llvm::BasicBlock* wExit =
                llvm::BasicBlock::Create(ctx, "wi.exit", wrapper);
            llvm::Constant* zero = llvm::ConstantInt::get(i32, 0);
            llvm::Constant* one  = llvm::ConstantInt::get(i32, 1);

            b.SetInsertPoint(wEntry);
            b.CreateBr(zHead);

            b.SetInsertPoint(zHead);
            llvm::PHINode* tz = b.CreatePHI(i32, 2, "tid.z");
            tz->addIncoming(zero, wEntry);
            b.CreateCondBr(b.CreateICmpSLT(tz, ntidZ, "wi.z.cond"), yHead, wExit);

            b.SetInsertPoint(yHead);
            llvm::PHINode* ty = b.CreatePHI(i32, 2, "tid.y");
            ty->addIncoming(zero, zHead);
            b.CreateCondBr(b.CreateICmpSLT(ty, ntidY, "wi.y.cond"), wHead, zLatch);

            b.SetInsertPoint(wHead);
            llvm::PHINode* tid = b.CreatePHI(i32, 2, "tid.x");
            tid->addIncoming(zero, yHead);
            b.CreateCondBr(b.CreateICmpSLT(tid, ntidX, "wi.cond"), wBody, yLatch);

            b.SetInsertPoint(wBody);
            std::vector<llvm::Value*> kArgs;
            kArgs.reserve(total);
            for (unsigned i = 0; i < nReal; ++i) kArgs.push_back(wrapper->getArg(i));
            kArgs.push_back(tid);                     // tid.x
            kArgs.push_back(ty);                      // tid.y
            kArgs.push_back(tz);                      // tid.z
            kArgs.push_back(ctaidX); kArgs.push_back(ctaidY); kArgs.push_back(ctaidZ);
            kArgs.push_back(ntidX);  kArgs.push_back(ntidY);  kArgs.push_back(ntidZ);
            llvm::CallInst* kcall = b.CreateCall(linked, kArgs);
            tid->addIncoming(b.CreateAdd(tid, one, "tid.next"), wBody);
            llvm::BranchInst* latchBr = b.CreateBr(wHead);   // inner (x) back-edge

            b.SetInsertPoint(yLatch);
            ty->addIncoming(b.CreateAdd(ty, one, "tid.y.next"), yLatch);
            b.CreateBr(yHead);

            b.SetInsertPoint(zLatch);
            tz->addIncoming(b.CreateAdd(tz, one, "tid.z.next"), zLatch);
            b.CreateBr(zHead);

            b.SetInsertPoint(wExit);
            b.CreateRetVoid();

            // --- Wave-op SIMD setup (Inc 5C) -------------------------------
            // If the kernel uses wave ops, give each its VFABI vector variants
            // (unmasked + masked) and force the work-item loop to vectorize at
            // the host's native width W (the CPU wave width) so LoopVectorize
            // substitutes the SIMD variant — the wave becomes W SIMD lanes.
            // Attribute + variant + loop-width metadata go in BEFORE inlining;
            // the loop metadata rides along when InlineFunction moves the latch.
            const unsigned waveW = cpuVectorWidthI32(hostTm.get(), *wrapper);
            bool waveKernel = false;
            if (waveW >= 2) {
                waveKernel = setupWaveVariants(*linked, hostModule, waveW);
                if (waveKernel) forceLoopVectorWidth(latchBr, waveW);
            }

            // Inline the per-work-item kernel into the loop body, then vectorize
            // the loop (mem2reg + LoopVectorize). This is what turns the CPU
            // backend's wave from width-1 to native SIMD width.
            llvm::InlineFunctionInfo ifi;
            llvm::InlineFunction(*kcall, ifi);

            // The wave width is W in a vectorized wave kernel (architectural,
            // like a warp size) — rewrite width() calls to the constant before
            // vectorizing so the value is uniform and folds cleanly.
            if (waveKernel) rewriteWaveWidth(*wrapper, waveW);

            vectorizeFunction(*wrapper, hostTm.get());

            // Fold the now-substituted `_vW`/`_Mv16` wave variant calls into the
            // loop (they are alwaysinline, but no inliner runs at -O0) so codegen
            // emits the reduce/gather/ballot SIMD directly rather than a
            // per-iteration call.
            if (waveKernel) foldWaveVariants(*wrapper);
            }   // end of the barrier-free single-loop wrapper build

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
            // coord[9]: the dynamic shared-memory byte count (the launch's
            // `sharedBytes:`), passed as the wrapper's trailing param.
            llvm::Value* dynPtr = b.CreateInBoundsGEP(
                i32, coordArg, llvm::ConstantInt::get(i64, kNumCoordParams),
                "coord.dyn.slot");
            callArgs.push_back(b.CreateLoad(i32, dynPtr, "coord.dyn"));
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
            // Barrier kernels are 1-D-block-only (fission); mark them so the
            // runtime guards a multi-dim-block launch.
            if (isBarrierKernel) {
                llvm::FunctionCallee no3dFn = hostModule.getOrInsertFunction(
                    "__cajeta_xpu_cpu_kernel_no_3d_block",
                    llvm::FunctionType::get(voidTy, {ptrTy}, false));
                b.CreateCall(no3dFn, {nameStr});
            }
            b.CreateRetVoid();

            llvm::appendToGlobalCtors(hostModule, ctor, /*priority=*/65535);
            ++emitted;
        }
        return emitted;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
