// CajetaQuaternion — the unit quaternion `Quaternion<T>`, lowering to a flat LLVM
// `<4 x T>` laid out (w, x, y, z), w scalar: q = w + x*i + y*j + z*k. A VALUE type
// deriving from CajetaType; every operation on it is lowered as an intrinsic.

#pragma once

#include "CajetaType.h"

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaQuaternion : public CajetaType {
    private:
        CajetaTypePtr elementType;
    public:
        explicit CajetaQuaternion(CajetaTypePtr elementType);

        CajetaTypePtr getElementType() const { return elementType; }
        // A quaternion is always 4 lanes (w, x, y, z).
        static constexpr uint32_t LANES = 4;

        // `<4 x T>`.
        llvm::Type* getLlvmType() override;

        // Canonical name, e.g. "Quaternion<float32>".
        static string canonicalName(const CajetaTypePtr& elementType);

        // Get-or-create the Quaternion<elem> instance, cached in canonicalMap.
        static shared_ptr<CajetaQuaternion> getOrCreate(CajetaModulePtr module,
                                                        CajetaTypePtr elementType);

        // getOrCreate after checking the element is floating-point; else throws
        // CAJETA_ERROR_QUATERNION_ELEMENT_TYPE.
        static shared_ptr<CajetaQuaternion> validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType);
    };
    typedef shared_ptr<CajetaQuaternion> CajetaQuaternionPtr;
}
