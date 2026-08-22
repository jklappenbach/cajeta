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

    // No native inline ray query (cajeta has no AMDGPU RT seam — that path is
    // Vulkan/SPIR-V's OpRayQuery, or a vendor RT extension), so the Acceleration-
    // Structure noun is built as the portable software BVH and the RayQuery verb
    // follows to the cajeta.xpu.SoftwareRayQuery walk — the same Portable tier
    // the CPU and NVPTX backends use. Without this the base default (VulkanNative)
    // routes RayQuery to rayQueryType(), which throws on AMDGPU. softwareRayQuery()
    // derives from this in the base; the HIP noun provider uploads the BVH to a
    // device buffer the kernel reads as bvh[i]. (coopMatrixTier stays the AMD
    // override below — AMD has native WMMA.)
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

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

    void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m, FenceScope scope,
                     MemoryOrder order = MemoryOrder::Default) override {
        // A scoped fence at `order` — no s_barrier, so no thread rendezvous. The
        // AMDGPU sync-scope name selects the reach: "workgroup" (LDS+global
        // within the block) vs "agent" (the whole device). Default/Relaxed →
        // AcqRel (a relaxed fence is a no-op). The backend lowers this to the
        // right s_waitcnt / cache-flush sequence.
        llvm::SyncScope::ID sc = m.getContext().getOrInsertSyncScopeID(
            scope == FenceScope::Workgroup ? "workgroup" : "agent");
        llvm::AtomicOrdering ord =
            toAtomicOrdering(order, llvm::AtomicOrdering::AcquireRelease);
        if (ord == llvm::AtomicOrdering::Monotonic)
            ord = llvm::AtomicOrdering::AcquireRelease;
        b.CreateFence(ord, sc);
    }

    // Async global->LDS copy (xpu-pipelined-gemm-primitives U2). The LDS-direct
    // vmem load `global_load_lds_{ubyte,ushort,dword}` exists on GFX9/CDNA and
    // gfx1250+, but NOT on RDNA1-3.5 (gfx10xx/gfx11xx incl. gfx1151), which fall
    // back to the synchronous staged copy (see archHasVmemToLds / bundleHasVmemToLds).
    // Where it IS available, `copy` issues `global_load_lds` per element — the data
    // goes global->LDS with NO VGPR staging buffer (frees the registers the WMMA
    // accumulator path is starved for), the key ISA win over the staged
    // global->reg->LDS path. The workgroup stripes it (e = tid, tid+nthr, ...), so
    // each wave's lanes coalesce. Element size must be 1/2/4 bytes; wider (e.g.
    // fp64) falls back to the synchronous strided seam (the fp64 path uses CoopStage,
    // not AsyncCopy). `commit` is a no-op (no async-mark); `wait` drains outstanding
    // vmem so the landed LDS is safe to publish through the caller's Barrier — a full
    // drain, since deep N-stage overlap needs the gfx1250 async-mark path.
    // True iff `arch` (gfxNNN) has the direct global->LDS load (VMemToLDSLoad in
    // LLVM): the GFX9/CDNA family and gfx1250+. RDNA1-3.5 (gfx10xx/gfx11xx) and
    // gfx1200/1201 lack it — emitting global_load_lds there Cannot-selects, so we
    // fall back to the synchronous staged copy.
    static bool archHasVmemToLds(llvm::StringRef arch) {
        if (arch.starts_with("gfx9")) return true;
        unsigned num = 0;
        if (arch.starts_with("gfx") && !arch.drop_front(3).getAsInteger(10, num))
            return num >= 1250;
        return false;
    }

    // A kernel is lowered ONCE but codegen'd for every arch in a multi-arch
    // bundle, so emitting global_load_lds is safe only when EVERY target arch
    // supports it — otherwise an unsupporting arch Cannot-selects and the kernel
    // is silently dropped from the bundle. `cajeta.amdgpu.archlist` carries the
    // full bundle; fall back to the single-arch flag when it is absent.
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

    // simd-fused-integer-madd 2.2.3 — the native int8 dot unit
    // (`v_dot4_i32_iu8` / `v_dot4_u32_u8`). Only SPIR-V overrode this seam
    // before, so every AMD kernel took the base portable widen and left the
    // instruction unused on hardware that has it.
    //
    // MEASURED against llc across the arch list, because the failure mode is a
    // hard ISel error rather than a slow path — an unsupporting arch in a
    // multi-arch bundle takes the kernel down with it. llvm.amdgcn.sdot4
    // selects on gfx906/908/90a/942/950, gfx1011/1012, gfx103x, gfx11xx and
    // gfx12xx, and FAILS on gfx900, gfx902, gfx940 and gfx1010. Note gfx940
    // fails while gfx942 works and gfx1010 fails while gfx1011 works, so this
    // is not a clean numeric threshold — hence the explicit exclusions.
    //
    // Unknown arches answer NO. A wrong no costs speed; a wrong yes fails the
    // build.
    static bool archHasDot4(llvm::StringRef arch) {
        unsigned num = 0;
        if (!arch.starts_with("gfx") || arch.drop_front(3).getAsInteger(10, num))
            return false;
        if (num == 940) return false;            // early MI300; gfx942 has it
        if (num >= 900 && num < 1000) return num >= 906;
        if (num >= 1000 && num < 1100) return num >= 1011;   // gfx1010 lacks it
        return num >= 1100 && num < 1300;
    }

    // Same bundle rule as bundleHasVmemToLds: lowered once, codegen'd for every
    // arch in the bundle, so the unit is usable only when EVERY arch has it.
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

    llvm::Value* integerDot4x8(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* a, llvm::Value* c, llvm::Value* acc,
                               bool isSigned) override {
        if (!bundleHasDot4(m))
            return LoweringTarget::integerDot4x8(b, m, a, c, acc, isSigned);
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* x = b.CreateBitCast(a, i32, "dp4a.x");
        llvm::Value* y = b.CreateBitCast(c, i32, "dp4a.y");
        llvm::Intrinsic::ID id = isSigned ? llvm::Intrinsic::amdgcn_sdot4
                                          : llvm::Intrinsic::amdgcn_udot4;
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        // clamp=false: the non-saturating encoding, so this tier stays
        // bit-identical with the portable widen it replaces (spec §4.8/§4.9).
        return b.CreateCall(f, {x, y, acc, b.getFalse()}, "dp4a");
    }

    void asyncCopy(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* dstBase,
                   llvm::Type* dstElem, llvm::Value* dstOffset, llvm::Value* srcBase,
                   llvm::Type* srcElem, llvm::Value* srcOffset,
                   llvm::Value* count) override {
        llvm::LLVMContext& ctx = m.getContext();
        uint64_t elemBytes = m.getDataLayout().getTypeStoreSize(srcElem);
        // No native direct global->LDS on some target arch in the bundle, or a
        // width the LDS-direct load can't carry (1/2/4 bytes only) — use the
        // synchronous staged copy (correct on every arch).
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

    // Drain the outstanding global_load_lds writes (a workgroup AcqRel fence the
    // backend lowers to the right s_waitcnt) so the caller's Barrier publishes
    // landed data. Full drain — gfx1151 has no async-mark partial wait.
    void asyncWait(llvm::IRBuilderBase& b, llvm::Module& m,
                   llvm::Value* /*groupsInFlight*/) override {
        llvm::SyncScope::ID wg =
            m.getContext().getOrInsertSyncScopeID("workgroup");
        b.CreateFence(llvm::AtomicOrdering::AcquireRelease, wg);
    }

    // Instruction-scheduling hints (xpu-kernel-scheduling-hints §3) → the native
    // amdgcn intrinsics. sched_barrier / sched_group_barrier / iglp_opt are
    // SCHEDULER directives consumed by the MachineScheduler (no ISA instruction
    // survives to the output); s_setprio is a real SOPP instruction. All operands
    // are ImmArg — passed as ConstantInt, already validated/range-checked by the
    // call-site dispatch.
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

    // Conflict-free LDS swizzle for a Swizzled<T,S> tile: permute the flat element
    // index so consecutive rows of the tile land in different LDS banks. With
    // row = idx >> log2S and col = idx & (S-1), the physical slot is
    // row*S + (col ^ (row & (S-1))). The XOR touches only col's bits [0,log2S),
    // disjoint from the row bits it reads, so it is an involution — applied
    // identically on every access, the staged data reads back unchanged while the
    // WMMA fragment loads stop colliding on banks. (S is a power of two; the
    // frontend rejects anything else.)
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

    // Block-padded LDS tile (BlockPadded<T,Block,Pad> / Tensile LdsBlockSizePerPad):
    // physical = idx + (idx / period) * pad. The byte-period boundary is independent
    // of the row/col stride, so the one layout de-conflicts both the wide staging
    // store and the transposed WMMA read; applied identically on every access, the
    // staged data reads back unchanged.
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

    // AMDGPU marks kernels purely by calling convention — no metadata
    // analogue to nvvm.annotations.
    void decorateKernel(llvm::Function* fn, llvm::Module& /*m*/) override {
        fn->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);
    }

    // @Occupancy override (kernel-occupancy-autotune §3). maxThreads pins the
    // launch bound (flat-work-group-size — the lever that controls the VGPR
    // budget on RDNA); minResident pins the occupancy floor (waves-per-eu). On
    // gfx1151 waves-per-eu is inert, but it is the honest AMD mapping and matters
    // on other gfx. maxRegisters has no stable per-function AMDGPU attribute, so
    // it is folded into the occupancy intent rather than set directly.
    void applyOccupancy(llvm::Function* fn, const XpuKernelAttr& attr) override {
        if (auto mt = attr.maxThreads()) {
            std::string range = "1," + std::to_string(*mt);
            fn->addFnAttr("amdgpu-flat-work-group-size", range);
        }
        if (auto mr = attr.minResident()) {
            fn->addFnAttr("amdgpu-waves-per-eu", std::to_string(*mr));
        }
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
                               llvm::Value* v, llvm::Value* lod) override {
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
        // Explicit-LOD variant (lod is the float mip level; 0.0 for plain sample
        // → level 0, exactly as on a single-level image). sampleLod threads the
        // user LOD here; the mipmapped texobj's maxMipmapLevelClamp admits it.
        auto* fnTy = llvm::FunctionType::get(v4f, {p4, p4, v2f, f32}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_sample_lod_2D", fnTy);
        llvm::Value* rgba = b.CreateCall(s, {texHandle, sampPtr, coord, lod},
                                         "tex.sample.rgba");
        // Return the full <4 x float> RGBA — Texture2D.sample is typed
        // Vector<float32,4>; the caller picks a channel with .r/.x.
        return rgba;
    }

    // tex.fetch(x, y) → __ockl_image_load_2D (ROCm device library, linked by
    // AmdgpuBackend when an __ockl_image_* symbol is referenced). The unfiltered
    // twin of sampleTexture: it takes the image object ptr (= texHandle,
    // addrspace 4) and the integer <x, y> coord — NO sampler (the sampler SRD is
    // unused for a plain image load), no normalization. The image's format
    // descriptor still decodes the stored encoding (UNORM byte → [0, 1], half →
    // float). Returns the <4 x float> texel (Texture2D.fetch is Vector<float32,4>).
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
        // ockl exposes only a v4f32-returning 2-D image load. For an integer-format
        // image the HW image_load is RAW (no normalization / convert on a SINT/UINT
        // SRD), so the v4f32 result holds the verbatim 32-bit integer bits — bitcast
        // it to <4 x i32> to recover the integers. There is no int-returning ockl
        // image-load symbol, and this matches the SPIR-V/CPU paths bit-for-bit.
        // Explicit-LOD variant (lod is the i32 mip level; 0 for plain fetch →
        // level 0). fetchLod threads the user LOD here.
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

    // Texture3D.sample(sampler, u, v, w) → __ockl_image_sample_3D (the 3-D twin of
    // __ockl_image_sample_2D). The 3-D ockl coord is a <4 x float> (u, v, w, 0 —
    // the 4th lane unused); the sampler object rides the texture object at +48, as
    // in 2-D. Returns the <4 x float> trilinear gather.
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

    // Texture3D.fetch(x, y, z) → __ockl_image_load_3D (unfiltered 3-D twin). The
    // 3-D ockl load coord is a <4 x i32> (x, y, z, 0). Float result; bitcast to
    // <4 x i32> for an integer volume (raw image_load, as in 2-D).
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

    // Texture1D.sample(sampler, u) → __ockl_image_sample_1D (the 1-D twin of
    // __ockl_image_sample_2D). Unlike 2-D/3-D, the 1-D ockl coord is a SCALAR
    // float (not a vector); the sampler object rides the texture object at +48,
    // as in 2-D. Returns the <4 x float> linear gather. No lod variant — mipmaps
    // are 2-D only.
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

    // Texture1D.fetch(x) → __ockl_image_load_1D (unfiltered 1-D twin). The 1-D
    // ockl load coord is a SCALAR i32. Float result; bitcast to <4 x i32> for an
    // integer row (raw image_load, as in 2-D).
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

    // Texture2DArray.sample(sampler, u, v, layer) → __ockl_image_sample_2Da (the
    // layered twin of __ockl_image_sample_2D). The 2-D-array ockl coord is a
    // <4 x float> {u, v, layer, 0} — the 3rd lane is the (un-normalized) array
    // layer; `layer` arrives as i32 and is converted to float. The sampler object
    // rides the texture object at +48, as in 2-D. Returns the <4 x float> gather.
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

    // Texture2DArray.fetch(x, y, layer) → __ockl_image_load_2Da (unfiltered layered
    // twin). The 2-D-array ockl load coord is a <4 x i32> {x, y, layer, 0}. Float
    // result; bitcast to <4 x i32> for an integer array (raw image_load, as in 2-D).
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

    // TextureCube.sample(sampler, x, y, z) — EMULATED. The HIP runtime can't make a
    // cubemap array on gfx1151 (see runtime cajeta_xpu_hip_texcube_alloc), so the
    // cube is stored as a 6-LAYER layered array and we do the major-axis face
    // projection HERE (branchless port of __cajeta_xpu_cpu_texcube_sample_rgba —
    // same comparisons/order, so AMD bit-matches the CPU oracle), then sample the
    // chosen face via __ockl_image_sample_2Da (layer = face). Face order
    // +X,-X,+Y,-Y,+Z,-Z. Limitation: no seamless cross-face filtering (per-face clamp).
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
        // Major-axis selection, matching the CPU if/elseif/else exactly:
        //   xMajor = ax>=ay && ax>=az ; yMajor = !xMajor && ay>=ax && ay>=az ; else zMajor.
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
        // ma = dominant |axis| (guard 0 → 1)
        llvm::Value* ma = b.CreateSelect(xMajor, ax, b.CreateSelect(yMajor, ay, az));
        ma = b.CreateSelect(b.CreateFCmpOEQ(ma, cF(0.0)), cF(1.0), ma, "cube.ma");
        // sc: +X:-z -X:z  Y:x  +Z:x -Z:-x   tc: X:-y  +Y:z -Y:-z  Z:-y
        llvm::Value* scX = b.CreateSelect(xPos, negZ, z);
        llvm::Value* scZ = b.CreateSelect(zPos, x, negX);
        llvm::Value* sc = b.CreateSelect(xMajor, scX, b.CreateSelect(yMajor, x, scZ));
        llvm::Value* tcY = b.CreateSelect(yPos, z, negZ);
        llvm::Value* tc = b.CreateSelect(xMajor, negY, b.CreateSelect(yMajor, tcY, negY));
        // u = 0.5*(sc/ma + 1) ; v = 0.5*(tc/ma + 1)
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
    //
    // img.store(x, y, v) / img.load(x, y) → __ockl_image_store_2D /
    // __ockl_image_load_2D (ROCm device library, linked by AmdgpuBackend when an
    // __ockl_image_* symbol is referenced). Unlike the sampled texture path these
    // take NO sampler — a storage image is bound as a surface object (the image
    // SRD only), so the handle (= imgHandle, a ptr addrspace(4) kernarg, the same
    // arg model as a texture via textureParamType) is the sole resource operand.
    // The runtime binds it via hipCreateSurfaceObject with hipArraySurfaceLoadStore.
    // Coords are the integer <x, y> (NOT normalized); the texel is the R32f <4 x
    // float> with the scalar value in lane 0 (the R32 image keeps lane 0) — mirror
    // of the Vulkan OpImageWrite/OpImageRead path so the two agree.

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
        // R32f texel: value in lane 0, zero elsewhere (the image's R32 format keeps
        // only lane 0 — same packing the Vulkan storeImage uses).
        llvm::Value* texel = llvm::ConstantAggregateZero::get(v4f);
        texel = b.CreateInsertElement(texel, value, uint64_t(0), "img.texel");
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                                             {p4, v2i, v4f}, false);
        llvm::FunctionCallee s =
            m.getOrInsertFunction("__ockl_image_store_2D", fnTy);
        b.CreateCall(s, {imgHandle, coord, texel});
    }

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
        // R32f → the scalar texel is component 0 (same as the Vulkan loadImage).
        return b.CreateExtractElement(rgba, uint64_t(0), "img.load");
    }

    // (Shader clock uses the base default: llvm.readcyclecounter, which the
    // AMDGPU backend lowers to s_getreg HW_REG_SHADER_CYCLES on RDNA — the GCN/
    // CDNA s_memrealtime/s_memtime intrinsics are not selectable on gfx11+.)

    // Transcendentals via the ROCm OpenCL math library (ocml): `__ocml_<fn>_f32`
    // (or `_f64`). AMDGPU mis-lowers `llvm.sin`/etc. (no range reduction), so we
    // emit the ocml call directly; AmdgpuBackend links ocml.bc when these
    // `__ocml_` declarations are present. rsqrt is a native amdgcn intrinsic.
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
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
        // amdgcn.wave.reduce.{umax,umin,and,or,xor} over i32 (unsigned min/max
        // for the uint32 surface); strategy operand 0 = default lowering.
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
    llvm::Value* waveShuffleDivergent(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* value,
                                      llvm::Value* srcLane) override {
        // readlane (the uniform waveShuffle) can't take a per-lane source; use
        // ds_bpermute, the divergent intra-wave gather (reads from lane at byte
        // address src*4). The portable rotate/scan defaults route through here.
        llvm::Value* byteAddr = b.CreateShl(srcLane, 2, "wave.gather.byte");
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::amdgcn_ds_bpermute);
        return b.CreateCall(f, {byteAddr, value}, "wave.gather");
    }
    llvm::Value* waveRotate(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* value, llvm::Value* delta) override {
        // The base default routes through waveShuffle = amdgcn.readlane, which
        // requires a wave-UNIFORM source lane — but rotate's source
        // (laneId + delta) mod width is per-lane DIVERGENT. Use ds_bpermute, the
        // AMDGPU divergent-index intra-wave gather: each lane reads `value` from
        // the lane at byte address src*4. (gfx11 wave32; covers the 32-lane
        // window the rotate test verifies.)
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

    // ---- Cooperative matrix: RDNA3 WMMA (matrix cores), CM7 ------------------
    // gfx11/RDNA3.5 `v_wmma_*_16x16x16` (wave32): D[16x16] = A·B + C, with the
    // tile held DISTRIBUTED across the 32 lanes of the wave. Unlike Vulkan (where
    // the SPIR-V cooperative-matrix ops distribute implicitly), we marshal each
    // fragment by hand from/to global memory in load/store, per the hardware
    // layout:
    //   A  : lane L holds row (L & 15) of A as <16 x elem>, k = 0..15 (the K
    //        axis); replicated across the two 16-lane halves of the wave.
    //   B  : lane L holds column (L & 15) of B as <16 x elem>, k = 0..15.
    //   C/D: <8 x float> per lane — lane L holds column (L & 15), rows
    //        { 2*e + (L >> 4) : e = 0..7 } (the wave32 interleaved-row layout).
    // bf16 A/B are carried as <16 x i16> (the same 16 bits) for the intrinsic.
    //
    // int8 (CM8) uses the iu8 WMMA — `v_wmma_i32_16x16x16_iu8`: the 16 K-values
    // of each A row / B column are packed 4-per-i32 into a <4 x i32> fragment,
    // and the accumulator is <8 x i32> (D = A·B + C in i32). The fragment row/col
    // mapping across lanes is identical to the f16/bf16 path — only the per-lane
    // packing and the intrinsic (signed operands, no clamp) differ.
    //
    // Native configs (16x16x16): f16/bf16 A·B → f32 accumulator, int8 A·B → i32
    // accumulator. Anything else falls to the portable Software tier (CM6).

    ImplTier coopMatrixTier(llvm::Type* elem, uint32_t rows, uint32_t cols,
                            uint32_t use) override {
        // Native 16x16x16 WMMA configs, by tile role:
        //   A/B operands : f16, bf16, or int8 (the iu8 path).
        //   accumulator  : f32 (for the f16/bf16 GEMMs) or int32 (for iu8).
        // The role-awareness keeps an int32 *operand* (no WMMA) on the portable
        // tier while an int32 *accumulator* (the iu8 output) is native — the two
        // share an LLVM type but not a config.
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

    // RDNA3 WMMA exists only in the wave32 encoding — mark the kernel so the
    // AMDGPU subtarget compiles this function at wavefront size 32.
    void prepareNativeCoopMatrix(llvm::Function* fn) override {
        fn->addFnAttr("target-features", "+wavefrontsize32");
    }

    llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem,
                               uint32_t /*rows*/, uint32_t /*cols*/,
                               uint32_t use) override {
        llvm::LLVMContext& ctx = m.getContext();
        if (use == 2) {
            // Accumulator fragment: 8 per lane — f32 for f16/bf16 GEMMs,
            // i32 for the iu8 (int8) GEMM.
            llvm::Type* ae = elem->isIntegerTy()
                ? (llvm::Type*) llvm::Type::getInt32Ty(ctx)
                : (llvm::Type*) llvm::Type::getFloatTy(ctx);
            return llvm::FixedVectorType::get(ae, 8);
        }
        // int8 A/B operand: the 16 K-values are packed 4-per-i32 → <4 x i32>
        // (the iu8 WMMA fragment encoding).
        if (elem->isIntegerTy(8))
            return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(ctx), 4);
        // f16/bf16 A/B operand fragment: 16 elements per lane (bf16 carried as
        // i16 for the intrinsic).
        llvm::Type* fe = elem->isBFloatTy()
            ? (llvm::Type*) llvm::Type::getInt16Ty(ctx) : elem;
        return llvm::FixedVectorType::get(fe, 16);
    }

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
        // int8 A/B operand: an <4 x i32> fragment, each i32 packing 4 consecutive
        // K-values (k = 4*w .. 4*w+3) as little-endian bytes — the iu8 WMMA
        // encoding. (The accumulator, use==2, is <8 x i32> with one value per
        // element and takes the generic per-element path below.)
        if (use != 2 && fe->isIntegerTy(32)) {
            llvm::LLVMContext& ctx = m.getContext();
            llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
            for (unsigned w = 0; w < n; ++w) {
                llvm::Value* word = llvm::ConstantInt::get(fe, 0);
                for (unsigned s = 0; s < 4; ++s) {
                    auto rc = fragCoord(b, m, use, 4 * w + s, layout, stride, swz, blk);
                    llvm::Value* p = b.CreateGEP(i8, ptr, rc, "cm.ld.ptr");
                    llvm::Value* byte = b.CreateLoad(i8, p, "cm.ld");
                    // zext keeps the raw 8 bits; shift into byte slot s and OR.
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

    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* a, llvm::Value* bMat,
                                  llvm::Value* c, llvm::Type* /*matrixType*/)
            override {
        // Pick the intrinsic by the A/B element type: <16 x half> -> f16 WMMA,
        // <16 x i16> -> bf16 WMMA, <4 x i32> (packed int8) -> iu8 WMMA.
        llvm::Type* ae =
            llvm::cast<llvm::FixedVectorType>(a->getType())->getElementType();
        if (ae->isIntegerTy(32)) {
            // int8 GEMM: D[<8 x i32>] = A[<4 x i32>]·B + C, signed operands,
            // no output clamp (the i32 accumulator can't overflow a 16-term
            // sum of int8 products, matching the host int32 reference).
            llvm::LLVMContext& ctx = m.getContext();
            llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::amdgcn_wmma_i32_16x16x16_iu8,
                {c->getType(), a->getType()});
            llvm::Value* tru = llvm::ConstantInt::getTrue(ctx);   // A/B signed
            llvm::Value* fls = llvm::ConstantInt::getFalse(ctx);  // no clamp
            return b.CreateCall(f, {tru, a, tru, bMat, c, fls}, "wmma.iu8");
        }
        llvm::Intrinsic::ID id = ae->isHalfTy()
            ? llvm::Intrinsic::amdgcn_wmma_f32_16x16x16_f16
            : llvm::Intrinsic::amdgcn_wmma_f32_16x16x16_bf16;
        // Overloaded on (D/C type, A/B type), in first-appearance order.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, id, {c->getType(), a->getType()});
        return b.CreateCall(f, {a, bMat, c}, "wmma");
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
    // The global linear index for fragment element `e` of a tile with the given
    // `use`, on the current lane — the heart of the WMMA layout. Returns the
    // element offset into the tile base (row-major `row*stride+col`, column-major
    // `col*stride+row`).
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
        // Swizzled<T,S> tile: permute the fragment coord by the same conflict-free
        // XOR the staging used (WMMA sub-tile offsets are S²-aligned, so this
        // fragment-local swizzle equals the absolute one).
        if (swz) { idx = swizzleAddr(b, idx, swz); return idx; }
        // BlockPadded<T,Block,Pad>: when an operand fragment provably fits one pad block
        // (constant stride, 15*stride+15 < Block) and the panel base is block-aligned, pad
        // the panel base ALONE and add the per-lane + e term unpadded — pad(panelBase +
        // lane*stride + e) == pad(panelBase) + lane*stride + e with no block boundary inside
        // the fragment. The base pad folds to a compile-time constant for a constant panel
        // offset (the unrolled-K case), so the K-loop carries zero pad VALU and e rides into
        // the ds_read offset: immediate. Otherwise fall back to padding the e=0 base once and
        // re-adding e (affine in e, reads stay ds_read_b128). ptr is the bare base; baseOffset
        // is logical. See specs/archive/amdgpu-constant-folded-lds-spec.md §1.4.
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
