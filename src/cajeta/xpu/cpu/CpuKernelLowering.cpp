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
// coordinates as the 12 trailing i32 kernel args and these reads pull them back.
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

    // Grid-stride stride (Item 6 Stage 2). Total work-items in `dim` =
    // gridDim·blockDim = nctaid·ntid. The SIMT backends read these from hardware;
    // the CPU now carries the 4th coord group, gridDim (nctaid = block count),
    // threaded from the launch ABI (runtime → thunk → wrapper → kernel). Returns
    // i32, matching the other coord reads.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return b.CreateMul(coord(b, /*group=*/9, dim),   // nctaid.{x,y,z}
                           coord(b, /*group=*/6, dim),   // ntid.{x,y,z}
                           "xpu.gridsize");
    }

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
            // Buffers and Texture2D handles are flat host pointers; a Sampler
            // rides by value as its {i32,i32} struct (p.type); scalars by value.
            tys.push_back((p.isBuffer || p.isTexture)
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
            "ctaid.z", "ntid.x", "ntid.y", "ntid.z",
            "nctaid.x", "nctaid.y", "nctaid.z"};
        for (unsigned c = 0; c < kNumCoordParams; ++c)
            fn->getArg(i++)->setName(kCoordNames[c]);
        decorateKernel(fn, m);
        return fn;
    }
    // materializeParam (fn->getArg) + bufferElementPtr (GEP) defaults are
    // correct: host pointers are addrspace 0, and the coordinate params live
    // past the materialized param indices, so they never collide.

    // A @Device helper's Buffer<T> param is a flat (addrspace 0) host pointer —
    // matching the buffer base createKernel hands the kernel (Item 2).
    llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* /*elemTy*/) override {
        return llvm::PointerType::get(m.getContext(), 0);
    }

    // Texture sampling (Item 8): emit a call to the C runtime bilinear/nearest
    // sampler. `texHandle` is the host texobj pointer (the materialized texture
    // param); `samplerHandle` is the {i32 filterMode, i32 addressMode} struct
    // value (the materialized sampler param) — unpacked to two i32s here so the
    // runtime sees a flat signature. The math (clamp/wrap addressing + filtering)
    // lives in C, where it is easy to get right; the IR stays a single call.
    //
    //   float __cajeta_xpu_cpu_tex_sample(ptr tex, i32 filterMode,
    //                                     i32 addressMode, float u, float v)
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle, llvm::Value* samplerHandle,
                               llvm::Value* u, llvm::Value* v,
                               llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        // The C sampler returns the full RGBA texel as a <4 x float> and takes the
        // explicit mip `lod` (0.0 for the plain sample). Single-channel formats
        // expand G/B = 0, A = 1, so Texture2D.sample yields a Vector<float32,4>.
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, f32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_tex_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee,
                            {texHandle, filterMode, addressMode, u, v, lod},
                            "tex.sample");
    }

    // Texture2D.fetch(x, y) (texelFetch): the unfiltered, sampler-free exact
    // texel read. Emits a call to the C runtime `__cajeta_xpu_cpu_tex_fetch_rgba`,
    // which indexes the decoded float store directly (no addressing/filtering) —
    // the texel-read primitive sampleTexture's bilinear path is built on. No
    // Sampler arg (x, y are i32 texel indices).
    //
    //   <4 x float> __cajeta_xpu_cpu_tex_fetch_rgba(ptr tex, i32 x, i32 y)
    //   <4 x i32>   __cajeta_xpu_cpu_tex_fetch_rgba_i32(ptr tex, i32 x, i32 y)
    // The integer variant reinterprets the texel store as raw i32 (the upload
    // memcpy'd the integer bits in verbatim) — for Texture2D<int32>/<uint32>.
    llvm::Value* fetchTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                              llvm::Value* texHandle, llvm::Value* x,
                              llvm::Value* y, llvm::Type* texelTy,
                              llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        bool isInt = texelTy && texelTy->isIntegerTy();
        auto* v4t = llvm::FixedVectorType::get(
            isInt ? i32 : (llvm::Type*) llvm::Type::getFloatTy(ctx), 4);
        const char* sym = isInt ? "__cajeta_xpu_cpu_tex_fetch_rgba_i32"
                                : "__cajeta_xpu_cpu_tex_fetch_rgba";
        // The C fetch takes the explicit mip `lod` (0 for the plain fetch).
        auto* fnTy = llvm::FunctionType::get(
            v4t, {llvm::PointerType::get(ctx, 0), i32, i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(sym, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, x, y, lod}, "tex.fetch");
    }

    // Texture3D.sample(sampler, u, v, w): the 3-D trilinear sampler — calls the
    // C runtime __cajeta_xpu_cpu_tex3d_sample_rgba (a third coord vs the 2-D
    // sampler), returning the filtered <4 x float> voxel.
    llvm::Value* sampleTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u, llvm::Value* v,
                                 llvm::Value* w) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, f32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_tex3d_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, filterMode, addressMode, u, v, w},
                            "tex3d.sample");
    }

    // Texture3D.fetch(x, y, z): the 3-D unfiltered voxel read — float or integer
    // variant by texel type, mirroring the 2-D fetch.
    //   <4 x float> __cajeta_xpu_cpu_tex3d_fetch_rgba(ptr, i32, i32, i32)
    //   <4 x i32>   __cajeta_xpu_cpu_tex3d_fetch_rgba_i32(ptr, i32, i32, i32)
    llvm::Value* fetchTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Value* y, llvm::Value* z,
                                llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        bool isInt = texelTy && texelTy->isIntegerTy();
        auto* v4t = llvm::FixedVectorType::get(
            isInt ? i32 : (llvm::Type*) llvm::Type::getFloatTy(ctx), 4);
        const char* sym = isInt ? "__cajeta_xpu_cpu_tex3d_fetch_rgba_i32"
                                : "__cajeta_xpu_cpu_tex3d_fetch_rgba";
        auto* fnTy = llvm::FunctionType::get(
            v4t, {llvm::PointerType::get(ctx, 0), i32, i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(sym, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, x, y, z}, "tex3d.fetch");
    }

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
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
        // Mirror reduceSum: a pure runtime wave stub whose VFABI vector variant
        // (CpuRegistration) does the cross-lane reduce when the kernel widens.
        const char* sym;
        switch (op) {
            case WaveReduceOp::Max: sym = "__cajeta_xpu_wave_reduce_max_u32"; break;
            case WaveReduceOp::Min: sym = "__cajeta_xpu_wave_reduce_min_u32"; break;
            case WaveReduceOp::And: sym = "__cajeta_xpu_wave_reduce_and_u32"; break;
            case WaveReduceOp::Or:  sym = "__cajeta_xpu_wave_reduce_or_u32"; break;
            case WaveReduceOp::Xor: sym = "__cajeta_xpu_wave_reduce_xor_u32"; break;
        }
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.reduce");
    }
    llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                          WaveScanOp op, llvm::Value* value) override {
        // A pure runtime stub whose VFABI vector variant does the in-lane
        // exclusive prefix scan when the kernel widens (the base Hillis-Steele
        // default's shuffle loop doesn't vectorize on the CPU wave model).
        const char* sym = op == WaveScanOp::Sum
            ? "__cajeta_xpu_wave_prefix_sum_u32"
            : "__cajeta_xpu_wave_prefix_product_u32";
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.scan");
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

    // Read coordinate (group + dim) from the 12 trailing kernel args, laid out
    // [tid.xyz, ctaid.xyz, ntid.xyz, nctaid.xyz]. group ∈ {0,3,6,9}, dim ∈ {0,1,2}.
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
