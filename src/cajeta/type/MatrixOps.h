//
// MatrixOps — shared LLVM-IR builders for the Matrix<T, R, C> value type (B1).
//
// A Matrix is a flat row-major `<R*C x T>` value (element (r,c) at lane r*C+c).
// Both codegen paths emit identical matrix IR through these helpers: the host
// expression codegen and the device kernel walker (xpu/lowering/KernelLowering).
// The flat lane mechanics delegate to `vecops` (extract/insert/build/splat) so
// the Matrix path can never drift from the Vector path it is built on.
//
// All helpers take an llvm::IRBuilderBase& so they serve both the host
// IRBuilder<> and the device builder. R, C, K are compile-time dimensions;
// r/c element indices may be runtime i32 values (the matrix lives in registers,
// but lane math is plain integer arithmetic). Row-major throughout.
//

#pragma once

#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"

#include "VectorOps.h"

namespace cajeta {
namespace matops {

    // Build a row-major `<R*C x T>` from R*C scalar elements (row 0 first).
    inline llvm::Value* buildMatrix(llvm::IRBuilderBase& b, llvm::Type* elemTy,
                                    unsigned rows, unsigned cols,
                                    const std::vector<llvm::Value*>& elems) {
        return vecops::buildVector(b, elemTy, rows * cols, elems);
    }

    // Flat lane index of element (r, c) in a row-major R x C matrix: r*C + c.
    // r, c are i32 runtime values; cols is the compile-time column count.
    inline llvm::Value* flatLane(llvm::IRBuilderBase& b, unsigned cols,
                                 llvm::Value* r, llvm::Value* c) {
        llvm::Value* rc = b.CreateMul(r, b.getInt32(cols), "mat.rowbase");
        return b.CreateAdd(rc, c, "mat.lane");
    }

    // m[r][c] read -> scalar element.
    inline llvm::Value* getElement(llvm::IRBuilderBase& b, llvm::Value* m,
                                   unsigned /*rows*/, unsigned cols,
                                   llvm::Value* r, llvm::Value* c) {
        return vecops::extractLane(b, m, flatLane(b, cols, r, c));
    }

    // m[r][c] = val -> updated matrix value (insertelement at flat lane).
    inline llvm::Value* setElement(llvm::IRBuilderBase& b, llvm::Value* m,
                                   unsigned /*rows*/, unsigned cols,
                                   llvm::Value* r, llvm::Value* c,
                                   llvm::Value* val) {
        return vecops::insertLane(b, m, val, flatLane(b, cols, r, c));
    }

    // Row r -> `<C x T>` (flat lanes [r*C, r*C+C)). r may be runtime; the C
    // lanes are gathered by extract/insert so a dynamic row works.
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

    // Column c -> `<R x T>` (flat lanes c, c+C, ..., c+(R-1)*C). c may be runtime.
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

    // Transpose an R x C matrix -> a C x R matrix (`<C*R x T>`): result element
    // (j, i) = source element (i, j), i.e. result lane j*R+i = source lane i*C+j.
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

    // N x N identity matrix as a constant `<N*N x T>` (1 on the diagonal).
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

    // Element-wise (Hadamard) product of two same-shape flat matrices.
    inline llvm::Value* hadamard(llvm::IRBuilderBase& b, llvm::Value* a,
                                 llvm::Value* c, bool isFloat) {
        return isFloat ? b.CreateFMul(a, c, "mat.hadamard")
                       : b.CreateMul(a, c, "mat.hadamard");
    }

    // Scale every element by a scalar -> same-shape matrix.
    inline llvm::Value* scale(llvm::IRBuilderBase& b, llvm::Value* m,
                              llvm::Value* scalar, bool isFloat) {
        unsigned lanes =
            llvm::cast<llvm::FixedVectorType>(m->getType())->getNumElements();
        llvm::Value* s = vecops::splat(b, scalar, lanes);
        return isFloat ? b.CreateFMul(m, s, "mat.scale")
                       : b.CreateMul(m, s, "mat.scale");
    }

    // Matrix multiply: A (R x K) * B (K x C) -> (R x C). C(i,j) = sum_k A(i,k)*B(k,j).
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

    // Matrix-vector: M (R x C) * v (`<C x T>`) -> `<R x T>`. out(i) = sum_j M(i,j)*v(j).
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

} // namespace matops
} // namespace cajeta
