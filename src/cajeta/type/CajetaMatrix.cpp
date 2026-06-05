//
// CajetaMatrix — see header.
//

#include "CajetaMatrix.h"

#include "llvm/IR/DerivedTypes.h"

#include "../error/Exception.h"

namespace cajeta {

    shared_ptr<CajetaMatrix> CajetaMatrix::validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType,
            int64_t rows, int64_t cols) {
        if (!elementType) {
            throw Exception("Matrix element type did not resolve",
                            "CAJETA_ERROR_MATRIX_ELEMENT_TYPE");
        }
        CajetaTypeFlags ef = elementType->getTypeFlags();
        llvm::Type* elemLlvm = elementType->getLlvmType();
        bool isBool = elemLlvm && elemLlvm->isIntegerTy(1);
        if (!(ef & NUMBER_FLAG) || !(ef & PRIMITIVE_FLAG) || isBool) {
            throw Exception(
                "Matrix element type must be a non-bool numeric primitive "
                "(got '" + elementType->toCanonical() + "')",
                "CAJETA_ERROR_MATRIX_ELEMENT_TYPE");
        }
        if (rows <= 0 || cols <= 0) {
            throw Exception(
                "Matrix dimensions R and C must be positive integer constants "
                "(got " + std::to_string(rows) + "x" + std::to_string(cols)
                + ")",
                "CAJETA_ERROR_MATRIX_DIMENSIONS");
        }
        return getOrCreate(module, elementType, (uint32_t) rows,
                           (uint32_t) cols);
    }

    string CajetaMatrix::canonicalName(const CajetaTypePtr& elementType,
                                       uint32_t rows, uint32_t cols) {
        return string("Matrix<") + elementType->toCanonical() + ","
            + std::to_string(rows) + "," + std::to_string(cols) + ">";
    }

    CajetaMatrix::CajetaMatrix(CajetaTypePtr elementType, uint32_t rows,
                               uint32_t cols)
        : elementType(elementType), rows(rows), cols(cols) {
        qName = QualifiedName::getOrCreate(canonicalName(elementType, rows, cols));
        canonical = qName->toCanonical();
        // By-value (PRIMITIVE_FLAG so the kernel-arg marshaller passes it like a
        // scalar/vector) and tagged MATRIX_FLAG so codegen recognizes it. Not
        // NUMBER_FLAG — a matrix is not a scalar number; arithmetic is handled by
        // the dedicated matrix path. The element's SIGNED_FLAG is inherited so
        // integer-matrix division picks SDiv/UDiv correctly.
        typeFlags = MATRIX_FLAG | PRIMITIVE_FLAG
            | (elementType->getTypeFlags() & SIGNED_FLAG);
        llvmType = nullptr;
    }

    llvm::Type* CajetaMatrix::getLlvmType() {
        if (!llvmType) {
            llvmType = llvm::FixedVectorType::get(elementType->getLlvmType(),
                                                  rows * cols);
        }
        return llvmType;
    }

    shared_ptr<CajetaMatrix> CajetaMatrix::getOrCreate(CajetaModulePtr /*module*/,
                                                       CajetaTypePtr elementType,
                                                       uint32_t rows,
                                                       uint32_t cols) {
        string key = canonicalName(elementType, rows, cols);
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find(key);
        if (it != cmap.end()) {
            if (auto m = dynamic_pointer_cast<CajetaMatrix>(it->second)) {
                return m;
            }
        }
        auto m = make_shared<CajetaMatrix>(elementType, rows, cols);
        cmap[key] = m;
        return m;
    }

}
