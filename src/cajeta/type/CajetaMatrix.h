// `Matrix<T, R, C>`: a fixed-shape numeric VALUE type lowering to a flat
// row-major `<R*C x T>`, element (r,c) at lane r*C+c. Its operators are declared
// in Matrix.cajeta but intercepted by codegen as intrinsics, never called.

#pragma once

#include "CajetaType.h"

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaMatrix : public CajetaType {
    private:
        CajetaTypePtr elementType;
        uint32_t rows;
        uint32_t cols;
    public:
        CajetaMatrix(CajetaTypePtr elementType, uint32_t rows, uint32_t cols);

        CajetaTypePtr getElementType() const { return elementType; }
        uint32_t getRows() const { return rows; }
        uint32_t getCols() const { return cols; }
        // Flat lane count R*C — the width of the underlying `<R*C x T>`.
        uint32_t getLanes() const { return rows * cols; }

        // `<R*C x T>`. T's llvm scalar type must already be registered, which
        // CajetaType::init guarantees before any matrix can be resolved.
        llvm::Type* getLlvmType() override;

        // Canonical name, e.g. "Matrix<float32,2,3>".
        static string canonicalName(const CajetaTypePtr& elementType,
                                    uint32_t rows, uint32_t cols);

        // Get-or-create the Matrix<elem,R,C> instance, cached in canonicalMap.
        static shared_ptr<CajetaMatrix> getOrCreate(CajetaModulePtr module,
                                                    CajetaTypePtr elementType,
                                                    uint32_t rows, uint32_t cols);

        // Check that the element is a non-bool numeric primitive and R, C are
        // positive, then getOrCreate. Every site that materializes a Matrix goes
        // through here. Throws MATRIX_ELEMENT_TYPE / MATRIX_DIMENSIONS.
        static shared_ptr<CajetaMatrix> validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType,
            int64_t rows, int64_t cols);
    };
    typedef shared_ptr<CajetaMatrix> CajetaMatrixPtr;
}
