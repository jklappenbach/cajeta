//
// VectorOps — shared LLVM-IR builders for the Vector<T,N> value type.
//
// Both codegen paths emit identical vector IR through these helpers: the host
// expression codegen (NewExpression / DotExpression / BinaryOpExpression /
// ArrayIndexExpression / MethodCallExpression) and the separate device kernel
// walker (xpu/lowering/KernelLowering's DeviceLowerer). Keeping the lane
// mapping, construction, splat, and dot/length/normalize sequences in one
// place is the host/device-drift mitigation called for in the plan.
//
// All helpers take an llvm::IRBuilderBase& so they serve both the host
// IRBuilder<> and the device builder.
//

#pragma once

#include <string>
#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/TargetParser/Triple.h"

namespace cajeta {
namespace vecops {

    // Component letter -> lane index for the first four lanes, or -1 when the
    // character is not a vector component letter. x/r=0, y/g=1, z/b=2, w/a=3.
    inline int laneForComponent(char c) {
        switch (c) {
            case 'x': case 'r': return 0;
            case 'y': case 'g': return 1;
            case 'z': case 'b': return 2;
            case 'w': case 'a': return 3;
            default: return -1;
        }
    }

    // Whole identifier -> lane index, for the single-component case (`.x`).
    // Returns -1 for a non-component name or a multi-letter swizzle (use
    // swizzleLanes for those).
    inline int laneForComponentName(const std::string& name) {
        if (name.size() != 1) return -1;
        return laneForComponent(name[0]);
    }

    // Parse a swizzle identifier (`xyz`, `xxyy`, `wzyx`, `rgba`, …) into its
    // lane indices. Returns an empty vector if `name` isn't 2-4 component
    // letters (length 1 is the scalar `.x` path; >4 is not a swizzle). The
    // caller must still bounds-check each lane against the source lane count.
    inline std::vector<int> swizzleLanes(const std::string& name) {
        if (name.size() < 2 || name.size() > 4) return {};
        std::vector<int> lanes;
        for (char c : name) {
            int l = laneForComponent(c);
            if (l < 0) return {};
            lanes.push_back(l);
        }
        return lanes;
    }

    // Multi-component swizzle read: `<lanes.size() x T>` where result lane k =
    // source lane `lanes[k]` (a shufflevector). Lanes must be < the source
    // count (caller-validated).
    inline llvm::Value* swizzle(llvm::IRBuilderBase& b, llvm::Value* vec,
                                const std::vector<int>& lanes) {
        return b.CreateShuffleVector(vec, lanes, "vec.swizzle");
    }

    // Coerce a scalar to a target scalar type (vector element). Handles the
    // common cases of int-literal -> float element and int-width adjustment.
    // Signed conversions are assumed (v1); unsigned-int -> float vectors are an
    // edge case documented as a follow-on.
    inline llvm::Value* coerceScalar(llvm::IRBuilderBase& b, llvm::Value* v,
                                     llvm::Type* target) {
        llvm::Type* src = v->getType();
        if (src == target) return v;
        bool sFloat = src->isFloatingPointTy();
        bool tFloat = target->isFloatingPointTy();
        if (sFloat && tFloat) return b.CreateFPCast(v, target, "vec.fpcast");
        if (!sFloat && tFloat) return b.CreateSIToFP(v, target, "vec.sitofp");
        if (sFloat && !tFloat) return b.CreateFPToSI(v, target, "vec.fptosi");
        return b.CreateIntCast(v, target, /*isSigned=*/true, "vec.intcast");
    }

    // Build a `<lanes x elemTy>` from N scalar elements. Uses ConstantVector
    // when every element is constant, else an undef + insertelement chain.
    inline llvm::Value* buildVector(llvm::IRBuilderBase& b, llvm::Type* elemTy,
                                    unsigned lanes,
                                    const std::vector<llvm::Value*>& elems) {
        auto* vecTy = llvm::FixedVectorType::get(elemTy, lanes);
        bool allConst = true;
        for (auto* e : elems) {
            if (!llvm::isa<llvm::Constant>(e)) { allConst = false; break; }
        }
        if (allConst) {
            std::vector<llvm::Constant*> cs;
            cs.reserve(elems.size());
            for (auto* e : elems) cs.push_back(llvm::cast<llvm::Constant>(e));
            return llvm::ConstantVector::get(cs);
        }
        llvm::Value* acc = llvm::UndefValue::get(vecTy);
        for (unsigned i = 0; i < lanes; ++i) {
            acc = b.CreateInsertElement(acc, elems[i], b.getInt32(i), "vec.init");
        }
        return acc;
    }

    inline llvm::Value* extractLane(llvm::IRBuilderBase& b, llvm::Value* vec,
                                    llvm::Value* idx) {
        return b.CreateExtractElement(vec, idx, "vec.elt");
    }

    inline llvm::Value* extractLane(llvm::IRBuilderBase& b, llvm::Value* vec,
                                    unsigned idx) {
        return b.CreateExtractElement(vec, b.getInt32(idx), "vec.elt");
    }

    inline llvm::Value* insertLane(llvm::IRBuilderBase& b, llvm::Value* vec,
                                   llvm::Value* val, llvm::Value* idx) {
        return b.CreateInsertElement(vec, val, idx, "vec.ins");
    }

    inline llvm::Value* insertLane(llvm::IRBuilderBase& b, llvm::Value* vec,
                                   llvm::Value* val, unsigned idx) {
        return b.CreateInsertElement(vec, val, b.getInt32(idx), "vec.ins");
    }

    // Broadcast a scalar across `lanes` lanes -> `<lanes x scalarTy>`.
    inline llvm::Value* splat(llvm::IRBuilderBase& b, llvm::Value* scalar,
                              unsigned lanes) {
        return b.CreateVectorSplat(lanes, scalar, "vec.splat");
    }

    // eqMask(v, needle) -> iN: per-lane equality packed into an integer, bit i
    // set iff lane i == needle (the SIMD movemask). icmp -> <N x i1> -> bitcast
    // iN; LLVM lowers this to pcmpeqb+pmovmskb (x86) / cmeq+reduce (NEON).
    // `needle` must already be the element scalar type. Caller zext's to int32.
    inline llvm::Value* eqMask(llvm::IRBuilderBase& b, llvm::Value* v,
                               llvm::Value* needle) {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
        unsigned n = vecTy->getNumElements();
        llvm::Value* sp = b.CreateVectorSplat(n, needle, "eqmask.splat");
        llvm::Value* cmp = vecTy->getElementType()->isFloatingPointTy()
            ? b.CreateFCmpOEQ(v, sp, "eqmask.cmp")
            : b.CreateICmpEQ(v, sp, "eqmask.cmp");            // <N x i1>
        auto* iN = llvm::IntegerType::get(b.getContext(), n);
        return b.CreateBitCast(cmp, iN, "eqmask.bits");      // packed movemask
    }

    // compressStore(ptr, vec, mask): pack the lanes of `vec` whose `mask` bit is
    // set into contiguous memory at `ptr`, low lane first — AVX-512 vpcompress*
    // (the partition primitive behind vqsort / x86-simd-sort). Lowers to
    // @llvm.masked.compressstore, overloaded ONLY on the vector type (the pointer
    // is a plain `ptr`, the mask `<N x i1>` is derived). Writes popcount(mask)
    // elements; the caller advances its write cursor by maskPopcount(mask). On a
    // target without AVX-512 the backend scalarizes it (correct, just slower).
    inline void compressStore(llvm::IRBuilderBase& b, llvm::Value* ptr,
                              llvm::Value* vec, llvm::Value* mask) {
        llvm::Module* m = b.GetInsertBlock()->getModule();
        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
            m, llvm::Intrinsic::masked_compressstore, {vec->getType()});
        b.CreateCall(fn, {vec, ptr, mask});
    }

    // maskPopcount(mask) -> i32: number of set lanes in an `<N x i1>` mask. Bit-
    // cast to iN then ctpop — the count compressStore just wrote, so a partition
    // can bump its cursor by it.
    inline llvm::Value* maskPopcount(llvm::IRBuilderBase& b, llvm::Value* mask) {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(mask->getType());
        unsigned n = vecTy->getNumElements();
        auto* iN = llvm::IntegerType::get(b.getContext(), n);
        llvm::Value* bits = b.CreateBitCast(mask, iN, "mask.bits");
        llvm::Value* pc = b.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, bits,
                                                 nullptr, "mask.popcount");
        return b.CreateZExtOrTrunc(pc, llvm::Type::getInt32Ty(b.getContext()),
                                   "mask.count");
    }

    // dot(a, b) -> scalar. Element-wise multiply then horizontal add. `isFloat`
    // picks fmul/fadd vs mul/add from the element type.
    inline llvm::Value* dot(llvm::IRBuilderBase& b, llvm::Value* a,
                            llvm::Value* c, bool isFloat) {
        llvm::Value* prod = isFloat ? b.CreateFMul(a, c, "vec.dot.mul")
                                    : b.CreateMul(a, c, "vec.dot.mul");
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(prod->getType());
        unsigned n = vecTy->getNumElements();
        llvm::Value* acc = extractLane(b, prod, 0u);
        for (unsigned i = 1; i < n; ++i) {
            llvm::Value* e = extractLane(b, prod, i);
            acc = isFloat ? b.CreateFAdd(acc, e, "vec.dot.add")
                          : b.CreateAdd(acc, e, "vec.dot.add");
        }
        return acc;
    }

    // idotWiden(a, c, acc, aSigned, cSigned) -> i32. Integer dot product with
    // int32 accumulation: each lane is widened to i32 (sext if signed, zext if
    // unsigned), multiplied, and summed into `acc`. The portable form of DP4a,
    // correct on every backend; device backends override the lowering seam
    // (LoweringTarget::integerDot4x8) to emit a hardware DP4a instead. `a`/`c`
    // are <N x iK> vectors; `acc` is i32 (pass i32 0 for a plain dot).
    //
    // The two flags are INDEPENDENT and that is the whole point. `dot` is
    // symmetric and passes the same flag twice; `dotAccum` is unsigned
    // weights x SIGNED activations, the asymmetry the instruction family
    // exists for (vpdpbusd, usdot, vqdotsu). A single shared flag cannot
    // express it: with an unsigned receiver it zero-extended the activations
    // too, silently, on every device backend (measured on gfx1151 -- 6440
    // where the host returns -1240; threaded-forward-path spec 3.5.4-3.5.6).
    inline llvm::Value* idotWiden(llvm::IRBuilderBase& b, llvm::Value* a,
                                  llvm::Value* c, llvm::Value* acc,
                                  bool aSigned, bool cSigned) {
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
        unsigned n = vecTy->getNumElements();
        llvm::Type* i32 = llvm::Type::getInt32Ty(b.getContext());
        llvm::Value* sum = acc;
        for (unsigned i = 0; i < n; ++i) {
            llvm::Value* ai = b.CreateIntCast(extractLane(b, a, i), i32,
                                              aSigned, "idot.a");
            llvm::Value* ci = b.CreateIntCast(extractLane(b, c, i), i32,
                                              cSigned, "idot.b");
            sum = b.CreateAdd(sum, b.CreateMul(ai, ci, "idot.mul"), "idot.acc");
        }
        return sum;
    }

    // length(v) -> scalar = sqrt(dot(v, v)). Float element only.
    inline llvm::Value* length(llvm::IRBuilderBase& b, llvm::Value* v) {
        llvm::Value* d = dot(b, v, v, /*isFloat=*/true);
        return b.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, d, nullptr,
                                      "vec.len");
    }

    // normalize(v) -> Vector = v / splat(length(v)). Float element only.
    inline llvm::Value* normalize(llvm::IRBuilderBase& b, llvm::Value* v) {
        llvm::Value* len = length(b, v);
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
        llvm::Value* s = splat(b, len, vecTy->getNumElements());
        return b.CreateFDiv(v, s, "vec.norm");
    }

    inline unsigned laneCount(llvm::Value* v) {
        return llvm::cast<llvm::FixedVectorType>(v->getType())->getNumElements();
    }

    // Element-wise min / max. Float -> minnum/maxnum; integer -> the
    // signedness-correct smin/umin / smax/umax. All three select natively as
    // vector intrinsics on every backend.
    inline llvm::Value* vmin(llvm::IRBuilderBase& b, llvm::Value* a,
                             llvm::Value* c, bool isFloat, bool isSigned) {
        llvm::Intrinsic::ID id = isFloat ? llvm::Intrinsic::minnum
                                : (isSigned ? llvm::Intrinsic::smin
                                            : llvm::Intrinsic::umin);
        return b.CreateBinaryIntrinsic(id, a, c, nullptr, "vec.min");
    }
    inline llvm::Value* vmax(llvm::IRBuilderBase& b, llvm::Value* a,
                             llvm::Value* c, bool isFloat, bool isSigned) {
        llvm::Intrinsic::ID id = isFloat ? llvm::Intrinsic::maxnum
                                : (isSigned ? llvm::Intrinsic::smax
                                            : llvm::Intrinsic::umax);
        return b.CreateBinaryIntrinsic(id, a, c, nullptr, "vec.max");
    }

    // clamp(v, lo, hi) -> max(min(v, hi), lo), with scalar bounds broadcast
    // across the lanes. lo/hi are already the element type.
    inline llvm::Value* clamp(llvm::IRBuilderBase& b, llvm::Value* v,
                              llvm::Value* lo, llvm::Value* hi, bool isFloat,
                              bool isSigned) {
        unsigned n = laneCount(v);
        llvm::Value* loV = splat(b, lo, n);
        llvm::Value* hiV = splat(b, hi, n);
        return vmax(b, vmin(b, v, hiV, isFloat, isSigned), loV, isFloat, isSigned);
    }

    // lerp(a, b, t) -> a + (b - a) * t, t a scalar broadcast across the lanes.
    // Float element only (the only sensible interpolation domain).
    inline llvm::Value* lerp(llvm::IRBuilderBase& b, llvm::Value* a,
                             llvm::Value* c, llvm::Value* t) {
        llvm::Value* tV = splat(b, t, laneCount(a));
        llvm::Value* diff = b.CreateFSub(c, a, "vec.lerp.diff");
        llvm::Value* scaled = b.CreateFMul(diff, tV, "vec.lerp.mul");
        return b.CreateFAdd(a, scaled, "vec.lerp");
    }

    // cross(a, b) -> the 3-D cross product (`<3 x T>`, float element only):
    //   (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx).
    inline llvm::Value* cross(llvm::IRBuilderBase& b, llvm::Value* a,
                              llvm::Value* c) {
        llvm::Value* ax = extractLane(b, a, 0u), * ay = extractLane(b, a, 1u),
                   * az = extractLane(b, a, 2u);
        llvm::Value* cx = extractLane(b, c, 0u), * cy = extractLane(b, c, 1u),
                   * cz = extractLane(b, c, 2u);
        llvm::Value* x = b.CreateFSub(b.CreateFMul(ay, cz), b.CreateFMul(az, cy),
                                      "cross.x");
        llvm::Value* y = b.CreateFSub(b.CreateFMul(az, cx), b.CreateFMul(ax, cz),
                                      "cross.y");
        llvm::Value* z = b.CreateFSub(b.CreateFMul(ax, cy), b.CreateFMul(ay, cx),
                                      "cross.z");
        llvm::Value* acc = llvm::UndefValue::get(a->getType());
        acc = insertLane(b, acc, x, 0u);
        acc = insertLane(b, acc, y, 1u);
        return insertLane(b, acc, z, 2u);
    }

    // reflect(i, n) -> i - 2*dot(i,n)*n (n assumed unit length). Float element.
    inline llvm::Value* reflect(llvm::IRBuilderBase& b, llvm::Value* i,
                                llvm::Value* n) {
        llvm::Value* d = dot(b, i, n, /*isFloat=*/true);
        llvm::Value* two = llvm::ConstantFP::get(d->getType(), 2.0);
        llvm::Value* s = splat(b, b.CreateFMul(two, d), laneCount(i));
        return b.CreateFSub(i, b.CreateFMul(s, n), "vec.reflect");
    }

    // distance(a, b) -> length(a - b), a scalar. Float element only.
    inline llvm::Value* distance(llvm::IRBuilderBase& b, llvm::Value* a,
                                 llvm::Value* c) {
        return length(b, b.CreateFSub(a, c, "vec.dist.diff"));
    }

    // refract(i, n, eta) -> the GLSL refraction (n, i assumed unit; eta the
    // ratio of indices). k = 1 - eta^2*(1 - dot(n,i)^2); total internal
    // reflection (k < 0) yields the zero vector. Float element only.
    inline llvm::Value* refract(llvm::IRBuilderBase& b, llvm::Value* i,
                                llvm::Value* n, llvm::Value* eta) {
        llvm::Type* ft = eta->getType();
        llvm::Value* one = llvm::ConstantFP::get(ft, 1.0);
        llvm::Value* zero = llvm::ConstantFP::get(ft, 0.0);
        llvm::Value* ni = dot(b, n, i, /*isFloat=*/true);
        llvm::Value* eta2 = b.CreateFMul(eta, eta);
        llvm::Value* k = b.CreateFSub(
            one, b.CreateFMul(eta2, b.CreateFSub(one, b.CreateFMul(ni, ni))));
        // sqrt(max(k,0)) keeps the math NaN-free; the select masks TIR to zero.
        llvm::Value* sk = b.CreateUnaryIntrinsic(
            llvm::Intrinsic::sqrt,
            b.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, k, zero), nullptr);
        llvm::Value* coef = b.CreateFAdd(b.CreateFMul(eta, ni), sk);
        unsigned lanes = laneCount(i);
        llvm::Value* res = b.CreateFSub(
            b.CreateFMul(splat(b, eta, lanes), i),
            b.CreateFMul(splat(b, coef, lanes), n), "vec.refract");
        llvm::Value* tir = b.CreateVectorSplat(
            lanes, b.CreateFCmpOLT(k, zero), "vec.refract.tir");
        return b.CreateSelect(tir, llvm::Constant::getNullValue(i->getType()),
                              res, "vec.refract.sel");
    }


    // ── simd-fused-integer-madd 1.2.1: dotAccum ────────────────────────
    //
    // dotAccum(w, a, acc) -> <N x i32>: multiply four ADJACENT int8 pairs, sum
    // the four products, accumulate into the corresponding i32 lane. `w` and
    // `a` are <4N x i8>; `acc` is <N x i32>.
    //
    // This is the generalization of the DP4a `dot` above — same operation,
    // result kept in VECTOR space rather than collapsed to a scalar. Reduce
    // frequency is what costs in quantized kernels (cajeta-llama 15.1.18a: the
    // same kernel reducing per sub-block ran 70.7 ms against 24.2 ms reducing
    // once per block), so a partial reduction that stays in lanes is the whole
    // point.
    //
    // Every tier is BIT-IDENTICAL. That is a guarantee, not an aspiration: the
    // operation is integer, so there is no reassociation hazard of the kind
    // float accumulation has, and the non-saturating encoding is used so the
    // equality holds exactly. Taking a faster tier changes speed, never the
    // answer — which is what lets the scalar tier stand as the correctness
    // floor.
    //
    // Tiers: x86 VNNI (`vpdpbusd`, one instruction) -> the portable
    // `llvm.vector.partial.reduce.add` (lowers to fused ops on every target
    // LLVM supports) -> a scalar lane loop. Callers NEVER branch on target.
    /** Which fused-int-dot units the target actually has. A named set rather
     *  than a row of bools, because every one of them is a silent-speed knob:
     *  get one wrong and the answer stays correct while the kernel quietly
     *  loses its instruction. */
    struct DotAccumTargets {
        bool vnni        = false;   // x86 AVX512-VNNI / AVX-VNNI
        bool avx2        = false;   // x86 AVX2, the pre-VNNI tier
        bool armDotProd  = false;   // AArch64 ARMv8.2 dotprod: sdot / udot
        bool armI8mm     = false;   // AArch64 i8mm: usdot, the mixed form
        bool forceScalar = false;   // CAJETA_SIMD_SCALAR_FALLBACK=1
    };

    inline llvm::Value* dotAccum(llvm::IRBuilderBase& b, llvm::Module* m,
                                 llvm::Value* w, llvm::Value* a,
                                 llvm::Value* acc, bool wUnsigned,
                                 const DotAccumTargets& tgt) {
        const bool hasVnni     = tgt.vnni;
        const bool hasAvx2     = tgt.avx2;
        const bool forceScalar = tgt.forceScalar;
        llvm::LLVMContext& ctx = b.getContext();
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        auto* accTy = llvm::cast<llvm::FixedVectorType>(acc->getType());
        unsigned n = accTy->getNumElements();
        auto* srcTy = llvm::cast<llvm::FixedVectorType>(w->getType());
        unsigned lanes = srcTy->getNumElements();
        const llvm::Triple& triple = m->getTargetTriple();
        bool isX86 = triple.getArch() == llvm::Triple::x86_64
                     || triple.getArch() == llvm::Triple::x86;

        // Tier 1 — VNNI. `vpdpbusd` is unsigned x signed, which is exactly the
        // quantized shape (unsigned weights against signed activations); the
        // same asymmetry RISC-V chose for `vqdotsu`. Non-saturating variant so
        // the tiers agree bit-for-bit.
        if (!forceScalar && isX86 && hasVnni && wUnsigned && lanes == n * 4
                && (n == 4 || n == 8 || n == 16)) {
            llvm::Intrinsic::ID id = n == 16
                ? llvm::Intrinsic::x86_avx512_vpdpbusd_512
                : (n == 8 ? llvm::Intrinsic::x86_avx512_vpdpbusd_256
                          : llvm::Intrinsic::x86_avx512_vpdpbusd_128);
            llvm::Function* fn =
                llvm::Intrinsic::getOrInsertDeclaration(m, id);
            return b.CreateCall(fn, {acc, w, a}, "dotacc.vnni");
        }

        // Tier 1.5a — pre-VNNI x86 (AVX2), the THREE-instruction form:
        // vpmaddubsw + vpmaddwd + vpaddd, which is what llama.cpp emits.
        //
        // It SATURATES, and that is a deliberate, authorized narrowing of §4.8
        // (decided 2026-08-22). vpmaddubsw sums two adjacent u8 x i8 products
        // into an i16 lane with clamping, so it is exact iff every adjacent
        // pair satisfies |w0*a0 + w1*a1| <= 32767. That holds for all weight
        // values 0..128 against any int8 activation — 128 * 128 * 2 = 32768
        // lands exactly on i16 MIN, and 128 * 127 * 2 = 32512 is inside — which
        // covers every K-quant field there is: Q4_K's nibbles 0..15, Q5_K's
        // 0..31, Q6_K's 0..63. Above 128 it clamps and disagrees with the other
        // tiers, and that boundary is pinned by test rather than left to be
        // discovered.
        //
        // 3 instructions against the exact form's 15 and the portable tier's
        // 57, for one 32-pair dotAccum on haswell.
        if (!forceScalar && isX86 && !hasVnni && hasAvx2 && wUnsigned
                && lanes == n * 4 && (lanes % 32) == 0) {
            auto* i16Ty = llvm::Type::getInt16Ty(ctx);
            auto* v32i8 = llvm::FixedVectorType::get(
                llvm::Type::getInt8Ty(ctx), 32);
            auto* v16i16 = llvm::FixedVectorType::get(i16Ty, 16);
            llvm::Value* ones = llvm::ConstantVector::getSplat(
                llvm::ElementCount::getFixed(16),
                llvm::ConstantInt::get(i16Ty, 1));
            llvm::Function* maddubs = llvm::Intrinsic::getOrInsertDeclaration(
                m, llvm::Intrinsic::x86_avx2_pmadd_ub_sw);
            llvm::Function* maddwd = llvm::Intrinsic::getOrInsertDeclaration(
                m, llvm::Intrinsic::x86_avx2_pmadd_wd);
            unsigned chunks = lanes / 32;      // 8 accumulator lanes each
            llvm::SmallVector<llvm::Value*, 4> octs;
            for (unsigned c = 0; c < chunks; ++c) {
                llvm::SmallVector<int, 32> mb(32);
                for (unsigned i = 0; i < 32; ++i) mb[i] = (int) (c * 32 + i);
                llvm::Value* ws = b.CreateShuffleVector(w, w, mb, "dotacc.w32");
                llvm::Value* as = b.CreateShuffleVector(a, a, mb, "dotacc.a32");
                llvm::Value* p16 = b.CreateCall(maddubs,
                    {b.CreateBitCast(ws, v32i8), b.CreateBitCast(as, v32i8)},
                    "dotacc.ubsw");
                octs.push_back(b.CreateCall(maddwd,
                    {b.CreateBitCast(p16, v16i16), ones}, "dotacc.wd"));
            }
            while (octs.size() > 1) {
                llvm::SmallVector<llvm::Value*, 4> next;
                for (unsigned i = 0; i + 1 < octs.size(); i += 2) {
                    unsigned half = llvm::cast<llvm::FixedVectorType>(
                        octs[i]->getType())->getNumElements();
                    llvm::SmallVector<int, 32> cat(half * 2);
                    for (unsigned k = 0; k < half * 2; ++k) cat[k] = (int) k;
                    next.push_back(b.CreateShuffleVector(
                        octs[i], octs[i + 1], cat, "dotacc.cat"));
                }
                octs = next;
            }
            return b.CreateAdd(acc, octs[0], "dotacc.sat");
        }

        // Tier 1.5b — the EXACT pre-VNNI form, for the shapes 1.5a cannot take:
        // signed weights (vpmaddubsw's first operand is unsigned) and lane
        // counts under 32. Load-bearing, not a nicety: the
        // portable tier below reaches NO vpdp* instruction on x86 in this
        // LLVM, measured through llc on the exact IR emitted here, and costs
        // 57 instructions on haswell against vpdpbusd's 1.
        //
        // NOT llama.cpp's `vpmaddubsw` sequence, which is 3 instructions but
        // SATURATES: it sums two adjacent u8 x i8 products into an i16 lane,
        // and at the full operand range 255 x -128 twice is -65280, clamping
        // to -32768. llama.cpp can use it because its caller is always a
        // K-quant (nibbles 0..15 peak at 3810, a factor of 8 of headroom);
        // dotAccum is public surface over any Vector<uint8,4N>, and §4.8's
        // bit-identity is what lets the scalar tier stand as the correctness
        // floor. So: widen to i16 and multiply EXACTLY (|u8 x i8| <= 32640
        // fits), then reduce through vpmaddwd, which accumulates adjacent
        // pairs in i32 and cannot saturate.
        if (!forceScalar && isX86 && !hasVnni && hasAvx2 && lanes == n * 4
                && (lanes % 16) == 0) {
            auto* i16Ty = llvm::Type::getInt16Ty(ctx);
            auto* mulTy = llvm::FixedVectorType::get(i16Ty, lanes);
            llvm::Value* we = wUnsigned
                ? b.CreateZExt(w, mulTy, "dotacc.w16")
                : b.CreateSExt(w, mulTy, "dotacc.w16");
            llvm::Value* ae = b.CreateSExt(a, mulTy, "dotacc.a16");
            llvm::Value* mul = b.CreateMul(we, ae, "dotacc.mul16");
            llvm::Value* ones = llvm::ConstantVector::getSplat(
                llvm::ElementCount::getFixed(16),
                llvm::ConstantInt::get(i16Ty, 1));
            llvm::Function* pmadd = llvm::Intrinsic::getOrInsertDeclaration(
                m, llvm::Intrinsic::x86_avx2_pmadd_wd);
            // Each 16-lane chunk yields 8 pair-sums, which are groups 2i and
            // 2i+1 of four bytes each -- both always land in the same chunk,
            // so a chunk maps cleanly onto 4 accumulator lanes.
            unsigned chunks = lanes / 16;
            llvm::SmallVector<llvm::Value*, 4> quads;
            for (unsigned c = 0; c < chunks; ++c) {
                llvm::SmallVector<int, 16> m16(16);
                for (unsigned i = 0; i < 16; ++i) m16[i] = (int) (c * 16 + i);
                llvm::Value* piece = b.CreateShuffleVector(mul, mul, m16,
                                                           "dotacc.chunk");
                llvm::Value* pairs = b.CreateCall(pmadd, {piece, ones},
                                                  "dotacc.pairs");
                // adjacent-2 of the pair sums == adjacent-4 of the bytes
                llvm::SmallVector<int, 4> ev = {0, 2, 4, 6};
                llvm::SmallVector<int, 4> od = {1, 3, 5, 7};
                llvm::Value* e = b.CreateShuffleVector(pairs, pairs, ev,
                                                       "dotacc.ev");
                llvm::Value* o = b.CreateShuffleVector(pairs, pairs, od,
                                                       "dotacc.od");
                quads.push_back(b.CreateAdd(e, o, "dotacc.q"));
            }
            // Concatenate the per-chunk quads into <n x i32>.
            while (quads.size() > 1) {
                llvm::SmallVector<llvm::Value*, 4> next;
                for (unsigned i = 0; i + 1 < quads.size(); i += 2) {
                    unsigned half = llvm::cast<llvm::FixedVectorType>(
                        quads[i]->getType())->getNumElements();
                    llvm::SmallVector<int, 32> cat(half * 2);
                    for (unsigned k = 0; k < half * 2; ++k) cat[k] = (int) k;
                    next.push_back(b.CreateShuffleVector(
                        quads[i], quads[i + 1], cat, "dotacc.cat"));
                }
                quads = next;
            }
            return b.CreateAdd(acc, quads[0], "dotacc.pre");
        }

        // Tier 1.6 — AArch64. `usdot`/`sdot` are one instruction per 16 bytes,
        // the same shape as vpdpbusd, and NEON is 128-bit so this chunks into
        // 4-lane groups exactly as the pre-VNNI x86 tier does.
        //
        // UNVERIFIED BY EXECUTION. This LLVM registers only amdgcn / r600 /
        // x86 / x86-64, so no cajeta build can target AArch64 and no test here
        // can reach this branch — it stands exactly as Unit 17's NEON `tbl1`
        // path does. What HAS been checked, on an AArch64-capable llc 21: this
        // IR shape selects `usdot v0.4s, v1.16b, v2.16b` and `sdot` likewise,
        // one instruction each. So the shape and the operand layout are
        // measured; only the runtime answer is not.
        //
        // The two features are separate on purpose. Measured on that same llc:
        // a target with +dotprod but no +i8mm CANNOT SELECT usdot and dies, so
        // the mixed unsigned x signed form needs its own gate rather than
        // riding dotprod's.
        {
            bool armOk = wUnsigned ? tgt.armI8mm : tgt.armDotProd;
            if (!forceScalar && triple.isAArch64() && armOk && lanes == n * 4
                    && (lanes % 16) == 0) {
                auto* i8Ty = llvm::Type::getInt8Ty(ctx);
                auto* v16i8 = llvm::FixedVectorType::get(i8Ty, 16);
                auto* v4i32 = llvm::FixedVectorType::get(i32, 4);
                llvm::Intrinsic::ID id = wUnsigned
                    ? llvm::Intrinsic::aarch64_neon_usdot
                    : llvm::Intrinsic::aarch64_neon_sdot;
                llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                    m, id, {v4i32, v16i8});
                unsigned chunks = lanes / 16;
                llvm::SmallVector<llvm::Value*, 4> quads;
                for (unsigned c = 0; c < chunks; ++c) {
                    llvm::SmallVector<int, 16> mb(16);
                    for (unsigned i = 0; i < 16; ++i) mb[i] = (int) (c * 16 + i);
                    llvm::SmallVector<int, 4> ma(4);
                    for (unsigned i = 0; i < 4; ++i) ma[i] = (int) (c * 4 + i);
                    llvm::Value* ws = b.CreateShuffleVector(w, w, mb,
                                                            "dotacc.w16b");
                    llvm::Value* as = b.CreateShuffleVector(a, a, mb,
                                                            "dotacc.a16b");
                    llvm::Value* ac = b.CreateShuffleVector(acc, acc, ma,
                                                            "dotacc.acc4");
                    quads.push_back(b.CreateCall(fn, {ac, ws, as},
                                                 "dotacc.neon"));
                }
                while (quads.size() > 1) {
                    llvm::SmallVector<llvm::Value*, 4> next;
                    for (unsigned i = 0; i + 1 < quads.size(); i += 2) {
                        unsigned half = llvm::cast<llvm::FixedVectorType>(
                            quads[i]->getType())->getNumElements();
                        llvm::SmallVector<int, 32> cat(half * 2);
                        for (unsigned k = 0; k < half * 2; ++k) cat[k] = (int) k;
                        next.push_back(b.CreateShuffleVector(
                            quads[i], quads[i + 1], cat, "dotacc.cat"));
                    }
                    quads = next;
                }
                return quads[0];
            }
        }

        // Tier 2 — portable partial reduction. The CORRECTNESS fallback for
        // targets with no hand-written path; measured NOT to reach vpdp* on
        // x86 (see Tier 1.5), so it is not the performance story it was
        // originally specified to be.
        if (!forceScalar && lanes == n * 4) {
            auto* wideTy = llvm::FixedVectorType::get(i32, lanes);
            llvm::Value* we = wUnsigned ? b.CreateZExt(w, wideTy, "dotacc.w")
                                        : b.CreateSExt(w, wideTy, "dotacc.w");
            llvm::Value* ae = b.CreateSExt(a, wideTy, "dotacc.a");
            llvm::Value* mul = b.CreateMul(we, ae, "dotacc.mul");
            // llvm.vector.partial.reduce.add reduces STRIDED, not adjacent:
            // result[i] = acc[i] + in[i] + in[i+N] + in[i+2N] + in[i+3N].
            // vpdpbusd (and the scalar tier) sum lanes 4i..4i+3. Same count,
            // different lane mapping -- so deinterleave first, putting lane
            // 4i+k at position k*N+i. Caught by the bit-identical test, which
            // is exactly what that requirement is for.
            llvm::SmallVector<int, 64> mask(lanes);
            for (unsigned k = 0; k < 4; ++k)
                for (unsigned i = 0; i < n; ++i)
                    mask[k * n + i] = (int) (i * 4 + k);
            llvm::Value* shuf = b.CreateShuffleVector(mul, mul, mask,
                                                      "dotacc.deint");
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                m, llvm::Intrinsic::vector_partial_reduce_add,
                {accTy, wideTy});
            return b.CreateCall(fn, {acc, shuf}, "dotacc.pr");
        }

        // Tier 3 — scalar. The correctness floor, and what
        // CAJETA_SIMD_SCALAR_FALLBACK=1 forces.
        llvm::Value* out = acc;
        for (unsigned lane = 0; lane < n; ++lane) {
            llvm::Value* sum = extractLane(b, acc, lane);
            for (unsigned k = 0; k < 4; ++k) {
                unsigned idx = lane * 4 + k;
                if (idx >= lanes) break;
                llvm::Value* wi = b.CreateIntCast(
                    extractLane(b, w, idx), i32, !wUnsigned, "dotacc.wi");
                llvm::Value* ai = b.CreateIntCast(
                    extractLane(b, a, idx), i32, true, "dotacc.ai");
                sum = b.CreateAdd(sum, b.CreateMul(wi, ai, "dotacc.m"),
                                  "dotacc.s");
            }
            out = b.CreateInsertElement(out, sum, lane, "dotacc.ins");
        }
        return out;
    }

    // ── cajeta-llama Unit 17: tableLookup / widen / narrow / convert ────

    // tableLookup — the per-byte classifier engine (pshufb semantics):
    // result lane i = table[indices[i] & 0x0F], and 0 whenever the index's
    // high bit is set. On x86 this IS `@llvm.x86.ssse3.pshuf.b.128`; on
    // AArch64 it is NEON `tbl1` with indices masked to idx & 0x8F (tbl1
    // zeroes out-of-range indices 16..255, so high-bit lanes land in
    // 0x80..0x8F and zero — pshufb's contract exactly). Elsewhere — or when
    // CAJETA_SIMD_SCALAR_FALLBACK=1 forces it — a 16-lane extract/select
    // chain with identical semantics. Table and indices are <16 x i8>.
    inline llvm::Value* tableLookup(llvm::IRBuilderBase& b, llvm::Module* m,
                                    llvm::Value* table, llvm::Value* indices,
                                    bool forceScalar) {
        llvm::LLVMContext& ctx = b.getContext();
        auto* i8 = llvm::Type::getInt8Ty(ctx);
        auto* v16 = llvm::FixedVectorType::get(i8, 16);
        const llvm::Triple& triple = m->getTargetTriple();
        if (!forceScalar && (triple.getArch() == llvm::Triple::x86_64
                             || triple.getArch() == llvm::Triple::x86)) {
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                m, llvm::Intrinsic::x86_ssse3_pshuf_b_128);
            return b.CreateCall(fn, {table, indices}, "tbl.pshufb");
        }
        if (!forceScalar && triple.isAArch64()) {
            llvm::Value* masked = b.CreateAnd(indices,
                llvm::ConstantVector::getSplat(
                    llvm::ElementCount::getFixed(16),
                    llvm::ConstantInt::get(i8, 0x8F)), "tbl.maskidx");
            llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                m, llvm::Intrinsic::aarch64_neon_tbl1, {v16});
            return b.CreateCall(fn, {table, masked}, "tbl.neon");
        }
        // Scalar fallback: per lane, select(high-bit, 0, table[idx & 15]).
        llvm::Value* out = llvm::UndefValue::get(v16);
        auto* i32 = llvm::Type::getInt32Ty(ctx);
        for (unsigned i = 0; i < 16; ++i) {
            llvm::Value* idx = b.CreateExtractElement(indices,
                llvm::ConstantInt::get(i32, i), "tbl.idx");
            llvm::Value* lo = b.CreateAnd(idx,
                llvm::ConstantInt::get(i8, 15), "tbl.lo");
            llvm::Value* elem = b.CreateExtractElement(table,
                b.CreateZExt(lo, i32, "tbl.lo32"), "tbl.elem");
            llvm::Value* hi = b.CreateICmpSLT(idx,
                llvm::ConstantInt::get(i8, 0), "tbl.hibit");
            llvm::Value* sel = b.CreateSelect(hi,
                llvm::ConstantInt::get(i8, 0), elem, "tbl.sel");
            out = b.CreateInsertElement(out, sel,
                llvm::ConstantInt::get(i32, i), "tbl.out");
        }
        return out;
    }

    // widen — one rung up the integer ladder over one half of the lanes:
    // <N x iW> -> <N/2 x i2W>, sext when the element is signed, zext when
    // unsigned. `lo` picks lanes [0, N/2) vs [N/2, N).
    inline llvm::Value* widenHalf(llvm::IRBuilderBase& b, llvm::Value* v,
                                  bool lo, bool isSigned) {
        auto* vt = llvm::cast<llvm::FixedVectorType>(v->getType());
        unsigned n = vt->getNumElements();
        unsigned half = n / 2;
        std::vector<int> lanes;
        for (unsigned i = 0; i < half; ++i) {
            lanes.push_back((int) (lo ? i : half + i));
        }
        llvm::Value* sub = b.CreateShuffleVector(v, lanes, "widen.half");
        auto* wide = llvm::FixedVectorType::get(
            llvm::IntegerType::get(b.getContext(),
                vt->getElementType()->getIntegerBitWidth() * 2), half);
        return isSigned ? b.CreateSExt(sub, wide, "widen.sext")
                        : b.CreateZExt(sub, wide, "widen.zext");
    }

    // narrow — one rung down over a PAIR: two <N x iW> -> <2N x iW/2>,
    // truncating each lane (modular, the ladder inverse of widenHalf: the
    // receiver's lanes land first). Bit-exact round trip for values that fit.
    inline llvm::Value* narrowPair(llvm::IRBuilderBase& b, llvm::Value* loV,
                                   llvm::Value* hiV) {
        auto* vt = llvm::cast<llvm::FixedVectorType>(loV->getType());
        unsigned n = vt->getNumElements();
        auto* nar = llvm::FixedVectorType::get(
            llvm::IntegerType::get(b.getContext(),
                vt->getElementType()->getIntegerBitWidth() / 2), n);
        llvm::Value* a = b.CreateTrunc(loV, nar, "narrow.lo");
        llvm::Value* c = b.CreateTrunc(hiV, nar, "narrow.hi");
        std::vector<int> lanes;
        for (unsigned i = 0; i < 2 * n; ++i) lanes.push_back((int) i);
        return b.CreateShuffleVector(a, c, lanes, "narrow.cat");
    }

    // int<->float convert and lane bitcast (17.1.4).
    inline llvm::Value* convertToF32(llvm::IRBuilderBase& b, llvm::Value* v,
                                     bool isSigned) {
        auto* vt = llvm::cast<llvm::FixedVectorType>(v->getType());
        auto* fv = llvm::FixedVectorType::get(
            llvm::Type::getFloatTy(b.getContext()), vt->getNumElements());
        return isSigned ? b.CreateSIToFP(v, fv, "conv.sitofp")
                        : b.CreateUIToFP(v, fv, "conv.uitofp");
    }

    inline llvm::Value* convertToI32(llvm::IRBuilderBase& b, llvm::Value* v) {
        auto* vt = llvm::cast<llvm::FixedVectorType>(v->getType());
        auto* iv = llvm::FixedVectorType::get(
            llvm::Type::getInt32Ty(b.getContext()), vt->getNumElements());
        return b.CreateFPToSI(v, iv, "conv.fptosi");
    }

    inline llvm::Value* bitcastLanes(llvm::IRBuilderBase& b, llvm::Value* v,
                                     llvm::Type* elemTo) {
        auto* vt = llvm::cast<llvm::FixedVectorType>(v->getType());
        auto* tv = llvm::FixedVectorType::get(elemTo, vt->getNumElements());
        return b.CreateBitCast(v, tv, "vec.bitcast");
    }

} // namespace vecops
} // namespace cajeta
