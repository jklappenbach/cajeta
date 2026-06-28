//
// CajetaVector — see header.
//

#include "CajetaVector.h"

#include "llvm/IR/DerivedTypes.h"

#include "../error/Exception.h"

namespace cajeta {

    shared_ptr<CajetaVector> CajetaVector::validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType, int64_t lanes) {
        if (!elementType) {
            throw Exception("Vector element type did not resolve",
                            "CAJETA_ERROR_VECTOR_ELEMENT_TYPE");
        }
        CajetaTypeFlags ef = elementType->getTypeFlags();
        llvm::Type* elemLlvm = elementType->getLlvmType();
        bool isBool = elemLlvm && elemLlvm->isIntegerTy(1);
        if (!(ef & NUMBER_FLAG) || !(ef & PRIMITIVE_FLAG) || isBool) {
            throw Exception(
                "Vector element type must be a non-bool numeric primitive "
                "(got '" + elementType->toCanonical() + "')",
                "CAJETA_ERROR_VECTOR_ELEMENT_TYPE");
        }
        if (lanes <= 0) {
            throw Exception(
                "Vector length N must be a positive integer constant (got "
                + std::to_string(lanes) + ")",
                "CAJETA_ERROR_VECTOR_LENGTH");
        }
        return getOrCreate(module, elementType, (uint32_t) lanes);
    }

    string CajetaVector::canonicalName(const CajetaTypePtr& elementType,
                                       uint32_t lanes) {
        return string("Vector<") + elementType->toCanonical() + ","
            + std::to_string(lanes) + ">";
    }

    CajetaVector::CajetaVector(CajetaTypePtr elementType, uint32_t lanes)
        : elementType(elementType), lanes(lanes) {
        qName = QualifiedName::getOrCreate(canonicalName(elementType, lanes));
        canonical = qName->toCanonical();
        // By-value (PRIMITIVE_FLAG so the kernel-arg marshaller passes it like
        // a scalar) and tagged VECTOR_FLAG so codegen recognizes it. Not
        // NUMBER_FLAG — a vector is not a scalar number; element-wise
        // arithmetic is handled by the dedicated vector path. The element's
        // SIGNED_FLAG is inherited so integer-vector division picks SDiv/UDiv
        // correctly.
        typeFlags = VECTOR_FLAG | PRIMITIVE_FLAG
            | (elementType->getTypeFlags() & SIGNED_FLAG);
        llvmType = nullptr;
    }

    llvm::Type* CajetaVector::getLlvmType() {
        // frozen-aware lazy-create (threadsafe U6.2): read/write via the base
        // accessor so a frozen shared instance binds per-thread, a normal one
        // inline (identical behavior pre-freeze).
        if (llvm::Type* cur = CajetaType::getLlvmType()) return cur;
        llvm::Type* t = llvm::FixedVectorType::get(elementType->getLlvmType(), lanes);
        setLlvmType(t);
        return t;
    }

    shared_ptr<CajetaVector> CajetaVector::getOrCreate(CajetaModulePtr /*module*/,
                                                       CajetaTypePtr elementType,
                                                       uint32_t lanes) {
        string key = canonicalName(elementType, lanes);
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find(key);
        if (it != cmap.end()) {
            if (auto v = dynamic_pointer_cast<CajetaVector>(it->second)) {
                return v;
            }
        }
        auto v = make_shared<CajetaVector>(elementType, lanes);
        cmap[key] = v;
        return v;
    }

}
