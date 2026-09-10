//
// AMDGPU kernel lowering — see header.
//

#include "AmdgpuKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "../core/XpuKernelAttr.h"

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
    // `!nontemporal` becomes the slc / nt cache policy — a @Streaming buffer skips L2.
    bool supportsNontemporal() const override { return true; }

    // No AMDGPU inline-ray-query seam, so the Acceleration Structure is a software BVH.
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

    // AMDGPU allocas MUST be private (address space 5); an AS-0 alloca is invalid IR here.
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

    // Block dim is not an intrinsic here: llvm.amdgcn.dispatch.ptr gives the HSA dispatch
    // packet, whose workgroup_size_{x,y,z} are uint16 at byte offsets 4/6/8.
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

    // Grid-stride stride: grid_size_{x,y,z} are uint32 at byte offsets 12/16/20 of that
    // packet and already hold the TOTAL work-item count per dim — no multiply needed.
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

    // Release fence, s_barrier, acquire fence — the shape __syncthreads() lowers to.
    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::SyncScope::ID wg = ctx.getOrInsertSyncScopeID("workgroup");
        b.CreateFence(llvm::AtomicOrdering::Release, wg);
        llvm::Function* bar = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_s_barrier);
        b.CreateCall(bar, {});
        b.CreateFence(llvm::AtomicOrdering::Acquire, wg);
    }

    // A scoped fence at `order`, no s_barrier and so no rendezvous: the sync-scope name
    // sets the reach ("workgroup" vs "agent"), and Default/Relaxed becomes AcqRel.
    void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m, FenceScope scope,
                     MemoryOrder order = MemoryOrder::Default) override {
        llvm::SyncScope::ID sc = m.getContext().getOrInsertSyncScopeID(
            scope == FenceScope::Workgroup ? "workgroup" : "agent");
        llvm::AtomicOrdering ord =
            toAtomicOrdering(order, llvm::AtomicOrdering::AcquireRelease);
        if (ord == llvm::AtomicOrdering::Monotonic)
            ord = llvm::AtomicOrdering::AcquireRelease;
        b.CreateFence(ord, sc);
    }

    // True iff `arch` has the direct global->LDS load: GFX9/CDNA and gfx1250+ only.
    static bool archHasVmemToLds(llvm::StringRef arch) {
        if (arch.starts_with("gfx9")) return true;
        unsigned num = 0;
        if (arch.starts_with("gfx") && !arch.drop_front(3).getAsInteger(10, num))
            return num >= 1250;
        return false;
    }

    // Lowered once but codegen'd per arch, so it is usable only when EVERY arch has it.
    static bool bundleHasVmemToLds(llvm::Module& m) {
        auto readFlag = [&](const char* name) -> llvm::StringRef {
            if (auto* f = m.getModuleFlag(name))
                if (auto* s = llvm::dyn_cast<llvm::MDString>(f)) return s->getString();
            return {};
        };
        llvm::StringRef list = readFlag("cajeta.amdgpu.archlist");
        if (list.empty()) return archHasVmemToLds(readFlag("cajeta.amdgpu.arch"));
        llvm::SmallVector<llvm::StringRef, 4> arches;
        list.split(arches, ',', -1, /*KeepEmpty=*/false);
        for (auto a : arches)
            if (!archHasVmemToLds(a.trim())) return false;
        return !arches.empty();
    }

    // True iff `arch` has the native int8 dot unit (v_dot4). NOT a numeric threshold —
    // gfx940 fails while gfx942 works — and unknown arches answer NO: a wrong yes is an
    // ISel error, not a slow path.
    static bool archHasDot4(llvm::StringRef arch) {
        unsigned num = 0;
        if (!arch.starts_with("gfx") || arch.drop_front(3).getAsInteger(10, num))
            return false;
        if (num == 940) return false;            // early MI300; gfx942 has it
        if (num >= 900 && num < 1000) return num >= 906;
        if (num >= 1000 && num < 1100) return num >= 1011;   // gfx1010 lacks it
        return num >= 1100 && num < 1300;
    }

    // Same bundle rule as bundleHasVmemToLds: usable only when EVERY arch has it.
    static bool bundleHasDot4(llvm::Module& m) {
        auto readFlag = [&](const char* name) -> llvm::StringRef {
            if (auto* f = m.getModuleFlag(name))
                if (auto* s = llvm::dyn_cast<llvm::MDString>(f)) return s->getString();
            return {};
        };
        llvm::StringRef list = readFlag("cajeta.amdgpu.archlist");
        if (list.empty()) return archHasDot4(readFlag("cajeta.amdgpu.arch"));
        llvm::SmallVector<llvm::StringRef, 4> arches;
        list.split(arches, ',', -1, /*KeepEmpty=*/false);
        for (auto a : arches)
            if (!archHasDot4(a.trim())) return false;
        return !arches.empty();
    }

    // The native int8 dot: sudot4 when the operand signs DIFFER (it carries a sign bit per
    // operand, which sdot4/udot4 cannot express), else sdot4/udot4. Never clamped, so this
    // tier stays bit-identical with the portable widen it replaces.
    llvm::Value* integerDot4x8(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* a, llvm::Value* c, llvm::Value* acc,
                               bool aSigned, bool cSigned) override {
        if (!bundleHasDot4(m))
            return LoweringTarget::integerDot4x8(b, m, a, c, acc, aSigned,
                                                 cSigned);
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* x = b.CreateBitCast(a, i32, "dp4a.x");
        llvm::Value* y = b.CreateBitCast(c, i32, "dp4a.y");
        if (aSigned != cSigned) {
            llvm::Function* su = llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::amdgcn_sudot4);
            return b.CreateCall(su, {b.getInt1(aSigned), x,
                                     b.getInt1(cSigned), y, acc, b.getFalse()},
                                "dp4a.su");
        }
        llvm::Intrinsic::ID id = aSigned ? llvm::Intrinsic::amdgcn_sdot4
                                         : llvm::Intrinsic::amdgcn_udot4;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        return b.CreateCall(f, {x, y, acc, b.getFalse()}, "dp4a");
    }

    // 16-entry int8 LUT by 4-bit index via v_perm_b32: perm the low (0-7) and high (8-15)
    // table halves with the 3-bit index, then a third perm picks per byte off bit 3.
    llvm::Value* byteLut16(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* indices, llvm::Value* table) override {
        auto* ivt = llvm::dyn_cast<llvm::FixedVectorType>(indices->getType());
        if (ivt == nullptr || (ivt->getNumElements() % 4) != 0)
            return LoweringTarget::byteLut16(b, m, indices, table);
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        unsigned groups = ivt->getNumElements() / 4;
        auto* t4 = llvm::FixedVectorType::get(i32, 4);
        llvm::Value* tw = b.CreateBitCast(table, t4, "lut.tw");
        llvm::Value* t0 = b.CreateExtractElement(tw, (uint64_t) 0, "lut.t0");
        llvm::Value* t1 = b.CreateExtractElement(tw, (uint64_t) 1, "lut.t1");
        llvm::Value* t2 = b.CreateExtractElement(tw, (uint64_t) 2, "lut.t2");
        llvm::Value* t3 = b.CreateExtractElement(tw, (uint64_t) 3, "lut.t3");
        auto* iw = llvm::FixedVectorType::get(i32, groups);
        llvm::Value* idxW = b.CreateBitCast(indices, iw, "lut.iw");
        llvm::Function* perm = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_perm);
        llvm::Value* m7 = llvm::ConstantInt::get(i32, 0x07070707u);
        llvm::Value* m8 = llvm::ConstantInt::get(i32, 0x08080808u);
        llvm::Value* base = llvm::ConstantInt::get(i32, 0x03020100u);
        llvm::Value* out = llvm::UndefValue::get(iw);
        for (unsigned g = 0; g < groups; ++g) {
            llvm::Value* idx = b.CreateExtractElement(idxW, g, "lut.g");
            llvm::Value* sel = b.CreateAnd(idx, m7, "lut.sel");
            llvm::Value* lo = b.CreateCall(perm, {t1, t0, sel}, "lut.lo");
            llvm::Value* hi = b.CreateCall(perm, {t3, t2, sel}, "lut.hi");
            llvm::Value* mb = b.CreateOr(base,
                b.CreateLShr(b.CreateAnd(idx, m8), b.getInt32(1)), "lut.mb");
            llvm::Value* res = b.CreateCall(perm, {hi, lo, mb}, "lut.res");
            out = b.CreateInsertElement(out, res, g, "lut.out");
        }
        return b.CreateBitCast(out, ivt, "lut.bytes");
    }

    // Async global->LDS copy: where the LDS-direct load exists, global_load_lds per element
    // with NO VGPR staging, striped across the workgroup. Elements must be 1/2/4 bytes.
    void asyncCopy(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* dstBase,
                   llvm::Type* dstElem, llvm::Value* dstOffset, llvm::Value* srcBase,
                   llvm::Type* srcElem, llvm::Value* srcOffset,
                   llvm::Value* count) override {
        llvm::LLVMContext& ctx = m.getContext();
        uint64_t elemBytes = m.getDataLayout().getTypeStoreSize(srcElem);
        if (!bundleHasVmemToLds(m)
                || (elemBytes != 1 && elemBytes != 2 && elemBytes != 4)) {
            LoweringTarget::asyncCopy(b, m, dstBase, dstElem, dstOffset, srcBase,
                                      srcElem, srcOffset, count);
            return;
        }
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        auto i32of = [&](llvm::Value* v) { return b.CreateZExtOrTrunc(v, i32); };
        llvm::Value* tid  = i32of(threadId(b, m, 0));
        llvm::Value* nthr = i32of(workgroupDim(b, m, 0));
        llvm::Value* cnt  = i32of(count);
        llvm::Value* dOff = i32of(dstOffset);
        llvm::Value* sOff = i32of(srcOffset);
        llvm::Function* loadLds = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_global_load_lds);
        llvm::BasicBlock* pred = b.GetInsertBlock();
        auto* head = llvm::BasicBlock::Create(ctx, "asynccopy.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx, "asynccopy.body", fn);
        auto* exit = llvm::BasicBlock::Create(ctx, "asynccopy.exit", fn);
        b.CreateBr(head);
        b.SetInsertPoint(head);
        llvm::PHINode* e = b.CreatePHI(i32, 2, "asynccopy.e");
        e->addIncoming(tid, pred);
        b.CreateCondBr(b.CreateICmpULT(e, cnt), body, exit);
        b.SetInsertPoint(body);
        llvm::Value* sPtr = bufferElementPtr(   // addrspace(1) global source
            b, m, srcBase, srcElem, b.CreateZExt(b.CreateAdd(sOff, e), i64));
        llvm::Value* dPtr = bufferElementPtr(   // addrspace(3) LDS destination
            b, m, dstBase, dstElem, b.CreateZExt(b.CreateAdd(dOff, e), i64));
        b.CreateCall(loadLds, {sPtr, dPtr,
                               llvm::ConstantInt::get(i32, elemBytes),
                               llvm::ConstantInt::get(i32, 0),
                               llvm::ConstantInt::get(i32, 0)});
        e->addIncoming(b.CreateAdd(e, nthr), body);
        b.CreateBr(head);
        b.SetInsertPoint(exit);
    }

    // No async-mark on gfx1151 — there is no group counter to close.
    void asyncCommit(llvm::IRBuilderBase&, llvm::Module&) override {}

    // Drain the outstanding global_load_lds writes (a workgroup AcqRel fence the backend
    // lowers to s_waitcnt) so the caller's Barrier publishes landed data. A full drain.
    void asyncWait(llvm::IRBuilderBase& b, llvm::Module& m,
                   llvm::Value* /*groupsInFlight*/) override {
        llvm::SyncScope::ID wg =
            m.getContext().getOrInsertSyncScopeID("workgroup");
        b.CreateFence(llvm::AtomicOrdering::AcquireRelease, wg);
    }

    // Scheduling hints → the native amdgcn intrinsics. sched_barrier, sched_group_barrier
    // and iglp_opt are MachineScheduler directives that emit no ISA; s_setprio is real.
    void schedBarrier(llvm::IRBuilderBase& b, llvm::Module& m,
                      uint32_t mask) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_sched_barrier);
        b.CreateCall(f, {b.getInt32(mask)});
    }
    void schedGroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m,
                           uint32_t mask, uint32_t size,
                           uint32_t syncId) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_sched_group_barrier);
        b.CreateCall(f, {b.getInt32(mask), b.getInt32(size), b.getInt32(syncId)});
    }
    void schedPriority(llvm::IRBuilderBase& b, llvm::Module& m,
                       uint32_t level) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_s_setprio);
        b.CreateCall(f, {b.getInt16((uint16_t) level)});
    }
    void schedPipelineOpt(llvm::IRBuilderBase& b, llvm::Module& m,
                          uint32_t strategy) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_iglp_opt);
        b.CreateCall(f, {b.getInt32(strategy)});
    }

    // Conflict-free LDS swizzle for Swizzled<T,S>: the slot is row*S + (col ^ (row & S-1)).
    // The XOR touches only col's bits, so it is an involution and data reads back whole.
    llvm::Value* swizzleAddr(llvm::IRBuilderBase& b, llvm::Value* idx,
                             uint32_t stride) override {
        if (stride <= 1) return idx;
        llvm::Type* ty = idx->getType();
        unsigned log2S = llvm::Log2_32(stride);
        llvm::Value* mask = llvm::ConstantInt::get(ty, stride - 1);
        llvm::Value* shift = llvm::ConstantInt::get(ty, log2S);
        llvm::Value* row = b.CreateLShr(idx, shift, "swz.row");
        llvm::Value* col = b.CreateAnd(idx, mask, "swz.col");
        llvm::Value* perm = b.CreateXor(
            col, b.CreateAnd(row, mask), "swz.col2");
        return b.CreateOr(b.CreateShl(row, shift), perm, "swz.idx");
    }

    // Block-padded LDS tile: physical = idx + (idx / period) * pad. The byte period is
    // independent of the stride, so it de-conflicts the store and the transposed read.
    llvm::Value* blockPadAddr(llvm::IRBuilderBase& b, llvm::Value* idx,
                              uint32_t period, uint32_t pad) override {
        if (period == 0 || pad == 0) return idx;
        llvm::Type* ty = idx->getType();
        llvm::Value* blk = b.CreateUDiv(idx, llvm::ConstantInt::get(ty, period),
                                        "bp.blk");
        llvm::Value* off = b.CreateMul(blk, llvm::ConstantInt::get(ty, pad),
                                       "bp.off");
        return b.CreateAdd(idx, off, "bp.idx");
    }

    // AMDGPU marks kernels purely by calling convention — no annotations metadata.
    void decorateKernel(llvm::Function* fn, llvm::Module& /*m*/) override {
        fn->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);
    }

    // @Occupancy: maxThreads pins flat-work-group-size (the VGPR-budget lever on RDNA) and
    // minResident waves-per-eu; maxRegisters has no per-function AMDGPU attribute.
    void applyOccupancy(llvm::Function* fn, const XpuKernelAttr& attr) override {
        if (auto mt = attr.maxThreads()) {
            std::string range = "1," + std::to_string(*mt);
            fn->addFnAttr("amdgpu-flat-work-group-size", range);
        }
        if (auto mr = attr.minResident()) {
            fn->addFnAttr("amdgpu-waves-per-eu", std::to_string(*mr));
        }
    }

    // A texture kernel param points at the HIP texture object in the constant address
    // space (4): { image SRD (12 dwords) | sampler SRD (8 dwords) }, and sample reads both.
    llvm::Type* textureParamType(llvm::Module& m) override {
        return llvm::PointerType::get(m.getContext(), 4);
    }

    // tex.sample → __ockl_image_sample_lod_2D: image object ptr, sampler object ptr
    // (texHandle + 48 bytes), normalized <u,v>, float LOD. samplerHandle is unused here.
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle,
                               llvm::Value* /*samplerHandle*/, llvm::Value* u,
                               llvm::Value* v, llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v2f = llvm::FixedVectorType::get(f32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* sampPtr =
            b.CreateConstGEP1_32(i8, texHandle, 48, "tex.samp.obj");
        llvm::Value* coord = llvm::PoisonValue::get(v2f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1), "tex.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, v2f, f32}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_sample_lod_2D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, sampPtr, coord, lod},
                                         "tex.sample.rgba");
        return rgba;
    }

    // tex.fetch → __ockl_image_load_lod_2D, the unfiltered twin: image object ptr, integer
    // <x,y>, i32 mip level, no sampler and no normalization. The format still decodes.
    llvm::Value* fetchTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                              llvm::Value* texHandle, llvm::Value* x,
                              llvm::Value* y, llvm::Type* texelTy,
                              llvm::Value* lod) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "tex.fetch.coord");
        // An integer-format image loads RAW, so the v4f32 holds verbatim integer bits.
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, v2i, i32}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_load_lod_2D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, coord, lod},
                                         "tex.fetch.rgba");
        if (texelTy && texelTy->isIntegerTy()) {
            auto* v4i = llvm::FixedVectorType::get(i32, 4);
            return b.CreateBitCast(rgba, v4i, "tex.fetch.i32");
        }
        return rgba;
    }

    // Texture3D.sample → __ockl_image_sample_3D: the coord is <4 x float> {u, v, w, 0}.
    llvm::Value* sampleTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* /*samplerHandle*/,
                                 llvm::Value* u, llvm::Value* v,
                                 llvm::Value* w) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* sampPtr =
            b.CreateConstGEP1_32(i8, texHandle, 48, "tex.samp.obj");
        llvm::Value* zero = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* coord = llvm::PoisonValue::get(v4f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1));
        coord = b.CreateInsertElement(coord, w, uint64_t(2));
        coord = b.CreateInsertElement(coord, zero, uint64_t(3), "tex3d.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, v4f}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_sample_3D", fnTy);
        return b.CreateCall(s, {texHandle, sampPtr, coord}, "tex3d.sample.rgba");
    }

    // Texture3D.fetch → __ockl_image_load_3D: coord <4 x i32> {x, y, z, 0}, float result.
    llvm::Value* fetchTexture3D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Value* y, llvm::Value* z,
                                llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4i = llvm::FixedVectorType::get(i32, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
        llvm::Value* coord = llvm::PoisonValue::get(v4i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1));
        coord = b.CreateInsertElement(coord, z, uint64_t(2));
        coord = b.CreateInsertElement(coord, zero, uint64_t(3), "tex3d.fetch.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, v4i}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_load_3D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, coord}, "tex3d.fetch.rgba");
        if (texelTy && texelTy->isIntegerTy())
            return b.CreateBitCast(rgba, v4i, "tex3d.fetch.i32");
        return rgba;
    }

    // Texture1D.sample → __ockl_image_sample_1D: unlike 2-D/3-D the coord is a SCALAR
    // float, and there is no lod variant — mipmaps are 2-D only.
    llvm::Value* sampleTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle,
                                 llvm::Value* /*samplerHandle*/,
                                 llvm::Value* u) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* sampPtr =
            b.CreateConstGEP1_32(i8, texHandle, 48, "tex.samp.obj");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, f32}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_sample_1D", fnTy);
        return b.CreateCall(s, {texHandle, sampPtr, u}, "tex1d.sample.rgba");
    }

    // Texture1D.fetch → __ockl_image_load_1D: the coord is a SCALAR i32.
    llvm::Value* fetchTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4i = llvm::FixedVectorType::get(i32, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, i32}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_load_1D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, x}, "tex1d.fetch.rgba");
        if (texelTy && texelTy->isIntegerTy())
            return b.CreateBitCast(rgba, v4i, "tex1d.fetch.i32");
        return rgba;
    }

    // Texture2DArray.sample → __ockl_image_sample_2Da: the coord is <4 x float>
    // {u, v, layer, 0}, whose 3rd lane is the UN-normalized layer, converted from i32.
    llvm::Value* sampleTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* texHandle,
                                      llvm::Value* /*samplerHandle*/,
                                      llvm::Value* u, llvm::Value* v,
                                      llvm::Value* layer) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* sampPtr =
            b.CreateConstGEP1_32(i8, texHandle, 48, "tex.samp.obj");
        llvm::Value* layerF = b.CreateSIToFP(layer, f32, "layer.f");
        llvm::Value* zero = llvm::ConstantFP::get(f32, 0.0);
        llvm::Value* coord = llvm::PoisonValue::get(v4f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1));
        coord = b.CreateInsertElement(coord, layerF, uint64_t(2));
        coord = b.CreateInsertElement(coord, zero, uint64_t(3), "tex2da.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, v4f}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_sample_2Da", fnTy);
        return b.CreateCall(s, {texHandle, sampPtr, coord}, "tex2da.sample.rgba");
    }

    // Texture2DArray.fetch → __ockl_image_load_2Da: coord <4 x i32> {x, y, layer, 0}.
    llvm::Value* fetchTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* texHandle, llvm::Value* x,
                                     llvm::Value* y, llvm::Value* layer,
                                     llvm::Type* texelTy) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4i = llvm::FixedVectorType::get(i32, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
        llvm::Value* coord = llvm::PoisonValue::get(v4i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1));
        coord = b.CreateInsertElement(coord, layer, uint64_t(2));
        coord = b.CreateInsertElement(coord, zero, uint64_t(3), "tex2da.fetch.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, v4i}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_load_2Da", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, coord}, "tex2da.fetch.rgba");
        if (texelTy && texelTy->isIntegerTy())
            return b.CreateBitCast(rgba, v4i, "tex2da.fetch.i32");
        return rgba;
    }

    // TextureCube.sample — EMULATED: HIP cannot make a cubemap array here, so the cube is a
    // 6-layer array and the major-axis face projection below, branchless and in the CPU
    // oracle's comparison order, picks the layer. Face order +X,-X,+Y,-Y,+Z,-Z.
    llvm::Value* sampleTextureCube(llvm::IRBuilderBase& b, llvm::Module& m,
                                   llvm::Value* texHandle,
                                   llvm::Value* /*samplerHandle*/, llvm::Value* x,
                                   llvm::Value* y, llvm::Value* z) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto cI = [&](int v) { return llvm::ConstantInt::get(i32, v); };
        auto cF = [&](double v) { return llvm::ConstantFP::get(f32, v); };

        llvm::Value* ax = b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x, nullptr, "ax");
        llvm::Value* ay = b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, y, nullptr, "ay");
        llvm::Value* az = b.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, z, nullptr, "az");
        // xMajor = ax>=ay && ax>=az ; yMajor = !xMajor && ay>=ax && ay>=az ; else zMajor.
        llvm::Value* xMajor = b.CreateAnd(b.CreateFCmpOGE(ax, ay), b.CreateFCmpOGE(ax, az), "xMajor");
        llvm::Value* yMajor = b.CreateAnd(b.CreateNot(xMajor),
                                 b.CreateAnd(b.CreateFCmpOGE(ay, ax), b.CreateFCmpOGE(ay, az)), "yMajor");
        llvm::Value* xPos = b.CreateFCmpOGE(x, cF(0.0));
        llvm::Value* yPos = b.CreateFCmpOGE(y, cF(0.0));
        llvm::Value* zPos = b.CreateFCmpOGE(z, cF(0.0));
        llvm::Value* negX = b.CreateFNeg(x), *negY = b.CreateFNeg(y), *negZ = b.CreateFNeg(z);
        // face: X→{+0,-1} Y→{+2,-3} Z→{+4,-5}
        llvm::Value* faceX = b.CreateSelect(xPos, cI(0), cI(1));
        llvm::Value* faceY = b.CreateSelect(yPos, cI(2), cI(3));
        llvm::Value* faceZ = b.CreateSelect(zPos, cI(4), cI(5));
        llvm::Value* face = b.CreateSelect(xMajor, faceX,
                              b.CreateSelect(yMajor, faceY, faceZ), "cube.face");
        llvm::Value* ma = b.CreateSelect(xMajor, ax, b.CreateSelect(yMajor, ay, az));
        ma = b.CreateSelect(b.CreateFCmpOEQ(ma, cF(0.0)), cF(1.0), ma, "cube.ma");
        // sc: +X:-z -X:z  Y:x  +Z:x -Z:-x   tc: X:-y  +Y:z -Y:-z  Z:-y
        llvm::Value* scX = b.CreateSelect(xPos, negZ, z);
        llvm::Value* scZ = b.CreateSelect(zPos, x, negX);
        llvm::Value* sc = b.CreateSelect(xMajor, scX, b.CreateSelect(yMajor, x, scZ));
        llvm::Value* tcY = b.CreateSelect(yPos, z, negZ);
        llvm::Value* tc = b.CreateSelect(xMajor, negY, b.CreateSelect(yMajor, tcY, negY));
        llvm::Value* u = b.CreateFMul(cF(0.5), b.CreateFAdd(b.CreateFDiv(sc, ma), cF(1.0)), "cube.u");
        llvm::Value* v = b.CreateFMul(cF(0.5), b.CreateFAdd(b.CreateFDiv(tc, ma), cF(1.0)), "cube.v");

        llvm::Value* sampPtr = b.CreateConstGEP1_32(i8, texHandle, 48, "tex.samp.obj");
        llvm::Value* faceF = b.CreateSIToFP(face, f32, "cube.facef");
        llvm::Value* coord = llvm::PoisonValue::get(v4f);
        coord = b.CreateInsertElement(coord, u, uint64_t(0));
        coord = b.CreateInsertElement(coord, v, uint64_t(1));
        coord = b.CreateInsertElement(coord, faceF, uint64_t(2));
        coord = b.CreateInsertElement(coord, cF(0.0), uint64_t(3), "texcube.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, v4f}, false);
        llvm::FunctionCallee s = m.getOrInsertFunction("__ockl_image_sample_2Da", fnTy);
        return b.CreateCall(s, {texHandle, sampPtr, coord}, "texcube.sample.rgba");
    }

    // --- Image2D storage images (the writable twin of Texture2D) --------------

    // img.store → __ockl_image_store_2D. A storage image is a surface object (image SRD
    // only), so imgHandle is the sole operand; the texel is R32f with the value in lane 0.
    void storeImage(llvm::IRBuilderBase& b, llvm::Module& m,
                    llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
                    llvm::Value* value) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "img.coord");
        llvm::Value* texel = llvm::ConstantAggregateZero::get(v4f);
        texel = b.CreateInsertElement(texel, value, uint64_t(0), "img.texel");
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                             {p4, v2i, v4f}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_store_2D", fnTy);
        b.CreateCall(s, {imgHandle, coord, texel});
    }

    // img.load → __ockl_image_load_2D, the read twin: the R32f texel is component 0.
    llvm::Value* loadImage(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* imgHandle, llvm::Value* x,
                           llvm::Value* y) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* p4 = llvm::PointerType::get(ctx, 4);
        auto* v2i = llvm::FixedVectorType::get(i32, 2);
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* coord = llvm::PoisonValue::get(v2i);
        coord = b.CreateInsertElement(coord, x, uint64_t(0));
        coord = b.CreateInsertElement(coord, y, uint64_t(1), "img.coord");
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, v2i}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_load_2D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {imgHandle, coord}, "img.load.rgba");
        return b.CreateExtractElement(rgba, uint64_t(0), "img.load");
    }

    // (Shader clock uses the base default llvm.readcyclecounter; s_memtime dies on gfx11+.)

    // Transcendentals via ROCm's ocml (`__ocml_<fn>_f32`/`_f64`): AMDGPU mis-lowers
    // llvm.sin and friends (no range reduction). rsqrt is a native amdgcn intrinsic.
    llvm::Value* transcendental(llvm::IRBuilderBase& b, llvm::Module& m,
                                const std::string& name,
                                llvm::ArrayRef<llvm::Value*> args) override {
        llvm::Type* ft = args[0]->getType();
        // ocml is scalar-only — vectorized math scalarizes per lane.
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

    // Wave ops. Wavefront size is target-dependent (32 or 64; 32 for compute here);
    // readlane is shuffle-by-index and ballot's wave-width mask widens to i64.
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
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_wave_reduce_add, {i32});
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0)}, "wavered");
    }
    // amdgcn.wave.reduce.{umax,umin,and,or,xor} over i32 — unsigned min/max for the
    // uint32 surface, strategy operand 0 for the default lowering, as in the f32 twin.
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
        llvm::Intrinsic::ID id;
        switch (op) {
            case WaveReduceOp::Max: id = llvm::Intrinsic::amdgcn_wave_reduce_umax; break;
            case WaveReduceOp::Min: id = llvm::Intrinsic::amdgcn_wave_reduce_umin; break;
            case WaveReduceOp::And: id = llvm::Intrinsic::amdgcn_wave_reduce_and; break;
            case WaveReduceOp::Or:  id = llvm::Intrinsic::amdgcn_wave_reduce_or; break;
            case WaveReduceOp::Xor: id = llvm::Intrinsic::amdgcn_wave_reduce_xor; break;
        }
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id, {i32});
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0)}, "wavered");
    }
    llvm::Value* waveReduceF32(llvm::IRBuilderBase& b, llvm::Module& m,
                               WaveReduceFOp op, llvm::Value* value) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Intrinsic::ID id = op == WaveReduceFOp::Sum
            ? llvm::Intrinsic::amdgcn_wave_reduce_fadd
            : llvm::Intrinsic::amdgcn_wave_reduce_fmax;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id, {f32});
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0)},
                            "wavered.f");
    }
    // The canonical lane-id idiom: mbcnt hi(~0, lo(~0, 0)) counts set exec bits below
    // this lane, giving its index in the wavefront — wave32 and wave64 alike.
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
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
    // readlane (the uniform waveShuffle) cannot take a per-lane source, so use
    // ds_bpermute, the divergent intra-wave gather: lane at byte address src*4.
    llvm::Value* waveShuffleDivergent(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* value,
                                      llvm::Value* srcLane) override {
        llvm::Value* byteAddr = b.CreateShl(srcLane, 2, "wave.gather.byte");
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_ds_bpermute);
        return b.CreateCall(f, {byteAddr, value}, "wave.gather");
    }
    // Rotate's source, (laneId + delta) mod width, is per-lane DIVERGENT, so it takes
    // ds_bpermute rather than the base default's wave-uniform readlane.
    llvm::Value* waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* value, llvm::Value* delta) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* lane = waveLaneId(b, m);
        llvm::Value* width = waveWidth(b, m);
        llvm::Value* src = b.CreateURem(
            b.CreateAdd(lane, delta), width, "wave.rotate.src");
        llvm::Value* byteAddr = b.CreateShl(src, 2, "wave.rotate.byte");
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_ds_bpermute);
        return b.CreateCall(f, {byteAddr, value}, "wave.rotate");
    }

    // ---- Cooperative matrix: gfx11 `v_wmma_*_16x16x16` (wave32) per-lane layout ----
    //   A  : lane L holds row (L & 15), <16 x elem> over k = 0..15, replicated across
    //        the two 16-lane halves of the wave.
    //   B  : lane L holds column (L & 15), <16 x elem> over k = 0..15.
    //   C/D: <8 x elem> per lane — column (L & 15), rows { 2e + (L >> 4) }.
    //   bf16 A/B: <16 x i16>.  int8: 4 K-values per i32, <4 x i32> with <8 x i32> acc.

    // Native only at 16x16x16: f16/bf16/int8 operands, f32 or i32 accumulator. The role
    // matters — an int32 *operand* has no WMMA while an int32 *accumulator* is native.
    ImplTier coopMatrixTier(llvm::Type* elem, uint32_t rows, uint32_t cols,
                            uint32_t use) override {
        if (rows == 16 && cols == 16) {
            if (use == 2) {
                if (elem->isFloatTy() || elem->isIntegerTy(32))
                    return ImplTier::Native;
            } else if (elem->isHalfTy() || elem->isBFloatTy() ||
                       elem->isIntegerTy(8)) {
                return ImplTier::Native;
            }
        }
        return ImplTier::Portable;
    }

    // RDNA3 WMMA exists only in the wave32 encoding, so pin the function's wave size.
    void prepareNativeCoopMatrix(llvm::Function* fn) override {
        fn->addFnAttr("target-features", "+wavefrontsize32");
    }

    // The per-lane fragment type for each tile role (see the CM7 layout above).
    llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem,
                               uint32_t /*rows*/, uint32_t /*cols*/,
                               uint32_t use) override {
        llvm::LLVMContext& ctx = m.getContext();
        if (use == 2) {
            llvm::Type* ae = elem->isIntegerTy()
                ? (llvm::Type*) llvm::Type::getInt32Ty(ctx)
                : (llvm::Type*) llvm::Type::getFloatTy(ctx);
            return llvm::FixedVectorType::get(ae, 8);
        }
        if (elem->isIntegerTy(8))
            return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(ctx), 4);
        llvm::Type* fe = elem->isBFloatTy()
            ? (llvm::Type*) llvm::Type::getInt16Ty(ctx) : elem;
        return llvm::FixedVectorType::get(fe, 16);
    }

    // Assemble this lane's fragment from memory: the int8 A/B path packs 4 consecutive
    // K-values per i32, little-endian, and every other role loads element by element.
    llvm::Value* coopMatrixLoad(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* ptr, llvm::Value* layout,
                                llvm::Value* stride, llvm::Type* matrixType,
                                uint32_t /*rows*/, uint32_t /*cols*/,
                                uint32_t use, uint32_t swz = 0,
                                LdsBlockPad blk = {}) override {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(matrixType);
        llvm::Type* fe = vecTy->getElementType();
        unsigned n = vecTy->getNumElements();
        llvm::Value* frag = llvm::UndefValue::get(vecTy);
        if (use != 2 && fe->isIntegerTy(32)) {
            llvm::LLVMContext& ctx = m.getContext();
            llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
            // FAST PATH: when this lane's 16 fragment bytes are provably CONSECUTIVE, load
            // them as ONE <4 x i32> — little-endian gives the byte loop's exact packing.
            {
                auto* cl = llvm::dyn_cast<llvm::ConstantInt>(layout);
                auto* cs = llvm::dyn_cast<llvm::ConstantInt>(stride);
                // Only the LAYOUT need be constant — the stride merely positions each
                // lane's base. A provably 4-aligned stride upgrades the alignment hint.
                bool laneRun = cl && n == 4 && swz == 0 &&
                    !blk.period &&
                    ((use == 0 && cl->getZExtValue() == 0) ||
                     (use == 1 && cl->getZExtValue() == 1));
                if (laneRun) {
                    llvm::Type* i32t = llvm::Type::getInt32Ty(ctx);
                    llvm::Value* lane16 = b.CreateAnd(
                        waveLaneId(b, m),
                        llvm::ConstantInt::get(i32t, 15));
                    llvm::Value* off =
                        b.CreateMul(lane16, stride, "cm.ld16.off");
                    llvm::Value* p =
                        b.CreateGEP(i8, ptr, off, "cm.ld16.ptr");
                    llvm::LoadInst* wide =
                        b.CreateLoad(vecTy, p, "cm.ld16");
                    wide->setAlignment(llvm::Align(
                        cs && (cs->getZExtValue() % 4) == 0 ? 4 : 1));
                    return wide;
                }
            }
            for (unsigned w = 0; w < n; ++w) {
                llvm::Value* word = llvm::ConstantInt::get(fe, 0);
                for (unsigned s = 0; s < 4; ++s) {
                    auto rc = fragCoord(b, m, use, 4 * w + s, layout, stride, swz, blk);
                    llvm::Value* p = b.CreateGEP(i8, ptr, rc, "cm.ld.ptr");
                    llvm::Value* byte = b.CreateLoad(i8, p, "cm.ld");
                    llvm::Value* bits = b.CreateShl(
                        b.CreateZExt(byte, fe),
                        llvm::ConstantInt::get(fe, s * 8));
                    word = b.CreateOr(word, bits);
                }
                frag = b.CreateInsertElement(frag, word, w);
            }
            return frag;
        }
        for (unsigned e = 0; e < n; ++e) {
            auto rc = fragCoord(b, m, use, e, layout, stride, swz, blk);
            llvm::Value* p = b.CreateGEP(fe, ptr, rc, "cm.ld.ptr");
            frag = b.CreateInsertElement(frag, b.CreateLoad(fe, p, "cm.ld"), e);
        }
        return frag;
    }

    void coopMatrixStore(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
                         llvm::Value* matrixVal, llvm::Value* layout,
                         llvm::Value* stride, uint32_t /*rows*/, uint32_t /*cols*/,
                         uint32_t use, uint32_t swz = 0,
                         LdsBlockPad blk = {}) override {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(matrixVal->getType());
        llvm::Type* fe = vecTy->getElementType();
        unsigned n = vecTy->getNumElements();
        for (unsigned e = 0; e < n; ++e) {
            auto rc = fragCoord(b, m, use, e, layout, stride, swz, blk);
            llvm::Value* p = b.CreateGEP(fe, ptr, rc, "cm.st.ptr");
            b.CreateStore(b.CreateExtractElement(matrixVal, e), p);
        }
    }

    // Pick the WMMA intrinsic by A/B element type: <16 x half> f16, <16 x i16> bf16,
    // <4 x i32> packed int8 (iu8 — signed per signFlags bit 0/1, and never clamped).
    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* a, llvm::Value* bMat,
                                  llvm::Value* c, llvm::Type* /*matrixType*/,
                                  uint32_t signFlags) override {
        llvm::Type* ae =
            llvm::cast<llvm::FixedVectorType>(a->getType())->getElementType();
        if (ae->isIntegerTy(32)) {
            llvm::LLVMContext& ctx = m.getContext();
            llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::amdgcn_wmma_i32_16x16x16_iu8,
                {c->getType(), a->getType()});
            llvm::Value* aSg = llvm::ConstantInt::getBool(ctx, (signFlags & 1u) != 0);
            llvm::Value* bSg = llvm::ConstantInt::getBool(ctx, (signFlags & 2u) != 0);
            llvm::Value* fls = llvm::ConstantInt::getFalse(ctx);  // no clamp
            return b.CreateCall(f, {aSg, a, bSg, bMat, c, fls}, "wmma.iu8");
        }
        llvm::Intrinsic::ID id = ae->isHalfTy()
            ? llvm::Intrinsic::amdgcn_wmma_f32_16x16x16_f16
            : llvm::Intrinsic::amdgcn_wmma_f32_16x16x16_bf16;
        // Overloaded on (D/C type, A/B type), in first-appearance order.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, id, {c->getType(), a->getType()});
        return b.CreateCall(f, {a, bMat, c}, "wmma");
    }

    // The fused epilogue runs in the accumulator fragment's registers: element e is at row
    // 2e + (lane>>4), column lane & 15. The association (rowF*colF)*C is a cross-tier contract.
    bool coopMatrixEpilogueSupported() const override { return true; }
    // The iu8 fragment is <4 x i32> per lane, so fromWords is a plain vector build.
    bool coopMatrixFromWordsSupported() const override { return true; }

    llvm::Value* coopMatrixEpilogueAccum(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* accVal,
            llvm::Value* faccVal, llvm::Value* rowFPtr, llvm::Type* rowETy,
            llvm::Value* colFPtr, llvm::Type* colETy,
            llvm::Value* rowGPtr = nullptr,
            llvm::Value* colGPtr = nullptr,
            llvm::Value* colFScalar = nullptr,
            llvm::Value* colGScalar = nullptr) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Value* lane = waveLaneId(b, m);
        llvm::Value* lane16 =
            b.CreateAnd(lane, llvm::ConstantInt::get(i32, 15));
        llvm::Value* half =
            b.CreateLShr(lane, llvm::ConstantInt::get(i32, 4));
        // colF[c] is a per-lane constant either way; the S variants pass it in a register.
        llvm::Value* cv = colFScalar
            ? colFScalar
            : b.CreateLoad(
                  colETy, b.CreateGEP(colETy, colFPtr, lane16,
                                      "epi.cf.ptr"),
                  "epi.cf");
        llvm::Value* cgv = colGScalar;
        if (!cgv && colGPtr)
            cgv = b.CreateLoad(
                colETy, b.CreateGEP(colETy, colGPtr, lane16, "epi.cg.ptr"),
                "epi.cg");
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(faccVal->getType());
        unsigned n = vecTy->getNumElements();
        llvm::Value* out = faccVal;
        for (unsigned e = 0; e < n; ++e) {
            llvm::Value* row = b.CreateAdd(
                llvm::ConstantInt::get(i32, 2 * e), half, "epi.row");
            llvm::Value* rv = b.CreateLoad(
                rowETy, b.CreateGEP(rowETy, rowFPtr, row, "epi.rf.ptr"),
                "epi.rf");
            llvm::Value* term = b.CreateFMul(rv, cv);
            if (accVal) {
                llvm::Value* av = b.CreateExtractElement(accVal, e);
                if (av->getType()->isIntegerTy())
                    av = b.CreateSIToFP(av, f32);
                term = b.CreateFMul(term, av);
            }
            if (rowGPtr) {
                llvm::Value* rg = b.CreateLoad(
                    rowETy,
                    b.CreateGEP(rowETy, rowGPtr, row, "epi.rg.ptr"),
                    "epi.rg");
                term = b.CreateFAdd(term, b.CreateFMul(rg, cgv));
            }
            llvm::Value* cur = b.CreateExtractElement(out, e);
            out = b.CreateInsertElement(out, b.CreateFAdd(cur, term), e,
                                        "epi.facc");
        }
        return out;
    }

    llvm::Value* coopMatrixSplat(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* value,
                                 llvm::Type* matrixType) override {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(matrixType);
        llvm::Type* fe = vecTy->getElementType();
        if (value->getType() != fe) {
            if (value->getType()->isFloatingPointTy() && fe->isFloatingPointTy())
                value = b.CreateFPCast(value, fe);
            else if (value->getType()->isIntegerTy() && fe->isIntegerTy())
                value = b.CreateIntCast(value, fe, /*isSigned=*/true);
        }
        (void) m;
        return b.CreateVectorSplat(vecTy->getNumElements(), value, "cm.splat");
    }

private:
    // The element offset into the tile base for fragment element `e` of a tile with role
    // `use`, on the current lane: row-major `row*stride+col`, column-major `col*stride+row`.
    llvm::Value* fragCoord(llvm::IRBuilderBase& b, llvm::Module& m, uint32_t use,
                           unsigned e, llvm::Value* layout, llvm::Value* stride,
                           uint32_t swz = 0, LdsBlockPad blk = {}) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* lane = waveLaneId(b, m);
        llvm::Value* lane16 = b.CreateAnd(lane, llvm::ConstantInt::get(i32, 15));
        llvm::Value* half = b.CreateLShr(lane, llvm::ConstantInt::get(i32, 4));
        llvm::Value* row;
        llvm::Value* col;
        if (use == 0) {            // A: lane = row, e = K index
            row = lane16;
            col = llvm::ConstantInt::get(i32, e);
        } else if (use == 1) {     // B: lane = col, e = K index
            row = llvm::ConstantInt::get(i32, e);
            col = lane16;
        } else {                   // C/D accumulator: col = lane16, row = 2e+half
            row = b.CreateAdd(llvm::ConstantInt::get(i32, 2 * e), half);
            col = lane16;
        }
        llvm::Value* rowMajor = b.CreateAdd(b.CreateMul(row, stride), col);
        llvm::Value* colMajor = b.CreateAdd(b.CreateMul(col, stride), row);
        llvm::Value* idx = b.CreateSelect(
            b.CreateICmpEQ(layout, llvm::ConstantInt::get(i32, 0)),
            rowMajor, colMajor, "cm.idx");
        // WMMA sub-tile offsets are S²-aligned, so the fragment-local swizzle equals
        // the absolute one the staging applied.
        if (swz) { idx = swizzleAddr(b, idx, swz); return idx; }
        // BlockPadded: when the fragment fits one pad block and the panel base is block-
        // aligned, pad that base ALONE and add per-lane + e unpadded — it folds to a constant.
        if (blk.period) {
            llvm::Value* eC = llvm::ConstantInt::get(i32, e);
            bool canFold = false;
            if ((use == 0 || use == 1) && llvm::isa<llvm::ConstantInt>(stride)) {
                uint64_t S = llvm::cast<llvm::ConstantInt>(stride)->getZExtValue();
                bool baseAligned = !blk.baseOffset;
                if (auto* bo = llvm::dyn_cast_or_null<llvm::ConstantInt>(blk.baseOffset))
                    baseAligned = (bo->getZExtValue() % blk.period) == 0;
                canFold = baseAligned && (15 * S + 15 < blk.period);
            }
            if (canFold) {
                llvm::Value* padBase = blk.baseOffset
                    ? blockPadAddr(b, blk.baseOffset, blk.period, blk.pad)
                    : llvm::ConstantInt::get(i32, 0);
                idx = b.CreateAdd(padBase, idx, "cm.foldidx");
            } else {
                llvm::Value* base = b.CreateSub(idx, eC, "cm.frag0");
                if (blk.baseOffset) base = b.CreateAdd(base, blk.baseOffset, "cm.abs");
                base = blockPadAddr(b, base, blk.period, blk.pad);
                idx = b.CreateAdd(base, eC, "cm.padidx");
            }
        }
        return idx;
    }

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
