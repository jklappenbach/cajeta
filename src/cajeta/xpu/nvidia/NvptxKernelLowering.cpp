// NVPTX kernel lowering — see header. The AST walk is the shared
// xpu/lowering/KernelLowering.cpp; this file is the NVPTX LoweringTarget.

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

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule) {
    NvptxTarget target;
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
