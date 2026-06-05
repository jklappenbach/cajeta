//
// AMDGPU kernel lowering — see header.
//

#include "AmdgpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"

namespace cajeta {
namespace xpu {
namespace amd {

namespace {

class AmdgpuTarget : public LoweringTarget {
public:
    const char* name() const override { return "amdgpu"; }

    // AMDGPU allocas MUST live in the private address space (5). An AS-0
    // alloca here is invalid IR for the AMDGPU backend — the classic first
    // bug (cajeta-amd.md §2). mem2reg removes most of them before ISA emit;
    // any survivor is a valid private (scratch) slot.
    unsigned allocaAddressSpace() const override { return 5; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::amdgcn_workitem_id_x,
            llvm::Intrinsic::amdgcn_workitem_id_y,
            llvm::Intrinsic::amdgcn_workitem_id_z};
        return readId(b, m, ids[dim]);
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                             unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::amdgcn_workgroup_id_x,
            llvm::Intrinsic::amdgcn_workgroup_id_y,
            llvm::Intrinsic::amdgcn_workgroup_id_z};
        return readId(b, m, ids[dim]);
    }

    // Block dim (ntid) is NOT an intrinsic on AMDGPU — it comes from the HSA
    // kernel dispatch packet (cajeta-amd.md §2). llvm.amdgcn.dispatch.ptr
    // returns a ptr addrspace(4) to that packet; workgroup_size_{x,y,z} are
    // uint16 fields at byte offsets 4/6/8 (after the 2-byte header + 2-byte
    // setup). Load the i16 and widen to i32 to match the other coordinates.
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                              unsigned dim) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Function* dp = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_dispatch_ptr);
        llvm::Value* packet = b.CreateCall(dp, {}, "dispatch.ptr");
        llvm::Value* field = b.CreateConstGEP1_32(
            llvm::Type::getInt8Ty(ctx), packet, 4 + 2 * dim, "wgsize.ptr");
        llvm::Value* sz16 = b.CreateLoad(llvm::Type::getInt16Ty(ctx), field,
                                         "wgsize");
        return b.CreateZExt(sz16, llvm::Type::getInt32Ty(ctx), "wgsize.i32");
    }

    // Grid-stride stride (Item 6). grid_size_{x,y,z} are uint32 fields of the
    // HSA dispatch packet at byte offsets 12/16/20 — and already hold the TOTAL
    // work-item count in each dim (gridDim·blockDim), so this is one load (no
    // multiply needed, unlike NVPTX). Same dispatch.ptr addrspace(4) packet as
    // workgroupDim above.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Function* dp = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_dispatch_ptr);
        llvm::Value* packet = b.CreateCall(dp, {}, "dispatch.ptr");
        llvm::Value* field = b.CreateConstGEP1_32(
            llvm::Type::getInt8Ty(ctx), packet, 12 + 4 * dim, "gridsize.ptr");
        return b.CreateLoad(llvm::Type::getInt32Ty(ctx), field, "gridsize");
    }

    // Workgroup barrier with LDS-visibility ordering: a workgroup-scoped
    // release fence, the hardware s_barrier, then a workgroup-scoped acquire
    // fence — the same shape HIP's __syncthreads() lowers to, so shared-memory
    // writes before the barrier are visible to reads after it.
    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::SyncScope::ID wg = ctx.getOrInsertSyncScopeID("workgroup");
        b.CreateFence(llvm::AtomicOrdering::Release, wg);
        llvm::Function* bar = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_s_barrier);
        b.CreateCall(bar, {});
        b.CreateFence(llvm::AtomicOrdering::Acquire, wg);
    }

    // AMDGPU marks kernels purely by calling convention — no metadata
    // analogue to nvvm.annotations.
    void decorateKernel(llvm::Function* fn, llvm::Module& /*m*/) override {
        fn->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);
    }

    // A Texture2D kernel param is a pointer to the HIP texture object, in the
    // constant address space (4) — the kernarg holds the 64-bit object address,
    // read through the scalar cache, exactly as HIP's tex2D casts it. The object
    // is { image SRD (12 dwords) | sampler SRD (8 dwords) }; sampleTexture reads
    // both. (Item 8 Stage C.)
    llvm::Type* textureParamType(llvm::Module& m) override {
        return llvm::PointerType::get(m.getContext(), 4);
    }

    // tex.sample(sampler, u, v) → __ockl_image_sample_2D (ROCm device library,
    // linked in by AmdgpuBackend when this symbol is referenced). It takes the
    // image object ptr (= texHandle) and the sampler object ptr (= texHandle +
    // HIP_SAMPLER_OBJECT_OFFSET_DWORD·4 = +48 bytes) — both addrspace(4) — plus
    // the normalized <u, v>; it converts coords to unnormalized + emits
    // image_sample_lz internally (handling the gfx ISA variants). Returns the
    // <4 x float> gather; v1 takes the R channel. The separate Sampler kernel
    // arg (samplerHandle) is unused on AMD — its modes are baked into the texture
    // object at hipCreateTextureObject time, so the sampler SRD rides the object.
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle,
                               llvm::Value* /*samplerHandle*/, llvm::Value* u,
                               llvm::Value* v) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v2f = llvm::FixedVectorType::get(f32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        // sampler object sits HIP_SAMPLER_OBJECT_OFFSET_DWORD (12) dwords in.
        llvm::Value* sampPtr =
            b.CreateConstGEP1_32(i8, texHandle, 48, "tex.samp.obj");
        llvm::Value* coord = llvm::PoisonValue::get(v2f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1), "tex.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, v2f}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_sample_2D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, sampPtr, coord},
                                         "tex.sample.rgba");
        return b.CreateExtractElement(rgba, uint64_t(0), "tex.sample");
    }

    // Transcendentals via the ROCm OpenCL math library (ocml): `__ocml_<fn>_f32`
    // (or `_f64`). AMDGPU mis-lowers `llvm.sin`/etc. (no range reduction), so we
    // emit the ocml call directly; AmdgpuBackend links ocml.bc when these
    // `__ocml_` declarations are present. rsqrt is a native amdgcn intrinsic.
    llvm::Value* transcendental(llvm::IRBuilderBase& b, llvm::Module& m,
                                const std::string& name,
                                llvm::ArrayRef<llvm::Value*> args) override {
        llvm::Type* ft = args[0]->getType();
        if (name == "rsqrt") {
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::amdgcn_rsq, {ft});
            return b.CreateCall(fn, {args[0]});
        }
        const char* suffix = ft->isDoubleTy() ? "_f64" : "_f32";
        std::string sym = "__ocml_" + name + suffix;
        std::vector<llvm::Type*> params(args.size(), ft);
        auto* fnTy = llvm::FunctionType::get(ft, params, false);
        llvm::FunctionCallee fn = m.getOrInsertFunction(sym, fnTy);
        return b.CreateCall(fn,
            std::vector<llvm::Value*>(args.begin(), args.end()), "ocml.call");
    }

    // Wave ops. Wavefront size is target-/feature-dependent (32 or 64 on
    // RDNA; default 32 for compute here). readlane is shuffle-by-index; ballot
    // returns the wave-width mask (i32 in the wave32 default), widened to i64.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_wavefrontsize);
        return b.CreateCall(f, {}, "wavesize");
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                             llvm::Value* value, llvm::Value* srcLane) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_readlane, {i32});
        return b.CreateCall(f, {value, srcLane}, "readlane");
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_ballot, {llvm::Type::getInt32Ty(ctx)});
        llvm::Value* bits = b.CreateCall(f, {pred}, "ballot");
        return b.CreateZExt(bits, llvm::Type::getInt64Ty(ctx));
    }
    llvm::Value* waveReduceSum(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* value) override {
        // wave.reduce.add over i32; strategy operand 0 = default lowering.
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_wave_reduce_add, {i32});
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0)}, "wavered");
    }
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // The canonical AMDGPU lane-id idiom: mbcnt counts set bits of the
        // exec-relative mask below this lane. hi(~0, lo(~0, 0)) = this lane's
        // index within the wavefront (handles both wave32 and wave64).
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* allOnes = llvm::ConstantInt::get(i32, 0xFFFFFFFFu);
        llvm::Function* lo = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_mbcnt_lo);
        llvm::Function* hi = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_mbcnt_hi);
        llvm::Value* lowCount =
            b.CreateCall(lo, {allOnes, llvm::ConstantInt::get(i32, 0)}, "mbcnt.lo");
        return b.CreateCall(hi, {allOnes, lowCount}, "wave.laneid");
    }

private:
    static llvm::Value* readId(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Intrinsic::ID id) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {}, "id");
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    AmdgpuTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace amd
} // namespace xpu
} // namespace cajeta
