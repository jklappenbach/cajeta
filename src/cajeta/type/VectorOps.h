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

} // namespace vecops
} // namespace cajeta
