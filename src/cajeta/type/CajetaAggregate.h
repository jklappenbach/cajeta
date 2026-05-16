//
// CajetaAggregate — shared base for the two struct-shaped user-defined
// types: CajetaStruct (stack value) and CajetaView (memory overlay).
//
// Sits between CajetaClass and the two leaf types. Carries only what's
// genuinely common to both:
//
//   - Both are "aggregate of fields with declared types."
//   - Both lack a vtable header, so user-declared properties occupy slot
//     indices starting at 0 (CajetaClass would reserve slot 0 for the
//     vtable pointer; this override removes that).
//   - Both pass by pointer at call sites (handled where the cast happens
//     in ParameterField / CajetaFunctionType / etc.).
//   - The variable-size-field predicate (length-prefix-encoded String /
//     T[] fields) is meaningful in v1 only for views, but lives here so
//     that future stack-struct evolution can reuse it without an awkward
//     reach into a sibling type.
//
// What's deliberately NOT in this base:
//   - Endianness, alignment — view-only concepts (structs are host-endian
//     with compiler-chosen layout per Structs.md).
//   - generatePrototype — different shape between struct and view, both
//     override directly.
//   - getFixedSize — only meaningful once the LLVM struct body has been
//     built, which is currently view-only.
//

#pragma once

#include "CajetaClass.h"
#include "StructureProperty.h"

namespace cajeta {

    class CajetaAggregate;
    typedef shared_ptr<CajetaAggregate> CajetaAggregatePtr;

    class CajetaAggregate : public CajetaClass {
    public:
        CajetaAggregate(CajetaModulePtr module) : CajetaClass(module) { }
        CajetaAggregate(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaClass(module, qName, {}, {}) { }

        // True iff `property` is a variable-size field. Today recognizes
        // String fields; T[]-as-inline-field follows in S5. Used by view
        // layout (length-prefix substitution) and view field accessors
        // (DotExpression.cpp).
        static bool isVariableSize(const StructurePropertyPtr& property);

        // No vtable header — user properties stay at 0-based LLVM indices.
        // CajetaClass's default reserves slot 0 for the vtable pointer.
        int getFieldLlvmIndex(const StructurePropertyPtr& prop) const override {
            return prop->getOrder();
        }
    };

} // namespace cajeta
