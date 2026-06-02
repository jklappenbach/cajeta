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

    // Whole identifier -> lane index. v1 supports single-letter components only
    // (no multi-component swizzles like `.xyz`); returns -1 otherwise.
    inline int laneForComponentName(const std::string& name) {
        if (name.size() != 1) return -1;
        return laneForComponent(name[0]);
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

} // namespace vecops
} // namespace cajeta
