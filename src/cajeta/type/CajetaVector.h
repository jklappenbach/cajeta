// CajetaVector — the fixed-width numeric vector `Vector<T, N>`, lowering to LLVM
// `<N x T>`. A VALUE type with no struct body, fields or methods: it derives from
// CajetaType, and every operation on it is lowered as a compiler intrinsic.

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

        // `<N x T>`; T's llvm scalar type must already be registered.
        llvm::Type* getLlvmType() override;

        // Canonical name, e.g. "Vector<float32,4>".
        static string canonicalName(const CajetaTypePtr& elementType,
                                    uint32_t lanes);

        // Get-or-create, cached in canonicalMap so one shape is one CajetaType.
        static shared_ptr<CajetaVector> getOrCreate(CajetaModulePtr module,
                                                    CajetaTypePtr elementType,
                                                    uint32_t lanes);

        // getOrCreate, after checking that the element is a non-bool numeric
        // primitive and lanes a positive constant. Every site that materializes a
        // Vector goes through here; throws CAJETA_ERROR_VECTOR_ELEMENT_TYPE/_LENGTH.
        static shared_ptr<CajetaVector> validateAndCreate(
            CajetaModulePtr module, CajetaTypePtr elementType, int64_t lanes);
    };
    typedef shared_ptr<CajetaVector> CajetaVectorPtr;
}
