//
// CajetaVector — a fixed-width numeric vector, `Vector<T, N>`, lowering to
// LLVM `<N x T>`.
//
// A VALUE type (like a primitive): passed by value, no heap, no vtable, no
// drop chain. The element type T is a non-bool numeric primitive; N is a
// positive compile-time integer constant. Synthesized on demand and cached in
// CajetaType::canonicalMap under the key "Vector<T,N>" so every reference to
// the same shape resolves to one CajetaType (and so it participates in the
// resetGlobals lifecycle, unlike a private static cache).
//
// Modeled on CajetaTask/CajetaArray as a synthesized type living outside the
// user's class space, but — unlike those — it derives from CajetaType, not
// CajetaClass: a vector has no struct body, fields, or methods of its own.
// Construction, component/index access, arithmetic, and dot/length/normalize
// are lowered as compiler intrinsics (see the host expression codegen and the
// device KernelLowering walker).
//

#pragma once

#include "CajetaType.h"

namespace cajeta {
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaVector : public CajetaType {
    private:
        CajetaTypePtr elementType;
        uint32_t lanes;
    public:
        CajetaVector(CajetaTypePtr elementType, uint32_t lanes);

        CajetaTypePtr getElementType() const { return elementType; }
        uint32_t getLanes() const { return lanes; }

        // `<N x T>`. T's llvm scalar type must already exist (primitives are
        // registered in CajetaType::init before any vector is resolved).
        llvm::Type* getLlvmType() override;

        // Canonical name, e.g. "Vector<float32,4>".
        static string canonicalName(const CajetaTypePtr& elementType,
                                    uint32_t lanes);

        // Get-or-create the Vector<elem,N> instance, cached in canonicalMap.
        static shared_ptr<CajetaVector> getOrCreate(CajetaModulePtr module,
                                                    CajetaTypePtr elementType,
                                                    uint32_t lanes);

        // Validate the semantic constraints (element is a non-bool numeric
        // primitive; lanes is a positive constant) then getOrCreate. Shared by
        // every site that materializes a Vector type (CajetaType::fromContext
        // type positions and NewExpression construction), so the diagnostics
        // are identical. Throws CAJETA_ERROR_VECTOR_ELEMENT_TYPE /
        // CAJETA_ERROR_VECTOR_LENGTH.
        static shared_ptr<CajetaVector> validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType, int64_t lanes);
    };
    typedef shared_ptr<CajetaVector> CajetaVectorPtr;
}
