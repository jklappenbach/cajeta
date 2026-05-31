//
// CPU kernel lowering — see header. The host LoweringTarget + thin wrapper.
//

#include "CpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "../../error/Exception.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ModRef.h"

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

// The CPU LoweringTarget. The SIMT backends read coordinates from hardware
// intrinsics; the CPU has none, so the grid→threads model passes a work-item's
// coordinates as the 9 trailing i32 kernel args and these reads pull them back.
class CpuTarget : public LoweringTarget {
public:
    const char* name() const override { return "cpu"; }

    // Flat host address space.
    unsigned allocaAddressSpace() const override { return 0; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return coord(b, /*group=*/0, dim);     // tid.{x,y,z}
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module&,
                             unsigned dim) override {
        return coord(b, /*group=*/3, dim);     // ctaid.{x,y,z}
    }
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module&,
                              unsigned dim) override {
        return coord(b, /*group=*/6, dim);     // ntid.{x,y,z}
    }
    // globalId uses the shared default: ctaid*ntid + tid.

    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // A CPU workgroup barrier is realized by work-item loop fission in the
        // registration pass (cajeta-cpu.md Inc 6): this marker call delimits the
        // regions the fission pass splits the work-item loop at. It is left
        // impure (default memory effects), noinline, and noduplicate so the
        // optimizer neither deletes nor clones/moves it before fission runs; the
        // pass erases every call once it has split the regions.
        llvm::LLVMContext& ctx = m.getContext();
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                             /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_barrier", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            f->addFnAttr(llvm::Attribute::NoInline);
            f->addFnAttr(llvm::Attribute::NoDuplicate);
            f->setDoesNotThrow();
        }
        b.CreateCall(callee, {});
    }

    void decorateKernel(llvm::Function*, llvm::Module&) override {
        // Default C calling convention + external linkage (set at creation) is
        // exactly what the host driver / JIT looks up. Nothing to mark.
    }

    // Buffers as flat addrspace(0) pointers + scalars by value, then the 9
    // trailing i32 coordinate params. (Overrides the addrspace(1) default.)
    llvm::Function* createKernel(
        llvm::Module& m, const std::string& name,
        const std::vector<KernelParam>& params) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size() + kNumCoordParams);
        for (auto& p : params) {
            tys.push_back(p.isBuffer
                              ? (llvm::Type*) llvm::PointerType::get(ctx, 0)
                              : p.type);
        }
        for (unsigned i = 0; i < kNumCoordParams; ++i) tys.push_back(i32);

        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), tys,
                                             /*vararg=*/false);
        auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                          name, &m);
        unsigned i = 0;
        for (auto& p : params) fn->getArg(i++)->setName(p.name);
        static const char* kCoordNames[kNumCoordParams] = {
            "tid.x", "tid.y", "tid.z", "ctaid.x", "ctaid.y",
            "ctaid.z", "ntid.x", "ntid.y", "ntid.z"};
        for (unsigned c = 0; c < kNumCoordParams; ++c)
            fn->getArg(i++)->setName(kCoordNames[c]);
        decorateKernel(fn, m);
        return fn;
    }
    // materializeParam (fn->getArg) + bufferElementPtr (GEP) defaults are
    // correct: host pointers are addrspace 0, and the coordinate params live
    // past the materialized param indices, so they never collide.

    // Wave ops. Each lowers to a *call* to its `__cajeta_xpu_wave_*` runtime
    // stub (width-1 scalar semantics: one work-item per host invocation). The
    // CPU registration pass then attaches a Vector Function ABI variant to each
    // stub so that when the per-block work-item loop is vectorized to the host's
    // native width W, LoopVectorize substitutes the SIMD `_vW` (or masked
    // `_Mv16` for divergent uses) variant — the wave becomes W SIMD lanes
    // (cajeta-cpu.md Inc 5C). If vectorization does not fire, the scalar call
    // runs — width-1, always correct. `width()` is the exception: it takes no
    // argument (a 0-arg VFABI variant is invalid), so registration rewrites it
    // to the constant W in a vectorized wave kernel.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_width", i32, {}, "wave.width");
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                             llvm::Value* value, llvm::Value* srcLane) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_shuffle_sync_u32", i32,
                        {value, srcLane}, "wave.shuffle");
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);
        if (!pred->getType()->isIntegerTy(1))      // normalize boolean → i1
            pred = b.CreateICmpNE(pred,
                                  llvm::ConstantInt::get(pred->getType(), 0));
        return pureCall(b, m, "__cajeta_xpu_wave_ballot_sync",
                        llvm::Type::getInt64Ty(ctx), {pred}, "wave.ballot");
    }
    llvm::Value* waveReduceSum(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* value) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_reduce_sum_u32", i32, {value},
                        "wave.reducesum");
    }
    // Lane within the wave = the block-local work-item index modulo the wave
    // width. width() is rewritten to the constant W in a vectorized wave kernel,
    // so this folds to `tid.x % W`; vectorized, the W-aligned vector induction
    // yields lanes 0..W-1. In a width-1 fallback width() → 1, so laneId → 0.
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return b.CreateURem(threadId(b, m, 0), waveWidth(b, m), "wave.laneid");
    }

private:
    // Emit a call to a pure (memory-none, willreturn, nounwind) runtime wave
    // stub — the marking LoopVectorize needs to be willing to widen the call
    // into its VFABI variant.
    static llvm::Value* pureCall(llvm::IRBuilderBase& b, llvm::Module& m,
                                 const char* name, llvm::Type* retTy,
                                 llvm::ArrayRef<llvm::Value*> args,
                                 const char* twine) {
        std::vector<llvm::Type*> argTys;
        argTys.reserve(args.size());
        for (auto* a : args) argTys.push_back(a->getType());
        auto* fnTy = llvm::FunctionType::get(retTy, argTys, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(name, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
            f->setDoesNotThrow();
            f->setWillReturn();
            f->setMemoryEffects(llvm::MemoryEffects::none());
        }
        auto* call = b.CreateCall(callee, args, twine);
        call->setDoesNotThrow();
        return call;
    }

    // Read coordinate (group + dim) from the 9 trailing kernel args, laid out
    // [tid.xyz, ctaid.xyz, ntid.xyz]. group ∈ {0,3,6}, dim ∈ {0,1,2}.
    static llvm::Value* coord(llvm::IRBuilderBase& b, unsigned group,
                              unsigned dim) {
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        unsigned n = fn->arg_size();
        return fn->getArg(n - kNumCoordParams + group + dim);
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method,
                            llvm::Module& deviceModule) {
    CpuTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
