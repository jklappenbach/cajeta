//
// CPU kernel lowering — see header. The host LoweringTarget + thin wrapper.
//

#include "CpuKernelLowering.h"
#include "../core/XpuKernelAttr.h"
#include "cajeta/xpu/core/DeclarationCheck.h"
#include "../../method/Method.h"

#include "../lowering/KernelLowering.h"
#include "../lowering/LoweringTarget.h"
#include "../../error/Exception.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ModRef.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/KnownBits.h"
#include <algorithm>
#include "CpuBackend.h"
#include "../../type/VectorOps.h"
#include <cstdlib>
#include <memory>

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

// The CPU LoweringTarget. The host has no coordinate intrinsics, so a work-item's
// coordinates arrive as the 12 trailing i32 kernel args the coord reads pull back.
class CpuTarget : public LoweringTarget {
public:
    explicit CpuTarget(std::shared_ptr<llvm::TargetMachine> tm = nullptr) : levelTm(std::move(tm)) {}
    const char* name() const override { return "cpu"; }
    bool honorsStatedAlignment() const override { return false; }
    // The blocks run on host workers, so the record is the runtime's: the
    // launcher reads it when the grid is done (kernel-load-alignment 5.1.1).
    void recordAlignFault(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* info,
                          llvm::Value* index) override {
        auto* i64 = llvm::Type::getInt64Ty(m.getContext());
        llvm::FunctionCallee hook = m.getOrInsertFunction(
            "__cajeta_xpu_cpu_align_fault",
            llvm::FunctionType::get(llvm::Type::getVoidTy(m.getContext()), {i64, i64}, false));
        b.CreateCall(hook, {info, index});
    }

    // A CPU cooperative wave is a SIMD vector of i32 lanes: the per-block
    // work-item loop, vectorized to waveW lanes. The cooperative-matrix kernels
    // are written against the universal WMMA cooperative wave of 32 (the CUDA
    // warp and the RDNA3 wave32 AMD pins), staging their tiles as `lane = tid %
    // 32`, `wid = tid / 32`. Emulating that same 32-lane wave on cpu — one
    // 32-wide SIMD vector per wave, forced through LoopVectorize exactly as the
    // loop-exposure spike forced 16 — lets that one source run here unchanged, no
    // per-device fork, and distributes the 16x16 tile over 32 lanes (8 elements
    // each, G = 2) rather than 16, halving the per-lane software-tile scratch.
    // Not derived from `cols`: it is the cooperative-group width the algorithm
    // and the other backends share, so a kernel's `% 32` staging is correct on
    // every backend. decideCoopDistribution still declines shapes 32 does not
    // divide. The block must be a multiple of 32 (the shipped kernels use 256).
    unsigned distributedCoopMatrixWaveWidth(uint32_t /*cols*/) override {
        return 32;
    }
    void prepareDistributedCoopMatrix(llvm::Function* fn,
                                      unsigned waveW) override {
        // The per-kernel wave width the work-item LoopVectorize must be forced
        // to; cpuVectorWidthI32 reads it off the kernel at registration time.
        fn->addFnAttr("cajeta.xpu.coop-wavew", std::to_string(waveW));
    }
    // @Wave(width = N) takes the same pin: the work-item loop runs at N lanes whatever
    // the host SIMD width, so a kernel written for 32 lanes is correct on every host.
    bool pinWaveWidth(llvm::Function* fn, unsigned width) override {
        if (width < 2) return width == 1;
        // CAJETA_XPU_FAULT=drop-wave-pin accepts the declaration and pins
        // nothing, for the declaration check's test that it fires.
        if (!xpuFault("drop-wave-pin"))
            fn->addFnAttr("cajeta.xpu.coop-wavew", std::to_string(width));
        return true;
    }
    // The inner loops must be gone for LoopVectorize to widen the work-item
    // loop (4A.7.2's loop-exposure spike).
    bool distributedCoopMatrixUnrollsLoops() const override { return true; }

    // Wide `dotAccum` for the host ISA, so a @Kernel reaches the tier an ordinary
    // method does. Null (leaving the portable reduce) off x86, without VNNI, or on
    // any shape but 4·n int8 lanes into n i32 accumulators.
    llvm::Value* integerDotWide(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* w, llvm::Value* a,
                                llvm::Value* acc, bool wUnsigned) override {
        const char* fsd = std::getenv("CAJETA_SIMD_SCALAR_FALLBACK");
        if (fsd && fsd[0] && fsd[0] != '0') return nullptr;
        const llvm::TargetMachine* tm = tmHere();
        if (!tm || !tm->getTargetTriple().isX86()) return nullptr;
        const llvm::MCSubtargetInfo& sti = tm->getMCSubtargetInfo();
        const bool vnni = sti.checkFeatures("+avx512vnni") || sti.checkFeatures("+avxvnni");
        const bool avx2 = sti.checkFeatures("+avx2");
        if (!vnni && !avx2) return nullptr;

        auto* wt = llvm::dyn_cast<llvm::FixedVectorType>(w->getType());
        auto* at = llvm::dyn_cast<llvm::FixedVectorType>(acc->getType());
        if (!wUnsigned || wt == nullptr || at == nullptr) return nullptr;
        unsigned n = at->getNumElements();
        if (wt->getNumElements() != n * 4) return nullptr;
        if (n != 4 && n != 8 && n != 16) return nullptr;

        // Without VNNI the AVX2 tiers (cpu-isa-variants 3.1, 3.2): the saturating
        // 3-instruction form only where the weights provably hold 0..128, so this
        // backend stays the GPU backends' bit-exact oracle; the exact form otherwise.
        vecops::DotAccumTargets t;
        t.vnni = vnni;
        t.avx2 = !vnni && avx2;
        t.forceScalar = false;
        if (t.avx2) t.saturatingOk = byteBits(w, m.getDataLayout(), 0).Zero[7];
        return vecops::dotAccum(b, &m, w, a, acc, wUnsigned, t);
    }

    // What is known of EVERY BYTE of `v`: a Vector of bytes is lowered as words
    // with bitcasts in between, and its subexpressions live in slots at lowering
    // time, so this follows bitcasts and single-store slots (computeKnownBits does
    // neither), with and/or/shifts/zext by hand and computeKnownBits as the floor.
    static llvm::KnownBits byteBits(llvm::Value* v, const llvm::DataLayout& dl, int depth) {
        llvm::KnownBits none(8);
        v = throughSlot(v);
        llvm::Type* ty = v->getType();
        const unsigned width = ty->getScalarSizeInBits();
        if (depth > 16 || width == 0 || (width % 8) != 0 || !ty->getScalarType()->isIntegerTy()) return none;
        auto fromLane = [&](const llvm::KnownBits& kb) {
            llvm::KnownBits r(8);
            r.Zero.setAllBits(); r.One.setAllBits();
            for (unsigned i = 0; i < kb.getBitWidth(); i += 8) {
                r.Zero &= kb.Zero.extractBits(8, i);
                r.One &= kb.One.extractBits(8, i);
            }
            return r;
        };
        auto shiftAmount = [&](llvm::Value* s, uint64_t& out) {
            auto* c = llvm::dyn_cast<llvm::Constant>(s);
            llvm::Constant* sp = c ? (c->getType()->isVectorTy() ? c->getSplatValue() : c) : nullptr;
            if (auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(sp)) { out = ci->getZExtValue(); return true; }
            return false;
        };
        if (auto* bc = llvm::dyn_cast<llvm::BitCastOperator>(v)) {
            llvm::Type* st = bc->getSrcTy()->getScalarType();
            if (st->isIntegerTy() && (st->getIntegerBitWidth() % 8) == 0) return byteBits(bc->getOperand(0), dl, depth + 1);
            return none;
        }
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
            uint64_t sh = 0;
            switch (bo->getOpcode()) {
            case llvm::Instruction::And:
                return byteBits(bo->getOperand(0), dl, depth + 1) & byteBits(bo->getOperand(1), dl, depth + 1);
            case llvm::Instruction::Or:
                return byteBits(bo->getOperand(0), dl, depth + 1) | byteBits(bo->getOperand(1), dl, depth + 1);
            case llvm::Instruction::Shl: {
                if (!shiftAmount(bo->getOperand(1), sh) || sh >= 8) return none;
                llvm::KnownBits k = byteBits(bo->getOperand(0), dl, depth + 1);
                k.Zero = k.Zero.shl((unsigned) sh); k.One = k.One.shl((unsigned) sh);
                // In a wider lane the low bits arrive from the byte below: unknown.
                if (width != 8) { k.Zero.clearLowBits((unsigned) sh); k.One.clearLowBits((unsigned) sh); }
                return k;
            }
            case llvm::Instruction::LShr: {
                if (!shiftAmount(bo->getOperand(1), sh) || sh >= 8) return none;
                llvm::KnownBits k = byteBits(bo->getOperand(0), dl, depth + 1);
                k.Zero = k.Zero.lshr((unsigned) sh); k.One = k.One.lshr((unsigned) sh);
                if (width != 8) {
                    llvm::APInt hi = llvm::APInt::getHighBitsSet(8, (unsigned) sh);
                    k.Zero &= ~hi; k.One &= ~hi;
                }
                return k;
            }
            default: break;
            }
        }
        if (auto* z = llvm::dyn_cast<llvm::ZExtInst>(v)) {
            llvm::KnownBits k = byteBits(z->getOperand(0), dl, depth + 1);
            k.One.clearAllBits();   // the new bytes are zero: their known ones are none
            return k;
        }
        return fromLane(llvm::computeKnownBits(v, dl));
    }

    // The value a load-of-single-store slot holds, through up to six hops: at
    // lowering time a Vector lives in its alloca, and known bits stop at a load.
    static llvm::Value* throughSlot(llvm::Value* v) {
        for (int hop = 0; hop < 6; ++hop) {
            auto* ld = llvm::dyn_cast<llvm::LoadInst>(v);
            if (!ld) break;
            auto* al = llvm::dyn_cast<llvm::AllocaInst>(ld->getPointerOperand());
            if (!al) break;
            llvm::StoreInst* only = nullptr;
            bool multi = false;
            for (llvm::User* u : al->users())
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                    if (only) multi = true;
                    only = st;
                }
            if (!only || multi || only->getParent() != ld->getParent() || !only->comesBefore(ld)) break;
            v = only->getValueOperand();
        }
        return v;
    }

    // The TargetMachine this copy is built for: its level's, or the build host's.
    const llvm::TargetMachine* tmHere() const {
        static const std::unique_ptr<llvm::TargetMachine> hostTm = createCpuTargetMachine();
        return levelTm ? levelTm.get() : hostTm.get();
    }

    // --- AMX (cpu-isa-variants Unit 6) --------------------------------------
    // One int8 16x16 tile a work item on the matrix unit, held off behind
    // CAJETA_XPU_CPU_AMX=1 until measured on hardware (spec 5.4). Tiles in memory
    // are <256 x i32>; A is 16 rows x 16 bytes, B the VNNI 4 x 64, C 16 x 64.
    bool amxHere() const {
        const char* on = std::getenv("CAJETA_XPU_CPU_AMX");
        if (!on || !*on || *on == '0') return false;
        const llvm::TargetMachine* tm = tmHere();
        if (!tm || !tm->getTargetTriple().isX86()) return false;
        const llvm::MCSubtargetInfo& sti = tm->getMCSubtargetInfo();
        return sti.checkFeatures("+amx-tile") && sti.checkFeatures("+amx-int8");
    }

    ImplTier coopMatrixTier(llvm::Type* elem, uint32_t rows, uint32_t cols,
                            uint32_t use) override {
        if (!amxHere() || rows != 16 || cols != 16) return ImplTier::Portable;
        if (use == 2) return elem->isIntegerTy(32) ? ImplTier::Native : ImplTier::Portable;
        return elem->isIntegerTy(8) ? ImplTier::Native : ImplTier::Portable;
    }

    llvm::Type* coopMatrixType(llvm::Module& m, llvm::Type*, uint32_t, uint32_t, uint32_t) override {
        return llvm::FixedVectorType::get(llvm::Type::getInt32Ty(m.getContext()), 256);
    }

    // The tile is opaque to per-element access and the fused epilogues: a kernel
    // using them demotes to the portable tile, as on SPIR-V.
    unsigned coopMatrixElementCount(llvm::Type*) override { return 0; }

    static llvm::Value* amxI16(llvm::Module& m, unsigned v) {
        return llvm::ConstantInt::get(llvm::Type::getInt16Ty(m.getContext()), v);
    }
    static llvm::Type* amxTileTy(llvm::Module& m) { return llvm::Type::getX86_AMXTy(m.getContext()); }
    static llvm::Value* amxToTile(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* vec) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::x86_cast_vector_to_tile, {vec->getType()});
        return b.CreateCall(f, {vec}, "amx.tile");
    }
    static llvm::Value* amxToVector(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* tile) {
        llvm::Type* vt = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(m.getContext()), 256);
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(
            &m, llvm::Intrinsic::x86_cast_tile_to_vector, {vt});
        return b.CreateCall(f, {tile}, "amx.vec");
    }
    // A 64-byte-aligned scratch in the entry block, `bytes` long.
    static llvm::Value* amxScratch(llvm::IRBuilderBase& b, llvm::Module& m, unsigned bytes, const char* nm) {
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        llvm::IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        auto* al = eb.CreateAlloca(llvm::ArrayType::get(llvm::Type::getInt8Ty(m.getContext()), bytes), nullptr, nm);
        al->setAlignment(llvm::Align(64));
        return al;
    }
    static llvm::Value* amxTileLoad(llvm::IRBuilderBase& b, llvm::Module& m, unsigned rows, unsigned colsb,
                                    llvm::Value* ptr, llvm::Value* strideBytes) {
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, llvm::Intrinsic::x86_tileloadd64_internal);
        return b.CreateCall(f, {amxI16(m, rows), amxI16(m, colsb), ptr, strideBytes}, "amx.load");
    }
    static uint32_t amxConstLayout(llvm::Value* layout, const char* what) {
        auto* c = llvm::dyn_cast<llvm::ConstantInt>(layout);
        if (!c) throw cajeta::Exception(std::string("AMX ") + what + " needs a constant layout", "XPU-N03");
        return (uint32_t) c->getZExtValue();
    }

    llvm::Value* coopMatrixLoad(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
                                llvm::Value* layout, llvm::Value* stride, llvm::Type*,
                                uint32_t, uint32_t, uint32_t use, uint32_t swz = 0,
                                LdsBlockPad blk = {}) override {
        if (swz || blk.period)
            throw cajeta::Exception("AMX tile load takes no swizzle or block pad", "XPU-N03");
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        const uint32_t lay = amxConstLayout(layout, "tile load");
        llvm::Value* s64 = b.CreateZExtOrTrunc(stride, i64, "amx.stride");
        auto rowPtr = [&](llvm::Value* base, llvm::Value* row, unsigned elemBytes) {
            llvm::Value* off = b.CreateMul(row, llvm::ConstantInt::get(i64, elemBytes));
            return b.CreateGEP(i8, base, b.CreateMul(off, s64));
        };
        auto at = [&](llvm::Value* base, unsigned bytes) { return b.CreateGEP(i8, base, llvm::ConstantInt::get(i64, bytes)); };
        llvm::Value* tile = nullptr;
        if (use == 0) {
            if (lay == 0) {
                tile = amxTileLoad(b, m, 16, 16, ptr, s64);
            } else {
                // Column-major A: column k holds the 16 rows; transpose 16x16 bytes.
                llvm::Value* sc = amxScratch(b, m, 256, "amx.a");
                llvm::Value* cols[16];
                for (unsigned k = 0; k < 16; ++k)
                    cols[k] = b.CreateAlignedLoad(llvm::FixedVectorType::get(i8, 16),
                                                  rowPtr(ptr, llvm::ConstantInt::get(i64, k), 1), llvm::Align(1));
                for (unsigned r = 0; r < 16; ++r) {
                    llvm::Value* row = llvm::UndefValue::get(llvm::FixedVectorType::get(i8, 16));
                    for (unsigned k = 0; k < 16; ++k)
                        row = b.CreateInsertElement(row, b.CreateExtractElement(cols[k], r), k);
                    b.CreateAlignedStore(row, at(sc, r * 16), llvm::Align(16));
                }
                tile = amxTileLoad(b, m, 16, 16, sc, llvm::ConstantInt::get(i64, 16));
            }
        } else if (use == 1) {
            // B in the VNNI form: row k4 holds, per column n, the bytes B[4k4..4k4+3][n].
            llvm::Value* sc = amxScratch(b, m, 256, "amx.b");
            if (lay == 0) {
                for (unsigned k4 = 0; k4 < 4; ++k4) {
                    llvm::Value* r[4];
                    for (unsigned j = 0; j < 4; ++j)
                        r[j] = b.CreateAlignedLoad(llvm::FixedVectorType::get(i8, 16),
                                                   rowPtr(ptr, llvm::ConstantInt::get(i64, 4 * k4 + j), 1), llvm::Align(1));
                    llvm::SmallVector<int, 32> cat;
                    for (int i = 0; i < 32; ++i) cat.push_back(i);
                    llvm::Value* lo = b.CreateShuffleVector(r[0], r[1], cat);
                    llvm::Value* hi = b.CreateShuffleVector(r[2], r[3], cat);
                    llvm::SmallVector<int, 64> il;
                    for (int n = 0; n < 16; ++n)
                        for (int j = 0; j < 4; ++j) il.push_back(j * 16 + n);
                    llvm::Value* v = b.CreateShuffleVector(lo, hi, il, "amx.vnni");
                    b.CreateAlignedStore(v, at(sc, k4 * 64), llvm::Align(64));
                }
            } else {
                // Column-major B: column n's 16 K-bytes are 4 VNNI dwords, one per row k4.
                llvm::Value* cols[16];
                for (unsigned n = 0; n < 16; ++n)
                    cols[n] = b.CreateAlignedLoad(llvm::FixedVectorType::get(i32, 4),
                                                  rowPtr(ptr, llvm::ConstantInt::get(i64, n), 1), llvm::Align(1));
                for (unsigned k4 = 0; k4 < 4; ++k4) {
                    llvm::Value* row = llvm::UndefValue::get(llvm::FixedVectorType::get(i32, 16));
                    for (unsigned n = 0; n < 16; ++n)
                        row = b.CreateInsertElement(row, b.CreateExtractElement(cols[n], k4), n);
                    b.CreateAlignedStore(row, at(sc, k4 * 64), llvm::Align(64));
                }
            }
            tile = amxTileLoad(b, m, 4, 64, sc, llvm::ConstantInt::get(i64, 64));
        } else {
            if (lay == 0) {
                tile = amxTileLoad(b, m, 16, 64, ptr, b.CreateMul(s64, llvm::ConstantInt::get(i64, 4)));
            } else {
                llvm::Value* sc = amxScratch(b, m, 1024, "amx.c");
                llvm::Value* cols[16];
                for (unsigned c = 0; c < 16; ++c)
                    cols[c] = b.CreateAlignedLoad(llvm::FixedVectorType::get(i32, 16),
                                                  rowPtr(ptr, llvm::ConstantInt::get(i64, c), 4), llvm::Align(4));
                for (unsigned r = 0; r < 16; ++r) {
                    llvm::Value* row = llvm::UndefValue::get(llvm::FixedVectorType::get(i32, 16));
                    for (unsigned c = 0; c < 16; ++c)
                        row = b.CreateInsertElement(row, b.CreateExtractElement(cols[c], r), c);
                    b.CreateAlignedStore(row, at(sc, r * 64), llvm::Align(64));
                }
                tile = amxTileLoad(b, m, 16, 64, sc, llvm::ConstantInt::get(i64, 64));
            }
        }
        return amxToVector(b, m, tile);
    }

    void coopMatrixStore(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* ptr,
                         llvm::Value* matrixVal, llvm::Value* layout, llvm::Value* stride,
                         uint32_t, uint32_t, uint32_t use, uint32_t swz = 0,
                         LdsBlockPad blk = {}) override {
        if (swz || blk.period || use != 2)
            throw cajeta::Exception("AMX stores the accumulator tile only, without swizzle or block pad", "XPU-N03");
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i8 = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        const uint32_t lay = amxConstLayout(layout, "tile store");
        llvm::Value* s64 = b.CreateZExtOrTrunc(stride, i64, "amx.stride");
        llvm::Value* tile = amxToTile(b, m, matrixVal);
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, llvm::Intrinsic::x86_tilestored64_internal);
        if (lay == 0) {
            b.CreateCall(f, {amxI16(m, 16), amxI16(m, 64), ptr, b.CreateMul(s64, llvm::ConstantInt::get(i64, 4)), tile});
            return;
        }
        // Column-major: store row-major to scratch, then column c of the result is row c of a transpose.
        llvm::Value* sc = amxScratch(b, m, 1024, "amx.cs");
        b.CreateCall(f, {amxI16(m, 16), amxI16(m, 64), sc, llvm::ConstantInt::get(i64, 64), tile});
        llvm::Value* rows[16];
        for (unsigned r = 0; r < 16; ++r)
            rows[r] = b.CreateAlignedLoad(llvm::FixedVectorType::get(i32, 16),
                                          b.CreateGEP(i8, sc, llvm::ConstantInt::get(i64, r * 64)), llvm::Align(64));
        for (unsigned c = 0; c < 16; ++c) {
            llvm::Value* col = llvm::UndefValue::get(llvm::FixedVectorType::get(i32, 16));
            for (unsigned r = 0; r < 16; ++r)
                col = b.CreateInsertElement(col, b.CreateExtractElement(rows[r], c), r);
            llvm::Value* dst = b.CreateGEP(i8, ptr, b.CreateMul(llvm::ConstantInt::get(i64, c * 4), s64));
            b.CreateAlignedStore(col, dst, llvm::Align(4));
        }
    }

    llvm::Value* coopMatrixMulAdd(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* a,
                                  llvm::Value* bMat, llvm::Value* c, llvm::Type*,
                                  uint32_t signFlags, uint32_t = 0, uint32_t = 0) override {
        const bool aSigned = (signFlags & 0x1u) != 0, bSigned = (signFlags & 0x2u) != 0;
        llvm::Intrinsic::ID id = aSigned ? (bSigned ? llvm::Intrinsic::x86_tdpbssd_internal
                                                    : llvm::Intrinsic::x86_tdpbsud_internal)
                                         : (bSigned ? llvm::Intrinsic::x86_tdpbusd_internal
                                                    : llvm::Intrinsic::x86_tdpbuud_internal);
        llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, id);
        // M = 16 rows, N = 64 bytes of C a row, K = 16 bytes of A a row.
        llvm::Value* d = b.CreateCall(f, {amxI16(m, 16), amxI16(m, 64), amxI16(m, 16),
                                          amxToTile(b, m, c), amxToTile(b, m, a), amxToTile(b, m, bMat)}, "amx.dp");
        return amxToVector(b, m, d);
    }

    llvm::Value* coopMatrixSplat(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* value,
                                 llvm::Type* matrixType) override {
        auto* vt = llvm::cast<llvm::FixedVectorType>(matrixType);
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(value); ci && ci->isZero()) {
            llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(&m, llvm::Intrinsic::x86_tilezero_internal);
            return amxToVector(b, m, b.CreateCall(f, {amxI16(m, 16), amxI16(m, 64)}, "amx.zero"));
        }
        return b.CreateVectorSplat(vt->getNumElements(), b.CreateZExtOrTrunc(value, vt->getElementType()));
    }

    // No native inline ray query: the noun is a software BVH, so RayQuery walks it.
    NounImpl accelImpl() const override { return NounImpl::SoftwareBvh; }

private:
    std::shared_ptr<llvm::TargetMachine> levelTm;   // null: the build host

    unsigned allocaAddressSpace() const override { return 0; }
    unsigned constantTableAddressSpace() const override { return 0; }

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

    // Grid-stride stride: work-items in `dim` as i32, nctaid·ntid, from coord params.
    llvm::Value* gridSize(llvm::IRBuilderBase& b, llvm::Module&,
                          unsigned dim) override {
        return b.CreateMul(coord(b, /*group=*/9, dim),   // nctaid.{x,y,z}
                           coord(b, /*group=*/6, dim),   // ntid.{x,y,z}
                           "xpu.gridsize");
    }

    // Emits the marker call the registration pass fissions the work-item loop at. It
    // stays impure, noinline and noduplicate so nothing deletes or clones it first.
    void workgroupBarrier(llvm::IRBuilderBase& b, llvm::Module& m) override {
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

    // Calls libc `printf` (a CPU kernel is host code under LLJIT), f32 args promoted
    // to double as C varargs require.
    void devicePrintf(llvm::IRBuilderBase& b, llvm::Module& m, llvm::Value* fmt,
                      llvm::ArrayRef<llvm::Value*> args) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* ptr = llvm::PointerType::get(ctx, 0);
        auto* fnTy = llvm::FunctionType::get(i32, {ptr}, /*vararg=*/true);
        llvm::FunctionCallee pf = m.getOrInsertFunction("printf", fnTy);
        std::vector<llvm::Value*> call;
        call.reserve(args.size() + 1);
        call.push_back(fmt);
        for (llvm::Value* a : args) {
            if (a->getType()->isFloatTy())
                a = b.CreateFPExt(a, llvm::Type::getDoubleTy(ctx), "printf.f2d");
            call.push_back(a);
        }
        b.CreateCall(pf, call);
    }

    // Reads the specialization constant at runtime rather than baking the literal, so
    // a host override needs no recompile; the helper returns `defaultValue` if none.
    llvm::Value* specConstantI32(llvm::IRBuilderBase& b, llvm::Module& m,
                                 unsigned slot, int32_t defaultValue) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(i32, {i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_spec_i32", fnTy);
        return b.CreateCall(
            callee,
            {llvm::ConstantInt::get(i32, slot),
             llvm::ConstantInt::get(i32, (uint64_t) (int64_t) defaultValue)},
            "spec.i32");
    }
    llvm::Value* specConstantF32(llvm::IRBuilderBase& b, llvm::Module& m,
                                 unsigned slot, float defaultValue) override {
        llvm::LLVMContext& ctx = m.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* f32 = llvm::Type::getFloatTy(ctx);
        auto* fnTy = llvm::FunctionType::get(f32, {i32, f32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_spec_f32", fnTy);
        return b.CreateCall(callee,
                            {llvm::ConstantInt::get(i32, slot),
                             llvm::ConstantFP::get(f32, defaultValue)},
                            "spec.f32");
    }

    // Nothing to mark: the C calling convention and external linkage set at creation
    // are exactly what the host driver and JIT look up.
    void decorateKernel(llvm::Function*, llvm::Module&) override {
    }

    // Buffer, texture and image handles as flat addrspace(0) pointers (overriding the
    // addrspace(1) default) and scalars by value, then the i32 coordinate params.
    llvm::Function* createKernel(
        llvm::Module& m, const std::string& name,
        const std::vector<KernelParam>& params) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        std::vector<llvm::Type*> tys;
        tys.reserve(params.size() + kNumCoordParams);
        for (auto& p : params) {
            // An Image2D handle is a host image-record pointer, but its
            // KernelParam.type is `float`: without isImage it arrives as a scalar.
            tys.push_back((p.isBuffer || p.isTexture || p.isImage)
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
        // How coord() tells a kernel from a @Device helper, which has no coord params.
        fn->addFnAttr("cajeta-cpu-kernel");
        decorateKernel(fn, m);
        return fn;
    }

    // A @Device helper's Buffer<T> param is the flat host pointer kernels also get.
    llvm::Type* bufferParamType(llvm::Module& m, llvm::Type* /*elemTy*/) override {
        return llvm::PointerType::get(m.getContext(), 0);
    }

    // Bilinear/nearest sample through the C runtime, which owns the addressing math.
    // `samplerHandle`'s {filterMode, addressMode} struct is unpacked to two i32s.
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

    // Unfiltered exact-texel read at i32 indices: the runtime indexes the decoded
    // store directly, or, for an integer texel type, reinterprets it as raw bits.
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
        auto* fnTy = llvm::FunctionType::get(
            v4t, {llvm::PointerType::get(ctx, 0), i32, i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee = m.getOrInsertFunction(sym, fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {texHandle, x, y, lod}, "tex.fetch");
    }

    // Trilinear 3-D sample: the 2-D sampler's runtime call with a third coordinate.
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

    // Unfiltered 3-D voxel read, float or integer variant by texel type.
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

    // A 1-D texture is allocated height-1, so reuse the 2-D sampler at v = 0.5.
    llvm::Value* sampleTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                 llvm::Value* texHandle, llvm::Value* samplerHandle,
                                 llvm::Value* u) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Value* v = llvm::ConstantFP::get(f32, 0.5);
        llvm::Value* lod = llvm::ConstantFP::get(f32, 0.0);
        return sampleTexture(b, m, texHandle, samplerHandle, u, v, lod);
    }

    // The 1-D exact-texel read: the same height-1 reuse, with y = 0.
    llvm::Value* fetchTexture1D(llvm::IRBuilderBase& b, llvm::Module& m,
                                llvm::Value* texHandle, llvm::Value* x,
                                llvm::Type* texelTy) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* y = llvm::ConstantInt::get(i32, 0);
        llvm::Value* lod = llvm::ConstantInt::get(i32, 0);
        return fetchTexture(b, m, texHandle, x, y, texelTy, lod);
    }

    // Bilinear within the selected layer only, so not the trilinear 3-D symbol.
    llvm::Value* sampleTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                      llvm::Value* texHandle,
                                      llvm::Value* samplerHandle, llvm::Value* u,
                                      llvm::Value* v, llvm::Value* layer) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        llvm::Value* filterMode =
            b.CreateExtractValue(samplerHandle, {0}, "samp.filter");
        llvm::Value* addressMode =
            b.CreateExtractValue(samplerHandle, {1}, "samp.addr");
        auto* v4f = llvm::FixedVectorType::get(f32, 4);
        auto* fnTy = llvm::FunctionType::get(
            v4f, {llvm::PointerType::get(ctx, 0), i32, i32, f32, f32, i32},
            /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_tex2da_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee,
                            {texHandle, filterMode, addressMode, u, v, layer},
                            "tex2da.sample");
    }

    // Layers are stored as a volume's z slices, so this is the 3-D fetch at z = layer.
    llvm::Value* fetchTexture2DArray(llvm::IRBuilderBase& b, llvm::Module& m,
                                     llvm::Value* texHandle, llvm::Value* x,
                                     llvm::Value* y, llvm::Value* layer,
                                     llvm::Type* texelTy) override {
        return fetchTexture3D(b, m, texHandle, x, y, layer, texelTy);
    }

    // The runtime projects the direction onto a cube face, then filters within it.
    llvm::Value* sampleTextureCube(llvm::IRBuilderBase& b, llvm::Module& m,
                                   llvm::Value* texHandle, llvm::Value* samplerHandle,
                                   llvm::Value* x, llvm::Value* y,
                                   llvm::Value* z) override {
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
            m.getOrInsertFunction("__cajeta_xpu_cpu_texcube_sample_rgba", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee,
                            {texHandle, filterMode, addressMode, x, y, z},
                            "texcube.sample");
    }

    // Image2D store and load: `imgHandle` points at a flat R32f host image record.
    void storeImage(llvm::IRBuilderBase& b, llvm::Module& m,
                    llvm::Value* imgHandle, llvm::Value* x, llvm::Value* y,
                    llvm::Value* value) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            {llvm::PointerType::get(ctx, 0), i32, i32, f32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_image_store", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        b.CreateCall(callee, {imgHandle, x, y, value});
    }

    llvm::Value* loadImage(llvm::IRBuilderBase& b, llvm::Module& m,
                           llvm::Value* imgHandle, llvm::Value* x,
                           llvm::Value* y) override {
        llvm::LLVMContext& ctx = m.getContext();
        llvm::Type* f32 = llvm::Type::getFloatTy(ctx);
        llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
        auto* fnTy = llvm::FunctionType::get(
            f32, {llvm::PointerType::get(ctx, 0), i32, i32}, /*vararg=*/false);
        llvm::FunctionCallee callee =
            m.getOrInsertFunction("__cajeta_xpu_cpu_image_load", fnTy);
        if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
            f->setDoesNotThrow();
        return b.CreateCall(callee, {imgHandle, x, y}, "img.load");
    }

    // Wave ops call `__cajeta_xpu_wave_*` stubs with width-1 scalar semantics;
    // registration attaches VFABI variants so LoopVectorize widens them to the host
    // width W. `width()` takes no argument, so registration rewrites it to W instead.
    llvm::Value* waveWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_width", i32, {}, "wave.width");
    }
    // The Group surface IS the wave on this backend (folded 2026-09-28): one
    // surface, one width, on every backend. width() is the wave width call
    // registration rewrites to the host SIMD width W, laneId() the lane within
    // it, and the reduces are the wave reduces whose VFABI variants widen at
    // W. A Group kernel therefore vectorizes exactly as a wave kernel does.
    // It used to be one work-item wide here (width 1, lane 0, identity
    // reduce), which was true while cpu kernels ran scalar and became a lie
    // the day wave kernels vectorized: a kernel striding by width / 4 strode
    // by 0 and spun for 33 minutes on a two-row mat-vec.
    llvm::Value* groupWidth(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return waveWidth(b, m);
    }
    llvm::Value* groupLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return waveLaneId(b, m);
    }
    // A scalar twin runs the wave at one lane.
    unsigned minWaveWidth() const override { return 1; }
    llvm::Value* groupReduceF32(llvm::IRBuilderBase& b, llvm::Module& m,
                                WaveReduceFOp op, llvm::Value* value) override {
        return waveReduceF32(b, m, op, value);
    }
    llvm::Value* groupReduceF32Segmented(llvm::IRBuilderBase& b, llvm::Module& m,
                                         WaveReduceFOp op, llvm::Value* value,
                                         llvm::Value* seg) override {
        return waveReduceF32Segmented(b, m, op, value, seg);
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
        if (!pred->getType()->isIntegerTy(1))
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
        const char* sym;
        switch (op) {
            case WaveReduceOp::Max: sym = "__cajeta_xpu_wave_reduce_max_u32"; break;
            case WaveReduceOp::Min: sym = "__cajeta_xpu_wave_reduce_min_u32"; break;
            case WaveReduceOp::And: sym = "__cajeta_xpu_wave_reduce_and_u32"; break;
            case WaveReduceOp::Or:  sym = "__cajeta_xpu_wave_reduce_or_u32"; break;
            case WaveReduceOp::Xor: sym = "__cajeta_xpu_wave_reduce_xor_u32"; break;
            case WaveReduceOp::SMax: sym = "__cajeta_xpu_wave_reduce_smax_u32"; break;
            case WaveReduceOp::SMin: sym = "__cajeta_xpu_wave_reduce_smin_u32"; break;
        }
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.reduce");
    }
    llvm::Value* waveReduceF32(llvm::IRBuilderBase& b, llvm::Module& m,
                               WaveReduceFOp op, llvm::Value* value) override {
        const char* sym = op == WaveReduceFOp::Sum
            ? "__cajeta_xpu_wave_reduce_sum_f32"
            : op == WaveReduceFOp::Max ? "__cajeta_xpu_wave_reduce_max_f32"
                                       : "__cajeta_xpu_wave_reduce_min_f32";
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        return pureCall(b, m, sym, f32, {value}, "wave.reducef");
    }

    // A stub with the segment as a DATA argument; registration attaches a
    // variant that reduces each aligned span of `segment` lanes separately (a
    // select-guarded butterfly, so the segment can be a runtime value). This
    // used to route to the whole-wave reduce on the grounds that a segment
    // always covered the host's 8- or 16-lane wave; a declared 32-lane wave
    // holding four 8-lane segments (the flash decode kernels) proved the
    // grounds gone (2026-09-30).
    llvm::Value* waveReduceF32Segmented(llvm::IRBuilderBase& b, llvm::Module& m,
                                        WaveReduceFOp op, llvm::Value* value,
                                        llvm::Value* segment) override {
        const char* sym = op == WaveReduceFOp::Sum
            ? "__cajeta_xpu_wave_reduce_sum_f32_seg"
            : "__cajeta_xpu_wave_reduce_max_f32_seg";
        llvm::Type* f32 = llvm::Type::getFloatTy(m.getContext());
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        llvm::Value* seg = b.CreateZExtOrTrunc(segment, i32, "wave.seg");
        return pureCall(b, m, sym, f32, {value, seg}, "wave.reducef.seg");
    }
    // A stub, not the base Hillis-Steele shuffle loop, which does not vectorize here.
    llvm::Value* waveScan(llvm::IRBuilderBase& b, llvm::Module& m,
                          WaveScanOp op, llvm::Value* value) override {
        const char* sym = op == WaveScanOp::Sum
            ? "__cajeta_xpu_wave_prefix_sum_u32"
            : "__cajeta_xpu_wave_prefix_product_u32";
        llvm::Type* i32 = llvm::Type::getInt32Ty(m.getContext());
        return pureCall(b, m, sym, i32, {value}, "wave.scan");
    }
    // The int64 reduce and scan are stubs with width-W variants, as the 32-bit
    // ones are; the int64 shuffle keeps the default, two halves through the
    // 32-bit shuffle stub.
    llvm::Value* waveReduce64(llvm::IRBuilderBase& b, llvm::Module& m,
                              WaveReduce64Op op, llvm::Value* value) override {
        const char* sym = op == WaveReduce64Op::Sum ? "__cajeta_xpu_wave_reduce_sum_i64"
                        : op == WaveReduce64Op::SMax ? "__cajeta_xpu_wave_reduce_smax_i64"
                                                     : "__cajeta_xpu_wave_reduce_smin_i64";
        llvm::Type* i64 = llvm::Type::getInt64Ty(m.getContext());
        return pureCall(b, m, sym, i64, {value}, "wave.reduce64");
    }
    llvm::Value* waveScan64(llvm::IRBuilderBase& b, llvm::Module& m,
                            llvm::Value* value) override {
        llvm::Type* i64 = llvm::Type::getInt64Ty(m.getContext());
        return pureCall(b, m, "__cajeta_xpu_wave_prefix_sum_i64", i64, {value},
                        "wave.scan64");
    }
    llvm::Value* waveScanMinMax(llvm::IRBuilderBase& b, llvm::Module& m, bool isMax,
                                llvm::Value* value) override {
        const bool is64 = value->getType()->isIntegerTy(64);
        std::string sym = std::string("__cajeta_xpu_wave_prefix_") + (isMax ? "smax" : "smin")
                        + (is64 ? "_i64" : "_i32");
        return pureCall(b, m, sym.c_str(), value->getType(), {value}, "wave.scanmm");
    }
    // The work-item index modulo the wave width; lane 0 in the width-1 fallback.
    llvm::Value* waveLaneId(llvm::IRBuilderBase& b, llvm::Module& m) override {
        return b.CreateURem(threadId(b, m, 0), waveWidth(b, m), "wave.laneid");
    }

private:
    // Calls a wave stub marked memory-none/willreturn/nounwind, as widening requires.
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
        // A @Device helper has no coord params: unguarded, the index below underflows.
        if (!fn->hasFnAttribute("cajeta-cpu-kernel") ||
            fn->arg_size() < kNumCoordParams)
            throw cajeta::Exception(
                "XPU CPU backend: thread/workgroup coordinate builtins are not "
                "supported inside a @Device helper (only in the @Kernel body); "
                "pass the coordinate in as a parameter instead",
                "XPU-C01");
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

llvm::Function* lowerKernel(const MethodPtr& method, llvm::Module& deviceModule,
                            std::shared_ptr<llvm::TargetMachine> tm) {
    CpuTarget target(std::move(tm));
    return cajeta::xpu::lowerKernel(method, deviceModule, target);
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
