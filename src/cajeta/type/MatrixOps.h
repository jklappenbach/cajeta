// Shared IR builders for `Matrix<T, R, C>`, a flat row-major `<R*C x T>` value
// with element (r,c) at lane r*C+c. R, C and K are compile-time; r and c may be
// runtime i32. IRBuilderBase& so the host and device builders share one path.

#pragma once

#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"

#include "VectorOps.h"

namespace cajeta {
namespace matops {

    inline llvm::Value* buildMatrix(llvm::IRBuilderBase& b, llvm::Type* elemTy,
                                    unsigned rows, unsigned cols,
                                    const std::vector<llvm::Value*>& elems) {
        return vecops::buildVector(b, elemTy, rows * cols, elems);
    }

    // Flat lane index of element (r, c): r*cols + c.
    inline llvm::Value* flatLane(llvm::IRBuilderBase& b, unsigned cols,
                                 llvm::Value* r, llvm::Value* c) {
        llvm::Value* rc = b.CreateMul(r, b.getInt32(cols), "mat.rowbase");
        return b.CreateAdd(rc, c, "mat.lane");
    }

    inline llvm::Value* getElement(llvm::IRBuilderBase& b, llvm::Value* m,
                                   unsigned /*rows*/, unsigned cols,
                                   llvm::Value* r, llvm::Value* c) {
        return vecops::extractLane(b, m, flatLane(b, cols, r, c));
    }

    inline llvm::Value* setElement(llvm::IRBuilderBase& b, llvm::Value* m,
                                   unsigned /*rows*/, unsigned cols,
                                   llvm::Value* r, llvm::Value* c,
                                   llvm::Value* val) {
        return vecops::insertLane(b, m, val, flatLane(b, cols, r, c));
    }

    // Row r -> `<C x T>`, gathered lane by lane so a runtime r works.
    inline llvm::Value* row(llvm::IRBuilderBase& b, llvm::Value* m,
                            unsigned /*rows*/, unsigned cols, llvm::Value* r) {
        llvm::Type* elemTy =
            llvm::cast<llvm::FixedVectorType>(m->getType())->getElementType();
        llvm::Value* base = b.CreateMul(r, b.getInt32(cols), "mat.rowbase");
        auto* rowTy = llvm::FixedVectorType::get(elemTy, cols);
        llvm::Value* acc = llvm::UndefValue::get(rowTy);
        for (unsigned j = 0; j < cols; ++j) {
            llvm::Value* lane = b.CreateAdd(base, b.getInt32(j), "mat.row.src");
            llvm::Value* e = vecops::extractLane(b, m, lane);
            acc = vecops::insertLane(b, acc, e, j);
        }
        return acc;
    }

    inline llvm::Value* col(llvm::IRBuilderBase& b, llvm::Value* m,
                            unsigned rows, unsigned cols, llvm::Value* c) {
        llvm::Type* elemTy =
            llvm::cast<llvm::FixedVectorType>(m->getType())->getElementType();
        auto* colTy = llvm::FixedVectorType::get(elemTy, rows);
        llvm::Value* acc = llvm::UndefValue::get(colTy);
        for (unsigned i = 0; i < rows; ++i) {
            llvm::Value* lane =
                b.CreateAdd(c, b.getInt32(i * cols), "mat.col.src");
            llvm::Value* e = vecops::extractLane(b, m, lane);
            acc = vecops::insertLane(b, acc, e, i);
        }
        return acc;
    }

    // R x C -> C x R: result lane j*R+i takes source lane i*C+j.
    inline llvm::Value* transpose(llvm::IRBuilderBase& b, llvm::Value* m,
                                  unsigned rows, unsigned cols) {
        llvm::Type* elemTy =
            llvm::cast<llvm::FixedVectorType>(m->getType())->getElementType();
        auto* outTy = llvm::FixedVectorType::get(elemTy, rows * cols);
        llvm::Value* acc = llvm::UndefValue::get(outTy);
        for (unsigned i = 0; i < rows; ++i) {
            for (unsigned j = 0; j < cols; ++j) {
                llvm::Value* e = vecops::extractLane(b, m, i * cols + j);
                acc = vecops::insertLane(b, acc, e, j * rows + i);
            }
        }
        return acc;
    }

    // N x N identity as a constant `<N*N x T>`.
    inline llvm::Value* identity(llvm::IRBuilderBase& b, llvm::Type* elemTy,
                                 unsigned n) {
        std::vector<llvm::Constant*> cs;
        cs.reserve(n * n);
        llvm::Constant* one = elemTy->isFloatingPointTy()
            ? (llvm::Constant*) llvm::ConstantFP::get(elemTy, 1.0)
            : (llvm::Constant*) llvm::ConstantInt::get(elemTy, 1);
        llvm::Constant* zero = elemTy->isFloatingPointTy()
            ? (llvm::Constant*) llvm::ConstantFP::get(elemTy, 0.0)
            : (llvm::Constant*) llvm::ConstantInt::get(elemTy, 0);
        for (unsigned i = 0; i < n; ++i)
            for (unsigned j = 0; j < n; ++j)
                cs.push_back(i == j ? one : zero);
        (void) b;
        return llvm::ConstantVector::get(cs);
    }

    inline llvm::Value* hadamard(llvm::IRBuilderBase& b, llvm::Value* a,
                                 llvm::Value* c, bool isFloat) {
        return isFloat ? b.CreateFMul(a, c, "mat.hadamard")
                       : b.CreateMul(a, c, "mat.hadamard");
    }

    inline llvm::Value* scale(llvm::IRBuilderBase& b, llvm::Value* m,
                              llvm::Value* scalar, bool isFloat) {
        unsigned lanes =
            llvm::cast<llvm::FixedVectorType>(m->getType())->getNumElements();
        llvm::Value* s = vecops::splat(b, scalar, lanes);
        return isFloat ? b.CreateFMul(m, s, "mat.scale")
                       : b.CreateMul(m, s, "mat.scale");
    }

    // A (R x K) * B (K x C) -> R x C, unrolled at compile time.
    inline llvm::Value* matmul(llvm::IRBuilderBase& b, llvm::Value* a,
                               unsigned rows, unsigned inner, llvm::Value* c,
                               unsigned cols, bool isFloat) {
        llvm::Type* elemTy =
            llvm::cast<llvm::FixedVectorType>(a->getType())->getElementType();
        auto* outTy = llvm::FixedVectorType::get(elemTy, rows * cols);
        llvm::Value* acc = llvm::UndefValue::get(outTy);
        for (unsigned i = 0; i < rows; ++i) {
            for (unsigned j = 0; j < cols; ++j) {
                llvm::Value* sum = nullptr;
                for (unsigned k = 0; k < inner; ++k) {
                    llvm::Value* av = vecops::extractLane(b, a, i * inner + k);
                    llvm::Value* bv = vecops::extractLane(b, c, k * cols + j);
                    llvm::Value* prod = isFloat ? b.CreateFMul(av, bv, "mat.mul")
                                                : b.CreateMul(av, bv, "mat.mul");
                    if (!sum) sum = prod;
                    else sum = isFloat ? b.CreateFAdd(sum, prod, "mat.acc")
                                       : b.CreateAdd(sum, prod, "mat.acc");
                }
                acc = vecops::insertLane(b, acc, sum, i * cols + j);
            }
        }
        return acc;
    }

    // M (R x C) * v (`<C x T>`) -> `<R x T>`.
    inline llvm::Value* matVec(llvm::IRBuilderBase& b, llvm::Value* m,
                               unsigned rows, unsigned cols, llvm::Value* v,
                               bool isFloat) {
        llvm::Type* elemTy =
            llvm::cast<llvm::FixedVectorType>(m->getType())->getElementType();
        auto* outTy = llvm::FixedVectorType::get(elemTy, rows);
        llvm::Value* acc = llvm::UndefValue::get(outTy);
        for (unsigned i = 0; i < rows; ++i) {
            llvm::Value* sum = nullptr;
            for (unsigned j = 0; j < cols; ++j) {
                llvm::Value* mv = vecops::extractLane(b, m, i * cols + j);
                llvm::Value* vv = vecops::extractLane(b, v, j);
                llvm::Value* prod = isFloat ? b.CreateFMul(mv, vv, "mat.vmul")
                                            : b.CreateMul(mv, vv, "mat.vmul");
                if (!sum) sum = prod;
                else sum = isFloat ? b.CreateFAdd(sum, prod, "mat.vacc")
                                   : b.CreateAdd(sum, prod, "mat.vacc");
            }
            acc = vecops::insertLane(b, acc, sum, i);
        }
        return acc;
    }

    // Determinant of row-major elements `e`, by cofactor expansion along row 0.
    inline llvm::Value* detElems(llvm::IRBuilderBase& b,
                                 const std::vector<llvm::Value*>& e, unsigned n) {
        if (n == 1) return e[0];
        if (n == 2)
            return b.CreateFSub(b.CreateFMul(e[0], e[3]),
                                b.CreateFMul(e[1], e[2]), "det2");
        llvm::Value* acc = nullptr;
        for (unsigned j = 0; j < n; ++j) {
            std::vector<llvm::Value*> minor;       // drop row 0, col j
            minor.reserve((n - 1) * (n - 1));
            for (unsigned r = 1; r < n; ++r)
                for (unsigned c = 0; c < n; ++c)
                    if (c != j) minor.push_back(e[r * n + c]);
            llvm::Value* term = b.CreateFMul(e[j], detElems(b, minor, n - 1));
            if (j & 1u) term = b.CreateFNeg(term);
            acc = acc ? b.CreateFAdd(acc, term, "det.acc") : term;
        }
        return acc;
    }

    // Scalar determinant of a square float matrix, n in {2,3,4}.
    inline llvm::Value* determinant(llvm::IRBuilderBase& b, llvm::Value* m,
                                    unsigned n) {
        std::vector<llvm::Value*> e;
        e.reserve(n * n);
        for (unsigned i = 0; i < n * n; ++i)
            e.push_back(vecops::extractLane(b, m, i));
        return detElems(b, e, n);
    }

    // adjugate(m) / det(m); a singular m yields inf/nan rather than an error.
    inline llvm::Value* inverse(llvm::IRBuilderBase& b, llvm::Value* m,
                                unsigned n) {
        std::vector<llvm::Value*> e;
        e.reserve(n * n);
        for (unsigned i = 0; i < n * n; ++i)
            e.push_back(vecops::extractLane(b, m, i));
        llvm::Type* elemTy = e[0]->getType();
        llvm::Value* invDet = b.CreateFDiv(
            llvm::ConstantFP::get(elemTy, 1.0), detElems(b, e, n), "inv.det");
        auto* outTy = llvm::FixedVectorType::get(elemTy, n * n);
        llvm::Value* acc = llvm::UndefValue::get(outTy);
        for (unsigned i = 0; i < n; ++i) {
            for (unsigned j = 0; j < n; ++j) {
                std::vector<llvm::Value*> minor;   // drop row j, col i (adjugate)
                minor.reserve((n - 1) * (n - 1));
                for (unsigned r = 0; r < n; ++r)
                    for (unsigned c = 0; c < n; ++c)
                        if (r != j && c != i) minor.push_back(e[r * n + c]);
                llvm::Value* cof = detElems(b, minor, n - 1);
                if ((i + j) & 1u) cof = b.CreateFNeg(cof);
                acc = vecops::insertLane(
                    b, acc, b.CreateFMul(cof, invDet, "inv.elem"), i * n + j);
            }
        }
        return acc;
    }

} // namespace matops
} // namespace cajeta
