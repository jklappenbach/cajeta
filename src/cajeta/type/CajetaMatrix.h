//
// CajetaMatrix — a fixed-shape numeric matrix, `Matrix<T, R, C>`, lowering to a
// flat row-major LLVM `<R*C x T>` (element (r,c) lives at lane r*C+c).
//
// A VALUE type (like a primitive / Vector): passed by value, no heap, no vtable,
// no drop chain. The element type T is a non-bool numeric primitive; R and C are
// positive compile-time integer constants. Synthesized on demand and cached in
// CajetaType::canonicalMap under the key "Matrix<T,R,C>" so every reference to
// the same shape resolves to one CajetaType (and so it participates in the
// resetGlobals lifecycle).
//
// Modeled directly on CajetaVector: it derives from CajetaType (not CajetaClass)
// — the matrix has no struct body, fields, or methods of its own. Construction,
// 2D element access (m[r][c]), element-wise arithmetic, scalar scale, `*` =
// matrix multiply, and the transpose/identity/row/col/hadamard methods are all
// lowered as compiler intrinsics (see the host expression codegen and the device
// KernelLowering walker), delegating to the shared `matops` helper.
//
// The HYBRID model (plans/fluttering-sparking-lantern.md): the user-facing
// surface is the declared `Matrix<T, uint32 R, uint32 C>` class in
// runtime/src/cajeta/math/Matrix.cajeta (so the operator/method DECLARATIONS are
// library-owned and exercise the operator-declaration + type-resolution path),
// while a `Matrix<...>` type REFERENCE resolves to this flat CajetaMatrix
// representation and codegen intercepts the operators — identical generated code
// to a pure-intrinsic vector.
//

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

        // `<R*C x T>`. T's llvm scalar type must already exist (primitives are
        // registered in CajetaType::init before any matrix is resolved).
        llvm::Type* getLlvmType() override;

        // Canonical name, e.g. "Matrix<float32,2,3>".
        static string canonicalName(const CajetaTypePtr& elementType,
                                    uint32_t rows, uint32_t cols);

        // Get-or-create the Matrix<elem,R,C> instance, cached in canonicalMap.
        static shared_ptr<CajetaMatrix> getOrCreate(CajetaModulePtr module,
                                                    CajetaTypePtr elementType,
                                                    uint32_t rows, uint32_t cols);

        // Validate the semantic constraints (element is a non-bool numeric
        // primitive; R and C are positive constants) then getOrCreate. Shared by
        // every site that materializes a Matrix type (CajetaType::fromContext
        // type positions and NewExpression construction), so the diagnostics are
        // identical. Throws CAJETA_ERROR_MATRIX_ELEMENT_TYPE /
        // CAJETA_ERROR_MATRIX_DIMENSIONS.
        static shared_ptr<CajetaMatrix> validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType,
            int64_t rows, int64_t cols);
    };
    typedef shared_ptr<CajetaMatrix> CajetaMatrixPtr;
}
