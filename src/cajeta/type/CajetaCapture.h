//
// CajetaCapture — synthetic per-binding-site capture type for wildcard
// receivers (see docs/CaptureConversion.md).
//
// A capture is the compile-time identity of "the unknown T that this
// specific wildcard-typed binding holds." Two `Box<? extends Animal>`
// locals get distinct captures (distinct IDs) even though they share
// the same source-level wildcard; that's what lets the type checker
// reject `b1.set(b2.get())` while accepting `b.set(b.get())`.
//
// At runtime captures are pointer-shaped and identity-erased — they
// contribute nothing to the vtable. The substitution-stable vtable
// hashes shipped in 1656022 already make dispatch work uniformly
// through any erased instantiation, so captures need no runtime
// machinery of their own.
//
// Phase 1.1 (this file): the type itself + factory. Wiring into
// LocalVariableDeclaration / method-call-site substitution / PECS
// follow in subsequent phases. The class exists now so the type
// system has a place to put captures before any code path produces
// them.
//

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

        // Monotonic source for fresh capture IDs. Module-local would
        // serve as well; global keeps test introspection simple
        // (each binding site gets a globally distinct ID).
        static int64_t nextCaptureId();

    public:
        // Constructs a capture with the given bounds. Callers go through
        // the factory below — the constructor is exposed for
        // make_shared but isn't the intended entry point.
        CajetaCapture(CajetaModulePtr module,
                      QualifiedNamePtr qName,
                      int64_t id,
                      CajetaTypePtr upperBound,
                      CajetaTypePtr lowerBound);

        // Factory: fresh capture for an extends-bounded wildcard.
        // `bound` is the upper bound; lower bound is null.
        static CajetaCapturePtr forExtendsBound(
            CajetaModulePtr module, CajetaTypePtr upperBound);

        // Factory: fresh capture for a super-bounded wildcard.
        // `bound` is the lower bound; upper bound is null (effectively
        // Object — not modeled explicitly yet).
        static CajetaCapturePtr forSuperBound(
            CajetaModulePtr module, CajetaTypePtr lowerBound);

        // Factory: fresh capture for an unbounded wildcard. Both
        // bounds null. Reads project to Object (when that lands).
        static CajetaCapturePtr forUnbounded(CajetaModulePtr module);

        int64_t getCaptureId() const { return captureId; }
        CajetaTypePtr getUpperBound() const { return upperBound; }
        CajetaTypePtr getLowerBound() const { return lowerBound; }

        // Capture predicate; distinguishes from plain classes and from
        // wildcard sentinels.
        bool isCapture() const { return true; }
    };

} // namespace cajeta
