// Synthetic per-binding-site capture type for wildcard receivers: the
// compile-time identity of the unknown T one wildcard-typed binding holds, so
// two `Box<? extends Animal>` locals get distinct captures. Erased at runtime.

#pragma once

#include "CajetaClass.h"

namespace cajeta {

    class CajetaCapture;
    typedef shared_ptr<CajetaCapture> CajetaCapturePtr;

    class CajetaCapture : public CajetaClass {
    private:
        int64_t captureId;
        CajetaTypePtr upperBound;
        CajetaTypePtr lowerBound;

        // Monotonic, process-global source of fresh capture IDs.
        static int64_t nextCaptureId();

    public:
        // Constructs a capture with the given bounds; exposed for make_shared,
        // but callers should go through the factories below.
        CajetaCapture(CajetaModulePtr module,
                      QualifiedNamePtr qName,
                      int64_t id,
                      CajetaTypePtr upperBound,
                      CajetaTypePtr lowerBound);

        // A fresh capture for an extends-bounded wildcard; lower bound is null.
        static CajetaCapturePtr forExtendsBound(
            CajetaModulePtr module, CajetaTypePtr upperBound);

        // A fresh capture for a super-bounded wildcard; upper bound is null.
        static CajetaCapturePtr forSuperBound(
            CajetaModulePtr module, CajetaTypePtr lowerBound);

        // A fresh capture for an unbounded wildcard; both bounds null.
        static CajetaCapturePtr forUnbounded(CajetaModulePtr module);

        int64_t getCaptureId() const { return captureId; }
        CajetaTypePtr getUpperBound() const { return upperBound; }
        CajetaTypePtr getLowerBound() const { return lowerBound; }

        bool isCapture() const { return true; }
    };

} // namespace cajeta
