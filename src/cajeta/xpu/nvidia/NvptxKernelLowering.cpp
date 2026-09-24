// NVPTX kernel lowering — see header. The AST walk is the shared
// xpu/lowering/KernelLowering.cpp; this file is the NVPTX LoweringTarget.

#include "../core/KernelManifest.h"
#include "NvptxKernelLowering.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "../core/XpuKernelAttr.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include <iostream>
#include <string>
#include <vector>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

class NvptxTarget : public LoweringTarget {
public:
    const char* name() const override { return "nvptx"; }
    // `!nontemporal` becomes the `.cs` qualifier on ld.global / st.global.
    bool supportsNontemporal() const override { return true; }

    // There is no NVPTX RT-core seam, so the noun builds a software BVH; the
    // base default routes RayQuery to rayQueryType(), which throws here.
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

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
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_barrier_cta_sync_aligned_all);
        b.CreateCall(f, {llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(m.getContext()), 0)});
    }

    void memoryFence(llvm::IRBuilderBase& b, llvm::Module& m, FenceScope scope,
                     MemoryOrder /*order*/ = MemoryOrder::Default) override {
        // membar has no weaker variant, so the order argument changes nothing.
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, scope == FenceScope::Workgroup
                    ? llvm::Intrinsic::nvvm_membar_cta
                    : llvm::Intrinsic::nvvm_membar_gl);
        b.CreateCall(f, {});
    }

    // Global->shared copy through cp.async (sm_80+), bypassing the register
    // file. It carries 4/8/16-byte elements ONLY; other widths need the base seam.
    void asyncCopy(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* dstBase,
                   llvm::Type* dstElem, llvm::Value* dstOffset, llvm::Value* srcBase,
                   llvm::Type* srcElem, llvm::Value* srcOffset,
                   llvm::Value* count) override {
        llvm::LLVMContext& ctx = m.getContext();
        uint64_t elemBytes = m.getDataLayout().getTypeStoreSize(srcElem);
        llvm::Intrinsic::ID cp;
        switch (elemBytes) {
            case 4:  cp = llvm::Intrinsic::nvvm_cp_async_ca_shared_global_4; break;
            case 8:  cp = llvm::Intrinsic::nvvm_cp_async_ca_shared_global_8; break;
            case 16: cp = llvm::Intrinsic::nvvm_cp_async_ca_shared_global_16; break;
            default:
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
        llvm::Function* cpAsync = llvm::Intrinsic::getOrInsertDeclaration(&m, cp);
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
        llvm::Value* dPtr = bufferElementPtr(   // addrspace(3) shared destination
            b, m, dstBase, dstElem, b.CreateZExt(b.CreateAdd(dOff, e), i64));
        b.CreateCall(cpAsync, {dPtr, sPtr});    // cp.async is (shared dst, global src)
        e->addIncoming(b.CreateAdd(e, nthr), body);
        b.CreateBr(head);
        b.SetInsertPoint(exit);
    }

    void asyncCommit(llvm::IRBuilderBase& b, llvm::Module& m) override {
        b.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_cp_async_commit_group), {});
    }

    // Block until at most `groupsInFlight` groups remain. cp.async.wait_group
    // takes an IMMEDIATE, so a dynamic count must drain all instead.
    void asyncWait(llvm::IRBuilderBase& b, llvm::Module& m,
                   llvm::Value* groupsInFlight) override {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(groupsInFlight)) {
            b.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                             &m, llvm::Intrinsic::nvvm_cp_async_wait_group),
                         {llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()),
                                                 c->getZExtValue())});
        } else {
            b.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                &m, llvm::Intrinsic::nvvm_cp_async_wait_all), {});
        }
    }

    void devicePrintf(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* fmt,
                      llvm::ArrayRef<llvm::Value*> args) override {
        // CUDA device printf is `i32 vprintf(i8* fmt, i8* args)`: the varargs
        // pack in declaration order (f32→double) into a stack buffer.
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
        llvm::Metadata* ops[] = {
            llvm::ValueAsMetadata::get(fn),
            llvm::MDString::get(ctx, "kernel"),
            llvm::ValueAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1)),
        };
        m.getOrInsertNamedMetadata("nvvm.annotations")
            ->addOperand(llvm::MDNode::get(ctx, ops));
    }

    // @Occupancy → nvvm.annotations: maxntidx, minctasm, maxnreg.
    void applyOccupancy(llvm::Function* fn, const XpuKernelAttr& attr) override {
        llvm::Module& m = *fn->getParent();
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto annotate = [&](const char* key, unsigned val) {
            llvm::Metadata* ops[] = {
                llvm::ValueAsMetadata::get(fn),
                llvm::MDString::get(ctx, key),
                llvm::ValueAsMetadata::get(llvm::ConstantInt::get(i32, val)),
            };
            m.getOrInsertNamedMetadata("nvvm.annotations")
                ->addOperand(llvm::MDNode::get(ctx, ops));
        };
        if (auto mt = attr.maxThreads())   annotate("maxntidx", *mt);
        if (auto mr = attr.minResident())  annotate("minctasm", *mr);
        if (auto rr = attr.maxRegisters()) annotate("maxnreg", *rr);
    }

    // Wave ops: NVIDIA warps are 32 wide; shuffle + ballot are hardware.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return readSreg(b, m, llvm::Intrinsic::nvvm_read_ptx_sreg_warpsize);
    }
    llvm::Value* waveShuffle(llvm::IRBuilderBase& b, llvm::Module& m,
                             llvm::Value* value, llvm::Value* srcLane) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_shfl_sync_idx_i32);
        return b.CreateCall(f, {llvm::ConstantInt::get(i32, 0xFFFFFFFFu), value,
                                srcLane, llvm::ConstantInt::get(i32, 0x1Fu)},
                            "shfl");
    }
    llvm::Value* waveBallot(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* pred) override {
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
        // redux.sync.add.s32 requires sm_80+.
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_redux_sync_add);
        return b.CreateCall(f, {value, llvm::ConstantInt::get(i32, 0xFFFFFFFFu)},
                            "redux");
    }
    llvm::Value* waveReduce(llvm::IRBuilderBase& b, llvm::Module& m,
                            WaveReduceOp op, llvm::Value* value) override {
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

    // `v[i]` with a runtime `i`: a select chain, not the backend's stack frame.
    //
    // NVPTX legalizes a dynamic extractelement by writing the whole vector to
    // the local frame and loading one lane back. On a GPU that is scratch, and
    // the spill gate reports it as untuned register pressure — which it is not:
    // `q4kMatVecKernelIL` paid 64 bytes for it at vgpr=48, with 207 registers
    // of headroom. Worse, the vector is usually rebuilt inside the loop that
    // indexes it, so the stores re-execute every iteration.
    //
    // The lanes are already live SSA values, so choosing among them costs N-1
    // `selp` and no memory. Two further properties worth stating: an
    // out-of-range index selects `i mod N` instead of yielding poison, which is
    // strictly safer than the extractelement this replaces; and the chain is
    // uniform across the warp whether or not `i` is, where the frame round trip
    // is not.
    //
    // Capped at 16 lanes. Every depot measured in the llm library was 16, 32 or
    // 64 bytes — 4, 8 or 16 lanes — and past that the chain stops being
    // obviously cheaper than the round trip, so a wider vector keeps the
    // default rather than being guessed at.
    llvm::Value* extractLaneDynamic(llvm::IRBuilderBase& b, llvm::Module& m,
                                    llvm::Value* vec,
                                    llvm::Value* idx) override {
        auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(vec->getType());
        const unsigned n = vt ? vt->getNumElements() : 0;
        if (vt == nullptr || llvm::isa<llvm::ConstantInt>(idx)
                || n < 2 || n > 16 || (n & (n - 1)) != 0
                || !vt->getElementType()->isSingleValueType())
            return LoweringTarget::extractLaneDynamic(b, m, vec, idx);

        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* k = b.CreateZExtOrTrunc(idx, i32, "lane.i");
        llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
        std::vector<llvm::Value*> cur;
        cur.reserve(n);
        for (unsigned i = 0; i < n; ++i)
            cur.push_back(b.CreateExtractElement(vec, i, "lane.v"));
        for (unsigned bit = 0; (1u << bit) < n; ++bit) {
            llvm::Value* set = b.CreateICmpNE(
                b.CreateAnd(k, llvm::ConstantInt::get(i32, 1u << bit),
                            "lane.m"),
                zero, "lane.b");
            std::vector<llvm::Value*> next;
            next.reserve(cur.size() / 2);
            for (std::size_t j = 0; j + 1 < cur.size(); j += 2)
                next.push_back(
                    b.CreateSelect(set, cur[j + 1], cur[j], "lane.sel"));
            cur.swap(next);
        }
        return cur.front();
    }

    // 16-entry int8 LUT by 4-bit index via prmt.b32, the NVPTX byte permute.
    //
    // Without this the shared default allocas the table and gathers it a lane at
    // a time. An alloca is `.local` here, so each lut4 cost 16 BYTES OF SCRATCH
    // per work-item and 16 `ld.local.b8` — measured on sm_89 as a clean
    // dose-response, 0 / 16 / 32 bytes for 0 / 1 / 2 calls, and it is the whole
    // of the 32 bytes the spill gate reports for the Q4_K/MXFP4/IQ4_NL mat-vecs
    // (they call lut4 twice, for the low and high nibble).
    //
    // prmt is NOT v_perm_b32 with a different name, and transliterating the
    // AMDGPU override would be wrong in two ways:
    //
    //   * the selector is four NIBBLES (16 bits), not four BYTES (32 bits), so
    //     the four byte indices must be compacted first;
    //   * a selector nibble whose bit 3 is set means "replicate the sign bit of
    //     the selected byte", not "byte 8". Every selector built below is masked
    //     so bit 3 is clear, which keeps the permute in plain byte-select mode.
    //
    // The compaction is pure ALU: with the index bytes at bits 0/8/16/24,
    // `x = m | (m >> 4)` lands b0,b1 in bits 0-7 and b2,b3 in bits 16-23, and
    // `x | (x >> 8)` then lands all four nibbles in the low 16 bits. Doing it on
    // the full 4-bit index (rather than per half) means the same compaction
    // serves both the table selector and the half-select.
    llvm::Value* byteLut16(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* indices, llvm::Value* table) override {
        auto* ivt = llvm::dyn_cast<llvm::FixedVectorType>(indices->getType());
        if (ivt == nullptr || (ivt->getNumElements() % 4) != 0)
            return LoweringTarget::byteLut16(b, m, indices, table);
        const bool loHalfOnly = masksOffHighHalf(indices);
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        const unsigned groups = ivt->getNumElements() / 4;
        auto* t4 = llvm::FixedVectorType::get(i32, 4);
        llvm::Value* tw = b.CreateBitCast(table, t4, "lut.tw");
        llvm::Value* t0 = b.CreateExtractElement(tw, (uint64_t) 0, "lut.t0");
        llvm::Value* t1 = b.CreateExtractElement(tw, (uint64_t) 1, "lut.t1");
        llvm::Value* t2 = b.CreateExtractElement(tw, (uint64_t) 2, "lut.t2");
        llvm::Value* t3 = b.CreateExtractElement(tw, (uint64_t) 3, "lut.t3");
        auto* iw = llvm::FixedVectorType::get(i32, groups);
        llvm::Value* idxW = b.CreateBitCast(indices, iw, "lut.iw");
        llvm::Function* prmt = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_prmt);
        llvm::Value* nibMask  = llvm::ConstantInt::get(i32, 0x0F0F0F0Fu);
        llvm::Value* pairMask = llvm::ConstantInt::get(i32, 0x00FF00FFu);
        llvm::Value* selMask  = llvm::ConstantInt::get(i32, 0x00007777u);
        llvm::Value* hiMask   = llvm::ConstantInt::get(i32, 0x00008888u);
        llvm::Value* base     = llvm::ConstantInt::get(i32, 0x00003210u);
        llvm::Value* out = llvm::UndefValue::get(iw);
        for (unsigned g = 0; g < groups; ++g) {
            llvm::Value* idx = b.CreateExtractElement(idxW, g, "lut.g");
            // four byte indices -> four nibbles, in the low 16 bits
            llvm::Value* n = b.CreateAnd(idx, nibMask, "lut.n");
            n = b.CreateAnd(b.CreateOr(n, b.CreateLShr(n, 4), "lut.p"),
                            pairMask, "lut.pm");
            n = b.CreateOr(n, b.CreateLShr(n, 8), "lut.nib");
            llvm::Value* sel = b.CreateAnd(n, selMask, "lut.sel");
            llvm::Value* lo = b.CreateCall(prmt, {t0, t1, sel}, "lut.lo");
            if (loHalfOnly) {
                out = b.CreateInsertElement(out, lo, g, "lut.out");
                continue;
            }
            llvm::Value* hi = b.CreateCall(prmt, {t2, t3, sel}, "lut.hi");
            // nibble i picks byte i of `lo` or byte i+4 of `hi`, off index bit 3
            llvm::Value* mb = b.CreateOr(base,
                b.CreateLShr(b.CreateAnd(n, hiMask, "lut.h"), 1), "lut.mb");
            llvm::Value* res = b.CreateCall(prmt, {lo, hi, mb}, "lut.res");
            out = b.CreateInsertElement(out, res, g, "lut.out");
        }
        return b.CreateBitCast(out, ivt, "lut.bytes");
    }

    // ---- Cooperative matrix: NVIDIA tensor cores (wmma) ----------------------
    // m16n16k16 D[f32] = A[f16/bf16]·B[f16/bf16] + C[f32], row-major, warp-collective.
    // The fragment↔lane layout is implementation-defined: load/store MUST use NVVM wmma.

    ImplTier coopMatrixTier(llvm::Type* elem, uint32_t rows, uint32_t cols,
                            uint32_t use) override {
        if (rows == 16 && cols == 16) {
            if (use == 2) {                        // accumulator: f32 or s32
                if (elem->isFloatTy() || elem->isIntegerTy(32))
                    return ImplTier::Native;
            } else if (elem->isHalfTy() || elem->isBFloatTy()   // A/B operand
                       || elem->isIntegerTy(8)) {
                return ImplTier::Native;
            }
        }
        // f64 and every other shape keep the portable tile. 8-bit operands go
        // native from here (4A.2.7): they were 58 of the 66 tier fallbacks the
        // llm library emitted on sm_89, and the tile IS their scratch. The
        // signedness question they raise cannot be answered HERE — this hook
        // sees one operand at a time and LLVM integers are signless — so it is
        // settled in coopMatrixMulAdd, which has both operands' signFlags.
        return ImplTier::Portable;
    }

    // sm_89 already has tensor cores, so no kernel ABI attribute is needed.
    void prepareNativeCoopMatrix(llvm::Function* /*fn*/) override {}

    llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type* elem,
                               uint32_t /*rows*/, uint32_t /*cols*/,
                               uint32_t use) override {
        // The fragment type IS the wmma.load return struct: everything matches.
        return nvWmmaLoadDecl(m, use, elem)->getReturnType();
    }

    llvm::Value* coopMatrixLoad(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* ptr, llvm::Value* layout,
                                llvm::Value* stride, llvm::Type* matrixType,
                                uint32_t /*rows*/, uint32_t /*cols*/,
                                uint32_t use, uint32_t swz = 0,
                                LdsBlockPad /*blk*/ = {}) override {
        if (swz) {
            // Degrade to identity: NVPTX stages the tile unpermuted too.
            std::cerr << "note: [swizzle-tier] CooperativeMatrix.load from a "
                         "Swizzled<T,S> tile uses the IDENTITY layout on NVPTX "
                         "(no per-element WMMA swizzle); correct, unaccelerated.\n";
        }
        uint32_t lay = constLayout(layout, "load");
        llvm::Function* f = nvWmmaLoadDecl(m, use, nvFragScalar(matrixType),
                                           ptr->getType(), lay);
        // 4A.2.2 chose the layout variant; the manifest records which (4A.2.6).
        cajeta::xpu::recordNativeOp(
            b, use == 0 ? "load.a" : use == 1 ? "load.b" : "load.c",
            cajeta::xpu::nativeInstructionName(
                nvWmmaLoadId(use, nvFragScalar(matrixType), lay)));
        return b.CreateCall(f, {ptr, stride}, "wmma.ld");
    }

    void coopMatrixStore(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
                         llvm::Value* matrixVal, llvm::Value* layout,
                         llvm::Value* stride, uint32_t /*rows*/, uint32_t /*cols*/,
                         uint32_t /*use*/, uint32_t swz = 0,
                         LdsBlockPad /*blk*/ = {}) override {
        if (swz) {
            // U5.3: degrade to identity, don't reject (see coopMatrixLoad).
            std::cerr << "note: [swizzle-tier] CooperativeMatrix.store to a "
                         "Swizzled<T,S> tile uses the IDENTITY layout on NVPTX "
                         "(no per-element WMMA swizzle); correct, unaccelerated.\n";
        }
        uint32_t lay = constLayout(layout, "store");
        // An s32 accumulator is {i32 x 8}; f32's is {float x 8}. The element
        // type picks the store, and getting it wrong is an IR verifier error
        // rather than a silent miscompile, which is the one mercy here.
        auto* dFrag = llvm::cast<llvm::StructType>(matrixVal->getType());
        const bool i32Acc = dFrag->getElementType(0)->isIntegerTy(32);
        llvm::Intrinsic::ID sid;
        if (i32Acc)
            sid = lay == 1
                ? llvm::Intrinsic::nvvm_wmma_m16n16k16_store_d_s32_col_stride
                : llvm::Intrinsic::nvvm_wmma_m16n16k16_store_d_s32_row_stride;
        else
            sid = lay == 1
                ? llvm::Intrinsic::nvvm_wmma_m16n16k16_store_d_f32_col_stride
                : llvm::Intrinsic::nvvm_wmma_m16n16k16_store_d_f32_row_stride;
        cajeta::xpu::recordNativeOp(b, "store", cajeta::xpu::nativeInstructionName(sid));
        llvm::Function* f = nvDecl(m, sid, ptr->getType());
        std::vector<llvm::Value*> args;
        args.push_back(ptr);
        appendStructElems(b, matrixVal, args);
        args.push_back(stride);
        b.CreateCall(f, args);
    }

    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* a, llvm::Value* bMat,
                                  llvm::Value* c, llvm::Type* /*matrixType*/,
                                  uint32_t signFlags, uint32_t aLayout = 0,
                                  uint32_t bLayout = 0) override {
        // Pick mma by the A fragment's SHAPE, never a scalar probe: f16 A/B is
        // {<2 x half> x 8} and bf16 is {i32 x 4}, so casting element 0 to
        // FixedVectorType asserts inside LLVM, uncatchably.
        auto* aFrag = llvm::cast<llvm::StructType>(a->getType());
        bool bf = !llvm::isa<llvm::FixedVectorType>(aFrag->getElementType(0));
        // wmma.mma encodes BOTH operand layouts. Pairing them wrongly lowers
        // cleanly and computes the wrong product, which is why the layouts
        // are threaded here from the loads rather than assumed row/row.
        bool aCol = aLayout == 1, bCol = bLayout == 1;
        llvm::Intrinsic::ID id;

        // 8-bit operands: {i32 x 2}, which the shape distinguishes from
        // bf16's {i32 x 4} (4A.2.7).
        if (bf && aFrag->getNumElements() == 2) {
            // PTX has .s8 and .u8 and NO MIXED FORM, so A and B must agree.
            // Picking one silently would read a signed -1 as 255. Refuse and
            // name the escape hatch instead — this combination is expressible
            // (dotAccum takes unsigned weights against signed activations) and
            // simply has no instruction.
            const bool aSigned = (signFlags & 0x1u) != 0;
            const bool bSigned = (signFlags & 0x2u) != 0;
            if (aSigned != bSigned)
                throw cajeta::Exception(
                    std::string("XPU NVPTX cooperative matrix: "
                    "CooperativeMatrix.mma got mismatched operand SIGNEDNESS (")
                    + (aSigned ? "signed" : "unsigned") + " A against "
                    + (bSigned ? "signed" : "unsigned") + " B). wmma has .s8 "
                    "and .u8 8-bit forms and no mixed one, and executing one "
                    "as the other reads -1 as 255. Give both operands the same "
                    "dtype, or force the portable tier with "
                    "CAJETA_GPU_COOPMATRIX_IMPL=software.", "XPU-N04");
            // The NON-saturating form, deliberately: the portable software
            // tile accumulates with plain mul/add carrying no nsw/nuw, so it
            // WRAPS. `.satfinite` clamps, which would agree on every input
            // that does not overflow and disagree on the ones that do — and
            // this backend is checked against that tile bit for bit.
            if (aSigned)
                id = aCol ? (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_col_s8
                                  : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_row_s8)
                          : (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_col_s8
                                  : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_row_s8);
            else
                id = aCol ? (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_col_u8
                                  : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_row_u8)
                          : (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_col_u8
                                  : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_row_u8);
            std::vector<llvm::Value*> iargs;
            appendStructElems(b, a, iargs);
            appendStructElems(b, bMat, iargs);
            appendStructElems(b, c, iargs);
            cajeta::xpu::recordNativeOp(b, "mma", cajeta::xpu::nativeInstructionName(id));
            return b.CreateCall(nvDecl(m, id), iargs, "wmma.mma");
        }

        if (bf) {
            id = aCol ? (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_col_bf16
                              : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_row_bf16)
                      : (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_col_bf16
                              : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_row_bf16);
        } else {
            id = aCol ? (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_col_f32_f32
                              : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_col_row_f32_f32)
                      : (bCol ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_col_f32_f32
                              : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_row_f32_f32);
        }
        std::vector<llvm::Value*> args;
        appendStructElems(b, a, args);
        appendStructElems(b, bMat, args);
        appendStructElems(b, c, args);
        cajeta::xpu::recordNativeOp(b, "mma", cajeta::xpu::nativeInstructionName(id));
        return b.CreateCall(nvDecl(m, id), args, "wmma.mma");
    }

    llvm::Value* coopMatrixSplat(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* value,
                                 llvm::Type* matrixType) override {
        (void) m;
        auto* st = llvm::cast<llvm::StructType>(matrixType);
        llvm::Type* fe = st->getElementType(0);
        llvm::Value* elemVal;
        if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(fe)) {
            elemVal = b.CreateVectorSplat(
                vt->getNumElements(), coerceScalar(b, value, vt->getElementType()));
        } else if (fe->isIntegerTy(32)) {
            // bf16 A/B fragment: build the .b32 register image, since a plain
            // coerceScalar would put an unpacked bfloat in the i32 slot.
            llvm::Type* bfTy = llvm::Type::getBFloatTy(b.getContext());
            llvm::Value* pair =
                b.CreateVectorSplat(2, coerceScalar(b, value, bfTy));
            elemVal = b.CreateBitCast(pair, fe);
        } else {
            elemVal = coerceScalar(b, value, fe);
        }
        llvm::Value* agg = llvm::UndefValue::get(st);
        for (unsigned e = 0; e < st->getNumElements(); ++e)
            agg = b.CreateInsertValue(agg, elemVal, e);
        return agg;
    }

    // --- fromWords, staged rather than refused (plan 4A.2.4) --------------
    //
    // The verb hands over four i32 words that on AMD WMMA ARE this lane's
    // fragment: 16 bytes per lane, lane L owning column L%16, the same 256
    // bytes held twice across the two half-waves of a wave32. NVIDIA's
    // m16n16k16 s8 operand fragment is {i32 x 2} -- eight bytes per lane,
    // 32 lanes, 256 bytes with NO duplication and a different partition. A
    // lane therefore does not hold the bytes its own slots need, and no
    // amount of register shuffling within the lane will produce them: the
    // data has to cross lanes.
    //
    // So the words are treated as what they logically are -- column L%16 of
    // the operand -- staged into shared memory in that layout, and re-loaded
    // through the ordinary wmma load, which performs the redistribution in
    // hardware. Column-major with stride 16 is exactly "element (k,n) at
    // n*16 + k", which is the placement below.
    //
    // This is deliberately done HERE rather than in the kernels. The four
    // kernels that call `fromWords` are hot, their stated design point is
    // that the weight side never touches LDS, and rewriting their 11 call
    // sites to stage by hand would put that cost on AMD as well -- on
    // hardware this machine does not have and cannot measure. Staging in the
    // lowering confines it to the backend with no alternative and leaves
    // AMD's single-instruction path untouched.
    bool coopMatrixFromWordsSupported() const override { return true; }
    bool coopMatrixFromWordsIsLaneFragment() const override { return false; }

    llvm::Value* coopMatrixFromWords(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* const w[4],
            llvm::Type* matrixType, uint32_t rows, uint32_t cols,
            uint32_t use) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);

        if (rows != 16 || cols != 16)
            throw cajeta::Exception(
                std::string("XPU NVPTX cooperative matrix: "
                "CooperativeMatrix.fromWords stages through shared memory "
                "and the staging layout is established only for the 16x16 "
                "operand, not ") + std::to_string(rows) + "x" +
                std::to_string(cols) + ".", "XPU-N04");

        // The staging buffer is PER WAVE, because every wave in the block
        // builds a different column set at the same moment. Sizing it needs
        // the block bound, and guessing it is exactly the kind of invented
        // constant this project treats as a defect: too small silently
        // corrupts a neighbouring wave's columns, too large eats the LDS the
        // kernel needs for its own tiles. So the bound is REQUIRED, and it
        // is a structural fact the author already knows.
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        unsigned maxThreads = 0;
        if (auto* na = m.getNamedMetadata("nvvm.annotations")) {
            for (auto* nd : na->operands()) {
                if (!nd || nd->getNumOperands() != 3) continue;
                auto* vam =
                    llvm::dyn_cast<llvm::ValueAsMetadata>(nd->getOperand(0));
                if (!vam || vam->getValue() != fn) continue;
                auto* key = llvm::dyn_cast<llvm::MDString>(nd->getOperand(1));
                if (!key || key->getString() != "maxntidx") continue;
                if (auto* cv = llvm::mdconst::dyn_extract_or_null<
                        llvm::ConstantInt>(nd->getOperand(2)))
                    maxThreads = (unsigned) cv->getZExtValue();
            }
        }
        if (maxThreads == 0)
            throw cajeta::Exception(
                std::string("XPU NVPTX cooperative matrix: "
                "CooperativeMatrix.fromWords needs the block's thread bound "
                "to size its per-wave staging buffer, and kernel '") +
                fn->getName().str() + "' does not declare one. NVIDIA cannot "
                "build this fragment from the calling lane's own words (its "
                "operand fragment is {i32 x 2} against AMD's <4 x i32>), so "
                "the words are staged in shared memory and re-loaded, and "
                "one wave's columns must not land on another's. Declare the "
                "structural bound the launch already obeys: "
                "@Occupancy(maxThreads = N).", "XPU-N04");

        const unsigned waves = (maxThreads + 31u) / 32u;
        const unsigned tileBytes = rows * cols;          // 16x16 int8 = 256
        const unsigned totalBytes = waves * tileBytes;

        // One buffer per (kernel, shape), reused by every fromWords in it:
        // each use stages, syncs and loads before the next can run.
        const std::string gname =
            "__cajeta.fromwords." + fn->getName().str() + "." +
            std::to_string(rows) + "x" + std::to_string(cols);
        llvm::GlobalVariable* stage =
            m.getNamedGlobal(gname);
        if (!stage) {
            llvm::ArrayType* at = llvm::ArrayType::get(i8, totalBytes);
            stage = new llvm::GlobalVariable(
                m, at, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::UndefValue::get(at), gname, nullptr,
                llvm::GlobalValue::NotThreadLocal, /*addrspace=*/3);
            stage->setAlignment(llvm::Align(16));
        }

        llvm::Value* lane = waveLaneId(b, m);
        llvm::Value* tid = readSreg(b, m, llvm::Intrinsic::nvvm_read_ptx_sreg_tid_x);
        llvm::Value* wid = b.CreateLShr(tid, llvm::ConstantInt::get(i32, 5),
                                        "fw.wid");
        llvm::Value* col = b.CreateAnd(lane, llvm::ConstantInt::get(i32, 15),
                                       "fw.col");

        // (k, n) at n*16 + k, which is column-major with stride 16.
        llvm::Value* waveOff =
            b.CreateMul(wid, llvm::ConstantInt::get(i32, tileBytes),
                        "fw.waveoff");
        llvm::Value* myOff = b.CreateAdd(
            waveOff, b.CreateMul(col, llvm::ConstantInt::get(i32, cols)),
            "fw.myoff");
        llvm::Value* myPtr =
            b.CreateGEP(i8, stage, {myOff}, "fw.myptr");
        for (unsigned k = 0; k < 4; ++k) {
            llvm::Value* p = b.CreateGEP(
                i32, myPtr, {llvm::ConstantInt::get(i32, k)}, "fw.wptr");
            b.CreateAlignedStore(w[k], p, llvm::Align(4));
        }

        // A WARP sync, not a block one. These kernels run eight waves and the
        // call sites sit inside loops the waves need not enter together, so a
        // block-wide barrier here would be a deadlock waiting to happen. The
        // staging is wave-private, so warp scope is also all that is needed.
        b.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                         &m, llvm::Intrinsic::nvvm_bar_warp_sync),
                     {llvm::ConstantInt::get(i32, 0xffffffffu)});

        llvm::Value* wavePtr =
            b.CreateGEP(i8, stage, {waveOff}, "fw.waveptr");
        llvm::Value* frag =
            coopMatrixLoad(b, m, wavePtr,
                           llvm::ConstantInt::get(i32, 1),   // col-major
                           llvm::ConstantInt::get(i32, cols),
                           matrixType, rows, cols, use);

        // A SECOND sync, for the hazard in the other direction. Every call in
        // a kernel shares this one buffer, so the next `fromWords` round
        // stores over what this load is still reading -- write-after-read, not
        // read-after-write. It is tempting to assume a warp's lanes move in
        // lockstep and skip it; since Volta they do not, independent thread
        // scheduling lets them diverge and reconverge freely, so a lane can
        // reach the next store while its neighbour is still loading.
        //
        // HONESTY NOTE: this was added on the hypothesis that the hazard
        // explained two llm kernels disagreeing with their reference. IT DID
        // NOT -- the suite was unchanged with and without it, and the cause
        // proved to be elsewhere. It is kept because the hazard is real
        // regardless of that bug, and costs one warp sync per call. Do not
        // read it as a measured fix for anything.
        b.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                         &m, llvm::Intrinsic::nvvm_bar_warp_sync),
                     {llvm::ConstantInt::get(i32, 0xffffffffu)});
        return frag;
    }

    // --- the fused GEMM epilogue (plan 4A.2.8) ----------------------------
    //
    // Why this is the last thing holding the spill gate. When this returns
    // false the tier scan demotes EVERY tile in a kernel that touches
    // scaledAccumInto/rank1Accum to the portable software tile, and that tile
    // IS the scratch: measured on the two probe kernels, 1024 bytes of frame
    // for a lone rank1Accum and 2048 for a scaledAccumInto. Seven of the
    // eleven kernels still on the sm_89 spill list are demoted this way.
    //
    // What it costs to say true. The contract is
    //     facc[r][c] += rowF[r] * colF[c] * acc[r][c]
    // per fragment element OF THIS LANE, so the backend has to map element ->
    // (row, column). AMD hardcodes its own wave32 layout; this is the NVIDIA
    // equivalent, and it is the one piece of per-vendor knowledge the verb
    // genuinely needs.
    //
    // ESTABLISHED BY MEASUREMENT, NOT FROM A TABLE. wmma's fragment layout is
    // documented for the PTX instruction but the earlier note in this plan
    // (and llama.cpp's own header) treats the CUDA C++ wmma fragment as
    // opaque, and I have misdiagnosed this blocker once already. So the
    // mapping below is checked on the device by
    // NvptxCoopEpilogueTests.theFragmentLayoutPlacesEveryElementAtItsOwnRowAndColumn,
    // which drives two probes (rowF = ramp with colF = 1, then the reverse)
    // and PRINTS the true (row, column) of any position that disagrees. A
    // wrong formula there is not noise, it is a readable permutation.
    bool coopMatrixEpilogueSupported() const override { return true; }

    // Element e of an accumulator fragment, for either shape. NVPTX fragments
    // are STRUCTS (extractvalue), where AMD's are vectors (extractelement),
    // so the generic epilogue cannot be shared as written.
    static llvm::Value* fragGet(llvm::IRBuilderBase& b, llvm::Value* agg,
                                unsigned e) {
        if (llvm::isa<llvm::StructType>(agg->getType()))
            return b.CreateExtractValue(agg, e);
        return b.CreateExtractElement(agg, e);
    }
    static llvm::Value* fragSet(llvm::IRBuilderBase& b, llvm::Value* agg,
                                llvm::Value* v, unsigned e) {
        if (llvm::isa<llvm::StructType>(agg->getType()))
            return b.CreateInsertValue(agg, v, e);
        return b.CreateInsertElement(agg, v, e);
    }
    static unsigned fragCount(llvm::Type* t) {
        if (auto* st = llvm::dyn_cast<llvm::StructType>(t))
            return st->getNumElements();
        if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(t))
            return vt->getNumElements();
        return 0;
    }

    // THE SCALAR COLUMN FACTOR, and the one place AMD's fragment layout is
    // baked into a portable contract. CooperativeMatrix.cajeta:247 states it
    // outright: "on the native WMMA mapping the column of every element a
    // lane holds is `lane & 15`, so `colF[c]` is a per-lane constant". That
    // is TRUE ON AMD, where a wave32 lane owns exactly one column. It is
    // FALSE HERE: an m16n16k16 accumulator gives each lane eight elements
    // spanning FOUR columns, `col2 + {0, 1, 8, 9}`.
    //
    // The first version of this epilogue used the one scalar for all eight
    // elements. Column 0 came out right and the rest did not, which is
    // exactly what eleven cajeta-llm "Mw" kernels reported — element 0
    // correct, row 1 onward wrong — and what
    // NvptxCoopEpilogueTests.theScalarColumnFormReachesEveryColumnOfTheFragment
    // measures directly: 240 of 256 cells took another column's factor.
    //
    // The rest of the contract is what makes the fix exact rather than a
    // guess: "both lanes holding column `c` pass the same value", so column
    // c's factor is whatever lane c holds. One shuffle recovers it. Four
    // distinct columns per lane means at most four shuffles for a whole
    // fragment, against an LDS round trip and a barrier for the vector form
    // — the S-verbs keep the saving they were introduced for.
    llvm::Value* scalarColFactor(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* scalar, llvm::Value* col) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Type* ty = scalar->getType();
        llvm::Value* bits = ty->isIntegerTy(32) ? scalar
                                                : b.CreateBitCast(scalar, i32);
        llvm::Value* got = waveShuffle(b, m, bits, col);
        return ty->isIntegerTy(32) ? got : b.CreateBitCast(got, ty);
    }

    llvm::Value* coopMatrixEpilogueAccum(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* accVal,
            llvm::Value* faccVal, llvm::Value* rowFPtr, llvm::Type* rowETy,
            llvm::Value* colFPtr, llvm::Type* colETy,
            llvm::Value* rowGPtr = nullptr,
            llvm::Value* colGPtr = nullptr,
            llvm::Value* colFScalar = nullptr,
            llvm::Value* colGScalar = nullptr,
            llvm::Value* colFStride = nullptr,
            llvm::Value* colGStride = nullptr) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        // colF[c] lives at colFPtr[c*stride]; stride null means the contiguous
        // vector (stride 1). `col` is already this element's logical column.
        auto colIdx = [&](llvm::Value* col, llvm::Value* stride) -> llvm::Value* {
            if (!stride) return col;
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(stride))
                if (ci->isOne()) return col;
            return b.CreateMul(col, stride, "epi.cstride");
        };
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);

        const unsigned n = fragCount(faccVal->getType());
        // Only the m16n16k16 f32/s32 accumulator has a mapping established
        // here. Refuse anything else BY NAME rather than compute a wrong
        // (row, column) silently, which is the failure this whole unit is
        // about.
        if (n != 8)
            throw cajeta::Exception(
                std::string("XPU NVPTX cooperative matrix: the fused GEMM "
                "epilogue (scaledAccumInto/rank1Accum) has an established "
                "fragment layout only for the 8-element m16n16k16 "
                "accumulator, and this one has ") + std::to_string(n) +
                " elements. Its element-to-(row,column) mapping has not been "
                "measured, and guessing it would place every term in the "
                "wrong cell. Add the shape to "
                "NvptxCoopEpilogueTests.theFragmentLayoutPlacesEveryElement"
                "AtItsOwnRowAndColumn and read the mapping off the failure, "
                "or force the portable tier with "
                "CAJETA_GPU_COOPMATRIX_IMPL=software.", "XPU-N04");

        llvm::Value* lane = waveLaneId(b, m);
        // wmma m16n16k16 is two m16n8k16 halves side by side. Within a half,
        // the quad (lane>>2) selects the row pair and (lane&3) the column
        // pair, which is the documented mma.sync C/D layout.
        llvm::Value* grp = b.CreateLShr(lane, llvm::ConstantInt::get(i32, 2),
                                        "epi.grp");
        llvm::Value* tig = b.CreateAnd(lane, llvm::ConstantInt::get(i32, 3),
                                       "epi.tig");
        llvm::Value* col2 = b.CreateShl(tig, llvm::ConstantInt::get(i32, 1),
                                        "epi.col2");

        llvm::Value* out = faccVal;
        for (unsigned e = 0; e < n; ++e) {
            const unsigned h = e >> 2;    // which 8-wide column half
            const unsigned j = e & 3;     // position within the half
            llvm::Value* row = b.CreateAdd(
                grp, llvm::ConstantInt::get(i32, 8u * (j >> 1)), "epi.row");
            llvm::Value* col = b.CreateAdd(
                col2, llvm::ConstantInt::get(i32, 8u * h + (j & 1u)),
                "epi.col");

            llvm::Value* rv = b.CreateLoad(
                rowETy, b.CreateGEP(rowETy, rowFPtr, row, "epi.rf.ptr"),
                "epi.rf");
            llvm::Value* cv = colFScalar
                ? scalarColFactor(b, m, colFScalar, col)
                : b.CreateLoad(
                      colETy,
                      b.CreateGEP(colETy, colFPtr, colIdx(col, colFStride),
                                  "epi.cf.ptr"),
                      "epi.cf");
            llvm::Value* term = b.CreateFMul(rv, cv);
            if (accVal) {
                llvm::Value* av = fragGet(b, accVal, e);
                if (av->getType()->isIntegerTy())
                    av = b.CreateSIToFP(av, f32);
                term = b.CreateFMul(term, av);
            }
            if (rowGPtr) {
                llvm::Value* cgv = colGScalar
                    ? scalarColFactor(b, m, colGScalar, col) : nullptr;
                if (!cgv && colGPtr)
                    cgv = b.CreateLoad(
                        colETy,
                        b.CreateGEP(colETy, colGPtr, colIdx(col, colGStride),
                                    "epi.cg.ptr"),
                        "epi.cg");
                llvm::Value* rg = b.CreateLoad(
                    rowETy, b.CreateGEP(rowETy, rowGPtr, row, "epi.rg.ptr"),
                    "epi.rg");
                term = b.CreateFAdd(term, b.CreateFMul(rg, cgv));
            }
            llvm::Value* cur = fragGet(b, out, e);
            out = fragSet(b, out, b.CreateFAdd(cur, term), e);
        }
        return out;
    }

    // iacc[e] += colS * acc[e]. The accumulator-to-accumulator part needs no
    // layout — element e of one is element e of the other — but `colS` is a
    // per-lane COLUMN factor under the same contract as scaledAccumIntoS, so
    // it needs the layout for exactly the same reason.
    //
    // CORRECTION, 2026-09-20. This comment used to read "unlike the float
    // epilogue above this needs NO layout at all", and that was wrong in the
    // half that mattered. It described the accumulator mapping and then drew
    // a conclusion about the scale, which is a different value with a
    // different rule. The five `theQ*Mw8DeqKernelAgreesWith...` failures were
    // this verb, not the float one.
    //
    // NVIDIA has no 24-bit multiply intrinsic to reach for the way AMD does
    // with llvm.amdgcn.mul.i24, and a plain i32 multiply is already full rate
    // here, so the product itself is CreateMul.
    llvm::Value* coopMatrixScaledAccumI32(
            llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* accVal,
            llvm::Value* iaccVal, llvm::Value* colS,
            llvm::Value* colSPtr = nullptr, llvm::Type* colSETy = nullptr,
            llvm::Value* colSStride = nullptr) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        const unsigned n = fragCount(iaccVal->getType());
        if (n != 8)
            throw cajeta::Exception(
                std::string("XPU NVPTX cooperative matrix: "
                "CooperativeMatrix.scaledAccumI32 takes a per-lane column "
                "factor, so it needs the element-to-(row,column) mapping, and "
                "that is established here only for the 8-element m16n16k16 "
                "accumulator. This one has ") + std::to_string(n) +
                " elements. Add the shape to NvptxCoopEpilogueTests and read "
                "the mapping off the failure, or force the portable tier with "
                "CAJETA_GPU_COOPMATRIX_IMPL=software.", "XPU-N04");

        llvm::Value* lane = waveLaneId(b, m);
        llvm::Value* col2 = b.CreateShl(
            b.CreateAnd(lane, llvm::ConstantInt::get(i32, 3)),
            llvm::ConstantInt::get(i32, 1), "i32epi.col2");
        llvm::Value* out = iaccVal;
        for (unsigned e = 0; e < n; ++e) {
            const unsigned h = e >> 2;
            const unsigned j = e & 3;
            llvm::Value* col = b.CreateAdd(
                col2, llvm::ConstantInt::get(i32, 8u * h + (j & 1u)),
                "i32epi.col");
            // colSPtr: read column c's int factor from the Shared panel at
            // colSPtr[c*stride] (WaveVector.ofSlice); else recover the per-lane
            // scalar for column c by shuffle.
            llvm::Value* cs;
            if (colSPtr) {
                llvm::Value* idx = col;
                if (colSStride) {
                    bool one = false;
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(colSStride))
                        one = ci->isOne();
                    if (!one) idx = b.CreateMul(col, colSStride, "i32epi.cstride");
                }
                cs = b.CreateLoad(
                    colSETy, b.CreateGEP(colSETy, colSPtr, idx, "i32epi.cs.ptr"),
                    "i32epi.cs");
            } else {
                cs = scalarColFactor(b, m, colS, col);
            }
            llvm::Value* av = fragGet(b, accVal, e);
            llvm::Value* cur = fragGet(b, out, e);
            out = fragSet(b, out, b.CreateAdd(cur, b.CreateMul(av, cs)), e);
        }
        return out;
    }

    // tex.sample → llvm.nvvm.tex.unified.2d.v4f32.f32; the i64 handle bundles
    // image AND sampler state, so the Sampler argument is unused here.
    llvm::Value* sampleTexture(llvm::IRBuilderBase& b, llvm::Module& m,
                               llvm::Value* texHandle,
                               llvm::Value* /*samplerHandle*/, llvm::Value* u,
                               llvm::Value* v, llvm::Value* /*lod*/) override {
        llvm::Function* tex = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_tex_unified_2d_v4f32_f32);
        llvm::Value* rgba = b.CreateCall(tex, {texHandle, u, v}, "tex.rgba");
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        llvm::Value* vec = llvm::PoisonValue::get(v4f);
        for (unsigned i = 0; i < 4; ++i)
            vec = b.CreateInsertElement(
                vec, b.CreateExtractValue(rgba, {i}), uint64_t(i),
                i == 3 ? "tex.sample" : "");
        return vec;
    }

    // Image2D → llvm.nvvm.sust.b.2d / suld.2d: surface coords are BYTE offsets
    // in x (x*4 for an R32 texel, y stays a row), the texel raw i32 bits.
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

    // Thread.clock(): the 64-bit SM clock, clock64.
    llvm::Value* readClock(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::nvvm_read_ptx_sreg_clock64);
        return b.CreateCall(f, {}, "clock");
    }

    // Transcendentals as libdevice calls, `__nv_<fn>f` / `__nv_<fn>`, linked at
    // cubin time; NVPTX has no IEEE transcendental instructions. rsqrt is native.
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

    // -- cooperative-matrix (wmma) helpers ------------------------------------

    // Push every element of an aggregate (the wmma fragment struct) onto `out`.
    static void appendStructElems(llvm::IRBuilderBase& b, llvm::Value* agg,
                                  std::vector<llvm::Value*>& out) {
        unsigned n =
            llvm::cast<llvm::StructType>(agg->getType())->getNumElements();
        for (unsigned e = 0; e < n; ++e)
            out.push_back(b.CreateExtractValue(agg, e));
    }

    static llvm::Value* coerceScalar(llvm::IRBuilderBase& b, llvm::Value* v,
                                     llvm::Type* to) {
        if (v->getType() == to) return v;
        if (v->getType()->isFloatingPointTy() && to->isFloatingPointTy())
            return b.CreateFPCast(v, to);
        if (v->getType()->isIntegerTy() && to->isIntegerTy())
            return b.CreateIntCast(v, to, /*isSigned=*/true);
        return v;
    }

    // An intrinsic declaration, with the pointer-overload type for intrinsics
    // overloaded on their address space (the wmma load/store family).
    static llvm::Function* nvDecl(llvm::Module& m, llvm::Intrinsic::ID id,
                                  llvm::Type* ptrTy = nullptr) {
        if (llvm::Intrinsic::isOverloaded(id)) {
            llvm::Type* pt = ptrTy
                ? ptrTy
                : (llvm::Type*) llvm::PointerType::get(m.getContext(), 1);
            return llvm::Intrinsic::getOrInsertDeclaration(&m, id, {pt});
        }
        return llvm::Intrinsic::getOrInsertDeclaration(&m, id);
    }

    // The wmma.load intrinsic for (use, element type, layout). Layout 1 is
    // col-major: NVIDIA WMMA takes it natively and the fork ships every
    // `_col_stride` form, so this is a selection and not a capability.
    static llvm::Intrinsic::ID nvWmmaLoadId(uint32_t use, llvm::Type* elem,
                                            uint32_t layout = 0) {
        bool bf = elem->isBFloatTy();
        bool col = layout == 1;
        // 8-bit operands and their 32-bit accumulator. The LOAD does not care
        // about signedness — s8 and u8 fragments are the same {i32 x 2} register
        // image and the load only moves bytes — so `s8` is used for both and the
        // interpretation is chosen at the mma, which is where it means anything.
        if (elem->isIntegerTy(8)) {
            if (use == 0)
                return col ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_s8_col_stride
                           : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_s8_row_stride;
            return col ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_s8_col_stride
                       : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_s8_row_stride;
        }
        if (use == 2 && elem->isIntegerTy(32))
            return col ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_c_s32_col_stride
                       : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_c_s32_row_stride;
        if (use == 0) {
            if (col)
                return bf ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_bf16_col_stride
                          : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_f16_col_stride;
            return bf ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_bf16_row_stride
                      : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_f16_row_stride;
        }
        if (use == 1) {
            if (col)
                return bf ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_bf16_col_stride
                          : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_f16_col_stride;
            return bf ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_bf16_row_stride
                      : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_f16_row_stride;
        }
        return col ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_c_f32_col_stride
                   : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_c_f32_row_stride;
    }
    static llvm::Function* nvWmmaLoadDecl(llvm::Module& m, uint32_t use,
                                          llvm::Type* elem,
                                          llvm::Type* ptrTy = nullptr,
                                          uint32_t layout = 0) {
        return nvDecl(m, nvWmmaLoadId(use, elem, layout), ptrTy);
    }

    // The fragment's scalar element, recovered from the fragment's SHAPE.
    //
    // Three of the five register images are `{i32 x n}` and a scalar probe
    // cannot tell them apart, so the COUNT carries the answer. Verified
    // against the IR verifier, 2026-09-19:
    //
    //     f16  A/B   {<2 x half> x 8}   vector members
    //     bf16 A/B   {i32 x 4}
    //     s8   A/B   {i32 x 2}
    //     s32  C/D   {i32 x 8}
    //     f32  C/D   {float x 8}        float members
    //
    // Adding a shape that collides with one of these must extend this, not
    // reuse it — reading an s8 fragment as bf16 lowers cleanly and computes
    // nonsense.
    static llvm::Type* nvFragScalar(llvm::Type* matrixType) {
        auto* st = llvm::cast<llvm::StructType>(matrixType);
        llvm::Type* e0 = st->getElementType(0);
        if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(e0))
            return vt->getElementType();                      // f16
        if (e0->isIntegerTy(32)) {
            llvm::LLVMContext& ctx = matrixType->getContext();
            switch (st->getNumElements()) {
                case 2: return llvm::Type::getInt8Ty(ctx);    // s8/u8 A/B
                case 8: return llvm::Type::getInt32Ty(ctx);   // s32 C/D
                default: return llvm::Type::getBFloatTy(ctx); // bf16 A/B ({i32 x 4})
            }
        }
        return e0;                                            // f32
    }

    // Row-major (0) and col-major (1) are both native. A NON-CONSTANT layout
    // still refuses: wmma encodes the layout in the instruction, so it has to
    // be known when the instruction is selected. Measured 2026-09-19 across
    // cajeta-llm: 739 of 1121 coop sites pass layout 1 and none passes a
    // non-constant, so this arm is the one that mattered.
    uint32_t constLayout(llvm::Value* layout, const char* op) {
        auto* ci = llvm::dyn_cast<llvm::ConstantInt>(layout);
        if (!ci)
            throw cajeta::Exception(
                std::string("XPU NVPTX cooperative matrix: CooperativeMatrix.") +
                op + " got a NON-CONSTANT layout. wmma encodes the operand "
                "layout in the instruction, so it must be known at lowering. "
                "Pass a constant 0 (row-major) or 1 (col-major), or force the "
                "portable tier with CAJETA_GPU_COOPMATRIX_IMPL=software.",
                "XPU-N04");
        uint64_t v = ci->getZExtValue();
        if (v > 1)
            throw cajeta::Exception(
                std::string("XPU NVPTX cooperative matrix: CooperativeMatrix.") +
                op + " got layout " + std::to_string(v) +
                "; only 0 (row-major) and 1 (col-major) exist.", "XPU-N04");
        return (uint32_t) v;
    }
};

} // namespace

// The PTX ABI's cap on STATIC `.shared`. The number itself lives in
// cajeta_xpu_abi.h, because the compiler decides the relocation and the C
// runtime performs the matching cuFuncSetAttribute opt-in, and a cap the two
// sides disagreed about would produce a kernel that assembles and will not
// launch.
static constexpr uint64_t kNvptxStaticSharedCap =
    CAJETA_XPU_CUDA_STATIC_SHARED_CAP;

// The one extern block an over-cap kernel's tiles are packed into. PTX gives
// a module a single dynamic shared region; every relocated tile is an offset
// inside it.
static const char* kNvptxDynSharedSym = "__cajeta_nvptx_dynshared";

uint64_t relocateOversizedStaticShared(llvm::Function* kfn,
                                       llvm::Module& m) {
    if (!kfn) return 0;
    const llvm::DataLayout& dl = m.getDataLayout();
    llvm::LLVMContext& ctx = m.getContext();

    llvm::SmallVector<llvm::GlobalVariable*, 8> statics;
    bool hasExtern = false;
    uint64_t total = 0;
    for (llvm::GlobalVariable& g : m.globals()) {
        if (g.getAddressSpace() != 3) continue;
        if (!g.hasInitializer()) { hasExtern = true; continue; }
        statics.push_back(&g);
    }
    // Align each tile the way it asked to be aligned; the block itself is
    // 16-aligned, so honouring the members keeps every one of them aligned.
    for (llvm::GlobalVariable* g : statics) {
        uint64_t a = g->getAlign() ? g->getAlign()->value() : 16;
        if (a < 1) a = 1;
        total = (total + a - 1) / a * a;
        total += dl.getTypeAllocSize(g->getValueType());
    }
    if (total <= kNvptxStaticSharedCap) return 0;

    // A kernel that ALREADY has a dynamic block cannot also have its static
    // tiles relocated: both would start at offset 0 of the same region and
    // silently alias. Refuse by name rather than miscompile.
    if (hasExtern)
        throw cajeta::Exception(
            std::string("XPU NVPTX shared memory: kernel '")
            + kfn->getName().str() + "' declares " + std::to_string(total)
            + " bytes of static Shared<T>, over the " +
            std::to_string(kNvptxStaticSharedCap) + "-byte PTX static cap, "
            "AND a runtime-sized Shared<T>. Both would have to live at the "
            "start of the one dynamic shared block. Give the runtime-sized "
            "tile a static size, or bring the static tiles under the cap.",
            "XPU-N05");

    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* blockTy = llvm::ArrayType::get(i8, 0);
    auto* block = llvm::cast<llvm::GlobalVariable>(
        m.getOrInsertGlobal(kNvptxDynSharedSym, blockTy, [&] {
            return new llvm::GlobalVariable(
                m, blockTy, /*isConstant=*/false,
                llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr,
                kNvptxDynSharedSym, /*InsertBefore=*/nullptr,
                llvm::GlobalValue::NotThreadLocal, /*AddressSpace=*/3);
        }));
    block->setAlignment(llvm::Align(16));

    auto* i64 = llvm::Type::getInt64Ty(ctx);
    uint64_t at = 0;
    for (llvm::GlobalVariable* g : statics) {
        uint64_t a = g->getAlign() ? g->getAlign()->value() : 16;
        if (a < 1) a = 1;
        at = (at + a - 1) / a * a;
        llvm::Constant* idx[] = {llvm::ConstantInt::get(i64, 0),
                                 llvm::ConstantInt::get(i64, at)};
        llvm::Constant* slot = llvm::ConstantExpr::getInBoundsGetElementPtr(
            blockTy, block, idx);
        g->replaceAllUsesWith(slot);
        at += dl.getTypeAllocSize(g->getValueType());
    }
    for (llvm::GlobalVariable* g : statics) g->eraseFromParent();
    return total;
}

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule,
                            uint64_t* dynSharedBytes) {
    NvptxTarget target;
    llvm::Function* f =
        cajeta::xpu::lowerKernel(method, deviceModule, target);
    uint64_t n = relocateOversizedStaticShared(f, deviceModule);
    if (dynSharedBytes) *dynSharedBytes = n;
    return f;
}

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    return lowerKernel(method, deviceModule, /*dynSharedBytes=*/nullptr);
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
