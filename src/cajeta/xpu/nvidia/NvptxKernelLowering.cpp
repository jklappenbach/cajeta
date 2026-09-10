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

    // ---- Cooperative matrix: NVIDIA tensor cores (wmma) ----------------------
    // m16n16k16 D[f32] = A[f16/bf16]·B[f16/bf16] + C[f32], row-major, warp-collective.
    // The fragment↔lane layout is implementation-defined: load/store MUST use NVVM wmma.

    ImplTier coopMatrixTier(llvm::Type* elem, uint32_t rows, uint32_t cols,
                            uint32_t use) override {
        if (rows == 16 && cols == 16) {
            if (use == 2) {                              // accumulator (v1: f32)
                if (elem->isFloatTy()) return ImplTier::Native;
            } else if (elem->isHalfTy() || elem->isBFloatTy()) {  // A/B operand
                return ImplTier::Native;
            }
        }
        return ImplTier::Portable;     // int8/u8, f64, other shapes → portable tile
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
        requireRowMajor(layout, "load");
        llvm::Function* f =
            nvWmmaLoadDecl(m, use, nvFragScalar(matrixType), ptr->getType());
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
        requireRowMajor(layout, "store");
        llvm::Function* f = nvDecl(
            m, llvm::Intrinsic::nvvm_wmma_m16n16k16_store_d_f32_row_stride,
            ptr->getType());
        std::vector<llvm::Value*> args;
        args.push_back(ptr);
        appendStructElems(b, matrixVal, args);
        args.push_back(stride);
        b.CreateCall(f, args);
    }

    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m,
                                  llvm::Value* a, llvm::Value* bMat,
                                  llvm::Value* c, llvm::Type* /*matrixType*/,
                                  uint32_t /*signFlags*/) override {
        // Pick mma by the A fragment's SHAPE, never a scalar probe: f16 A/B is
        // {<2 x half> x 8} and bf16 is {i32 x 4}, so casting element 0 to
        // FixedVectorType asserts inside LLVM, uncatchably.
        auto* aFrag = llvm::cast<llvm::StructType>(a->getType());
        bool bf = !llvm::isa<llvm::FixedVectorType>(aFrag->getElementType(0));
        llvm::Intrinsic::ID id = bf
            ? llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_row_bf16
            : llvm::Intrinsic::nvvm_wmma_m16n16k16_mma_row_row_f32_f32;
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

    // The row-major-stride wmma.load intrinsic for (use, element type).
    static llvm::Intrinsic::ID nvWmmaLoadId(uint32_t use, llvm::Type* elem) {
        bool bf = elem->isBFloatTy();
        if (use == 0)
            return bf ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_bf16_row_stride
                      : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_a_f16_row_stride;
        if (use == 1)
            return bf ? llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_bf16_row_stride
                      : llvm::Intrinsic::nvvm_wmma_m16n16k16_load_b_f16_row_stride;
        return llvm::Intrinsic::nvvm_wmma_m16n16k16_load_c_f32_row_stride;  // use 2
    }
    static llvm::Function* nvWmmaLoadDecl(llvm::Module& m, uint32_t use,
                                          llvm::Type* elem,
                                          llvm::Type* ptrTy = nullptr) {
        return nvDecl(m, nvWmmaLoadId(use, elem), ptrTy);
    }

    // The fragment's scalar element: the vector element for f16 A/B, bfloat for
    // bf16 A/B ({i32 x 4} is a register image), the struct element for f32.
    static llvm::Type* nvFragScalar(llvm::Type* matrixType) {
        llvm::Type* e0 =
            llvm::cast<llvm::StructType>(matrixType)->getElementType(0);
        if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(e0))
            return vt->getElementType();
        if (e0->isIntegerTy(32))
            return llvm::Type::getBFloatTy(matrixType->getContext());
        return e0;
    }

    void requireRowMajor(llvm::Value* layout, const char* op) {
        auto* ci = llvm::dyn_cast<llvm::ConstantInt>(layout);
        if (!ci || !ci->isZero())
            throw cajeta::Exception(
                std::string("XPU NVPTX native cooperative matrix (v1) supports "
                "only row-major operands (layout 0); CooperativeMatrix.") + op +
                " got a non-row-major or non-constant layout. Use row-major tiles, "
                "or force the portable tier with CAJETA_GPU_COOPMATRIX_IMPL="
                "software.", "XPU-N04");
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
