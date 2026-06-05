//
// CajetaQuaternion — see header.
//

#include "CajetaQuaternion.h"

#include "llvm/IR/DerivedTypes.h"

#include "../error/Exception.h"

namespace cajeta {

    shared_ptr<CajetaQuaternion> CajetaQuaternion::validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType) {
        if (!elementType) {
            throw Exception("Quaternion element type did not resolve",
                            "CAJETA_ERROR_QUATERNION_ELEMENT_TYPE");
        }
        CajetaTypeFlags ef = elementType->getTypeFlags();
        if (!(ef & FLOAT_FLAG) || !(ef & PRIMITIVE_FLAG)) {
            throw Exception(
                "Quaternion element type must be a floating-point primitive "
                "(got '" + elementType->toCanonical() + "')",
                "CAJETA_ERROR_QUATERNION_ELEMENT_TYPE");
        }
        return getOrCreate(module, elementType);
    }

    string CajetaQuaternion::canonicalName(const CajetaTypePtr& elementType) {
        return string("Quaternion<") + elementType->toCanonical() + ">";
    }

    CajetaQuaternion::CajetaQuaternion(CajetaTypePtr elementType)
        : elementType(elementType) {
        qName = QualifiedName::getOrCreate(canonicalName(elementType));
        canonical = qName->toCanonical();
        // By-value (PRIMITIVE_FLAG so the kernel-arg marshaller passes it like a
        // vector) and tagged QUATERNION_FLAG so codegen recognizes it. SIGNED is
        // inherited (float is signed) though it isn't consulted for quaternions.
        typeFlags = QUATERNION_FLAG | PRIMITIVE_FLAG
            | (elementType->getTypeFlags() & SIGNED_FLAG);
        llvmType = nullptr;
    }

    llvm::Type* CajetaQuaternion::getLlvmType() {
        if (!llvmType) {
            llvmType = llvm::FixedVectorType::get(elementType->getLlvmType(),
                                                  LANES);
        }
        return llvmType;
    }

    shared_ptr<CajetaQuaternion> CajetaQuaternion::getOrCreate(
            CajetaModulePtr /*module*/, CajetaTypePtr elementType) {
        string key = canonicalName(elementType);
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find(key);
        if (it != cmap.end()) {
            if (auto q = dynamic_pointer_cast<CajetaQuaternion>(it->second)) {
                return q;
            }
        }
        auto q = make_shared<CajetaQuaternion>(elementType);
        cmap[key] = q;
        return q;
    }

}
