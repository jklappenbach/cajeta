//
// CajetaQuaternion — a unit quaternion `Quaternion<T>`, lowering to a flat LLVM
// `<4 x T>` laid out (w, x, y, z) with w the scalar (real) part and (x,y,z) the
// vector part: q = w + x*i + y*j + z*k.
//
// A VALUE type (like a primitive / Vector / Matrix): passed by value, no heap,
// no vtable, no drop chain. The element type T is a floating-point primitive
// (rotations are float math). Synthesized on demand and cached in
// CajetaType::canonicalMap under "Quaternion<T>" so every reference to the same
// element type resolves to one CajetaType.
//
// Modeled on CajetaVector/CajetaMatrix: it derives from CajetaType (not
// CajetaClass) — no struct body, fields, or methods of its own. Construction,
// `*` = Hamilton product (Quaternion*Quaternion) / vector rotation
// (Quaternion*Vector<T,3>), `+ -` element-wise, and the normalize / conjugate /
// length / dot / slerp methods are lowered as compiler intrinsics (host
// expression codegen + device KernelLowering), delegating to the shared `quatops`
// helper.
//

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

        // Validate (element is a floating-point primitive) then getOrCreate.
        // Throws CAJETA_ERROR_QUATERNION_ELEMENT_TYPE.
        static shared_ptr<CajetaQuaternion> validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType);
    };
    typedef shared_ptr<CajetaQuaternion> CajetaQuaternionPtr;
}
