//
// NVPTX kernel lowering — see header.
//
// Since the AMD bring-up (cajeta-amd.md), the ~885-line AST walk lives in the
// shared xpu/lowering/KernelLowering.cpp. This file is now just the NVPTX
// LoweringTarget — the measured NVIDIA half of the variance surface — plus a
// thin wrapper preserving the nvidia::lowerKernel(method, module) API.
//

#include "NvptxKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

class NvptxTarget : public LoweringTarget {
public:
    const char* name() const override { return "nvptx"; }

    // No native inline ray query (cajeta has no NVPTX RT-core / OptiX seam), so
    // the AccelerationStructure noun is built as the portable software BVH and
    // the RayQuery verb follows to the cajeta.gpu.core.SoftwareRayQuery walk over
    // a software BVH buffer — the same Portable tier the CPU backend uses
    // (ray-query-to-core inc 1). Without this the base default (VulkanNative)
    // routes RayQuery to rayQueryType(), which throws on NVPTX. softwareRayQuery()
    // derives from this in the base; coopMatrixTier stays the base Portable
    // (flat-tile matmul) since there is no NVPTX native wmma seam yet.
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

    // NVPTX allocas live in the generic address space (0); mem2reg removes
    // most before PTX emit.
    unsigned allocaAddressSpace() const override { return 0; }

    llvm::Value* threadId(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x,
            llvm::Intrinsic::nvvm_read_ptx_sreg_tid_y,
            llvm::Intrinsic::nvvm_read_ptx_sreg_tid_z};
        return readSreg(b, m, ids[dim]);
    }
    llvm::Value* workgroupId(llvm::IRBuilderBase& b, llvm::Module& m,
                             unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_x,
            llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_y,
            llvm::Intrinsic::nvvm_read_ptx_sreg_ctaid_z};
        return readSreg(b, m, ids[dim]);
    }
    llvm::Value* workgroupDim(llvm::IRBuilderBase& b, llvm::Module& m,
                              unsigned dim) override {
        static const llvm::Intrinsic::ID ids[3] = {
            llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_x,
            llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_y,
            llvm::Intrinsic::nvvm_read_ptx_sreg_ntid_z};
        return readSreg(b, m, ids[dim]);
    }
    // Grid-stride stride = nctaid·ntid (number of CTAs × CTA size).
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module& m,
                          unsigned dim) override {
        static const llvm::Intrinsic::ID nctaid[3] = {
            llvm::Intrinsic::nvvm_read_ptx_sreg_nctaid_x,
            llvm::Intrinsic::nvvm_read_ptx_sreg_nctaid_y,
            llvm::Intrinsic::nvvm_read_ptx_sreg_nctaid_z};
        return b.CreateMul(readSreg(b, m, nctaid[dim]),
                           workgroupDim(b, m, dim), "gridsize");
    }

    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        // bar.sync 0 — synchronize all threads in the CTA.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all);
        b.CreateCall(f, {llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(m.getContext()), 0)});
    }

    void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m, FenceScope scope,
                     MemoryOrder /*order*/ = MemoryOrder::Default) override {
        // membar — a memory fence with no bar.sync (no thread rendezvous).
        // membar.cta orders within the CTA (workgroup); membar.gl orders
        // global memory across the device. membar is a full fence with no
        // weaker variant, so the requested order doesn't change the op.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, scope == FenceScope::Workgroup
                    ? llvm::Intrinsic::nvvm_membar_cta
                    : llvm::Intrinsic::nvvm_membar_gl);
        b.CreateCall(f, {});
    }

    void devicePrintf(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* fmt,
                      llvm::ArrayRef<llvm::Value*> args) override {
        // CUDA device printf is the external `i32 vprintf(i8* fmt, i8* args)`:
        // the varargs are packed (declaration order, f32→double per varargs
        // promotion) into a stack buffer whose address is the second operand.
        // Emit-only (no CUDA hardware here).
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* ptr = llvm::PointerType::get(ctx, 0);
        llvm::Value* argBuf = llvm::ConstantPointerNull::get(ptr);
        if (!args.empty()) {
            std::vector<llvm::Type*> ftys;
            std::vector<llvm::Value*> vals;
            ftys.reserve(args.size());
            vals.reserve(args.size());
            for (llvm::Value* a : args) {
                llvm::Value* v = a;
                if (a->getType()->isFloatTy())
                    v = b.CreateFPExt(a, llvm::Type::getDoubleTy(ctx), "printf.f2d");
                ftys.push_back(v->getType());
                vals.push_back(v);
            }
            auto* bufTy = llvm::StructType::get(ctx, ftys);
            llvm::Value* buf = b.CreateAlloca(bufTy, nullptr, "printf.args");
            for (unsigned i = 0; i < vals.size(); ++i)
                b.CreateStore(vals[i], b.CreateStructGEP(bufTy, buf, i));
            argBuf = buf;
        }
        auto* fnTy = llvm::FunctionType::get(i32, {ptr, ptr}, /*vararg=*/false);
        llvm::FunctionCallee vp = m.getOrInsertFunction("vprintf", fnTy);
        b.CreateCall(vp, {fmt, argBuf});
    }

    void decorateKernel(llvm::Function* fn, llvm::Module& m) override {
        llvm::LLVMContext& ctx = m.getContext();
        fn->setCallingConv(llvm::CallingConv::PTX_Kernel);
        // nvvm.annotations kernel marker (belt-and-suspenders alongside the CC).
        llvm::Metadata* ops[] = {
            llvm::ValueAsMetadata::get(fn),
            llvm::MDString::get(ctx, "kernel"),
            llvm::ValueAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1)),
        };
        m.getOrInsertNamedMetadata("nvvm.annotations")
            ->addOperand(llvm::MDNode::get(ctx, ops));
    }

    // Wave ops: NVIDIA warps are 32 wide; shuffle + ballot are hardware.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return readSreg(b, m, llvm::Intrinsic::nvvm_read_ptx_sreg_warpsize);
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                             llvm::Value* value, llvm::Value* srcLane) override {
        // shfl.sync.idx.b32: full-warp membermask, idx mode clamp 0x1f.
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_shfl_sync_idx_i32);
        return b.CreateCall(f, {llvm::ConstantInt::get(i32, 0xFFFFFFFFu), value,
                                srcLane, llvm::ConstantInt::get(i32, 0x1Fu)},
                            "shfl");
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
        // vote.ballot.sync over the full warp → i32, widened to the i64 API.
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_vote_ballot_sync);
        llvm::Value* bits = b.CreateCall(
            f, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0xFFFFFFFFu),
                pred}, "ballot");
        return b.CreateZExt(bits, llvm::Type::getInt64Ty(ctx));
    }
    llvm::Value* waveReduceSum(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* value) override {
        // redux.sync.add.s32: full-warp membermask. Requires sm_80+ (Ampere).
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_redux_sync_add);
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0xFFFFFFFFu)},
                            "redux");
    }
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
        // redux.sync.{umax,umin,and,or,xor}: full-warp membermask, sm_80+.
        // Unsigned min/max for the uint32 surface. (Emit-only — no NV device.)
        llvm::Intrinsic::ID id;
        switch (op) {
            case WaveReduceOp::Max: id = llvm::Intrinsic::nvvm_redux_sync_umax; break;
            case WaveReduceOp::Min: id = llvm::Intrinsic::nvvm_redux_sync_umin; break;
            case WaveReduceOp::And: id = llvm::Intrinsic::nvvm_redux_sync_and; break;
            case WaveReduceOp::Or:  id = llvm::Intrinsic::nvvm_redux_sync_or; break;
            case WaveReduceOp::Xor: id = llvm::Intrinsic::nvvm_redux_sync_xor; break;
        }
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0xFFFFFFFFu)},
                            "redux");
    }
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return readSreg(b, m, llvm::Intrinsic::nvvm_read_ptx_sreg_laneid);
    }

    // tex.sample(sampler, u, v) → llvm.nvvm.tex.unified.2d.v4f32.f32 (Item 8
    // Stage D, emit-only). NVIDIA's "unified" texture fetch takes the i64
    // cudaTextureObject_t — which bundles the image AND the sampler state — so
    // the separate Sampler kernel arg is unused here (as on AMD). The default
    // textureParamType (i64) already gives the handle by value. Returns
    // {float,float,float,float}; v1 takes the R channel. No NVIDIA hardware
    // here — proven via the PTX `tex.2d` instruction in the emit test.
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle,
                               llvm::Value* /*samplerHandle*/, llvm::Value* u,
                               llvm::Value* v, llvm::Value* /*lod*/) override {
        llvm::Function* tex = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_tex_unified_2d_v4f32_f32);
        llvm::Value* rgba = b.CreateCall(tex, {texHandle, u, v}, "tex.rgba");
        // The unified tex intrinsic returns a {f32,f32,f32,f32} struct; repack
        // it as a <4 x float> so Texture2D.sample yields a Vector<float32,4>
        // (the caller selects a channel with .r/.x).
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* vec = llvm::PoisonValue::get(v4f);
        for (unsigned i = 0; i < 4; ++i)
            vec = b.CreateInsertElement(
                vec, b.CreateExtractValue(rgba, {i}), uint64_t(i),
                i == 3 ? "tex.sample" : "");
        return vec;
    }

    // Image2D storage images (the writable twin of Texture2D, emit-only until the
    // NVIDIA runner / B5). img.store/img.load → llvm.nvvm.sust.b.2d.i32.trap /
    // llvm.nvvm.suld.2d.i32.trap (the surface store/load PTX ops). The handle is
    // the i64 cudaSurfaceObject_t (the default textureParamType, by value); there
    // is no sampler. Two NVPTX specifics vs the Vulkan/AMD paths: surface coords
    // are BYTE offsets in x (x*4 for the 4-byte R32 texel; y stays a row index),
    // and the texel rides as raw i32 bits (the R32f image preserves them — bitcast
    // f32<->i32). The CUDA surface RUNTIME (cuSurfObjectCreate + the launch
    // marshalling) lands with B5; this is the compiler lowering, proven via PTX.
    void storeImage(llvm::IRBuilderBase& b, llvm::Module& m,
                    llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
                    llvm::Value* value) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* xb =
            b.CreateShl(x, llvm::ConstantInt::get(i32, 2), "img.xbytes");
        llvm::Value* vi = b.CreateBitCast(value, i32, "img.bits");
        llvm::Function* st = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_sust_b_2d_i32_trap);
        b.CreateCall(st, {imgHandle, xb, y, vi});
    }

    llvm::Value* loadImage(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* imgHandle, llvm::Value* x,
                           llvm::Value* y) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Value* xb =
            b.CreateShl(x, llvm::ConstantInt::get(i32, 2), "img.xbytes");
        llvm::Function* ld = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_suld_2d_i32_trap);
        llvm::Value* raw = b.CreateCall(ld, {imgHandle, xb, y}, "img.raw");
        return b.CreateBitCast(raw, f32, "img.load");
    }

    // Shader clock: the 64-bit SM clock (clock64) — the NVIDIA analogue of
    // OpReadClockKHR for Thread.clock(). Emit-only until the NVIDIA runner (B5).
    llvm::Value* readClock(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_read_ptx_sreg_clock64);
        return b.CreateCall(f, {}, "clock");
    }

    // Transcendentals via NVIDIA libdevice: `__nv_<fn>f` (f32) / `__nv_<fn>`
    // (f64). NVPTX, like AMDGPU, has no IEEE transcendental instructions; the
    // libdevice call is the canonical path (linked at cubin time). Emit-only
    // until the NVIDIA runner lands (B5); rsqrt is the native nvvm intrinsic.
    llvm::Value* transcendental(llvm::IRBuilderBase& b, llvm::Module& m,
                                const std::string& name,
                                llvm::ArrayRef<llvm::Value*> args) override {
        llvm::Type* ft = args[0]->getType();
        // libdevice is scalar-only — vectorized math scalarizes per lane.
        if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(ft)) {
            llvm::Value* acc = llvm::UndefValue::get(vt);
            for (unsigned i = 0; i < vt->getNumElements(); ++i) {
                std::vector<llvm::Value*> lane;
                for (llvm::Value* a : args)
                    lane.push_back(b.CreateExtractElement(a, i));
                acc = b.CreateInsertElement(acc,
                    transcendental(b, m, name, lane), i);
            }
            return acc;
        }
        if (name == "rsqrt") {
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::nvvm_rsqrt_approx_f, {});
            // nvvm_rsqrt_approx_f is f32; cast in/out if a double slipped through.
            llvm::Value* x = ft->isFloatTy() ? args[0]
                : b.CreateFPCast(args[0], llvm::Type::getFloatTy(m.getContext()));
            llvm::Value* r = b.CreateCall(fn, {x});
            return ft->isFloatTy() ? r : b.CreateFPCast(r, ft);
        }
        std::string sym = "__nv_" + name + (ft->isDoubleTy() ? "" : "f");
        std::vector<llvm::Type*> params(args.size(), ft);
        auto* fnTy = llvm::FunctionType::get(ft, params, false);
        llvm::FunctionCallee fn = m.getOrInsertFunction(sym, fnTy);
        return b.CreateCall(fn,
            std::vector<llvm::Value*>(args.begin(), args.end()), "nv.math");
    }

private:
    static llvm::Value* readSreg(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Intrinsic::ID id) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {}, "sreg");
    }
};

} // namespace

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    NvptxTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
