//
// CajetaStruct — stack-allocated value aggregate (Structs.md).
//
// Sibling of CajetaView under CajetaAggregate. Codegen sites that need to
// match "any struct-shaped aggregate" use CajetaAggregate; sites that are
// specifically struct or specifically view cast to the leaf type.
//
// S6.1 (current): primitive-only fields, alloca + zero-init for locals.
// S6.3 lifts the no-class-ref restriction. S6.6 rejects variable-tail
// (`byte[?]`) at the layout pass. S7 adds inline composition into class
// fields. S8 adds direct-call methods. S9-S11 add interface dispatch via
// tagged fat pointer.
//

#pragma once

#include "CajetaAggregate.h"

namespace cajeta {

    class CajetaStruct;
    typedef shared_ptr<CajetaStruct> CajetaStructPtr;

    class CajetaStruct : public CajetaAggregate {
        // S9.2 emits per-(struct, interface) vtable globals; S9.5.2
        // added the parallel per-(class, interface) emission. Both
        // store their result in the inherited `interfaceVTables` map
        // on CajetaClass, with the only difference being the global's
        // naming prefix (`struct.` vs `class.`). The getInterfaceVTable
        // / getInterfaceVTables accessors on CajetaClass cover both.
    public:
        CajetaStruct(CajetaModulePtr module) : CajetaAggregate(module) { }
        CajetaStruct(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaAggregate(module, qName) { }

        // Build the LLVM struct body, register the type, and prototype any
        // methods. Field types are validated here — v1 accepts primitives,
        // nested structs, and class refs (S6.3); arrays, views, interfaces,
        // and recursive shapes are rejected with specific error IDs.
        void generatePrototype() override;

        // Synthesize (or return the cached) drop function for this struct.
        // Walks class-ref fields in reverse declaration order, loading each
        // pointer and calling the referent class's own drop function. Does
        // NOT free the body — structs are stack-resident. Primitives and
        // nested-struct fields are skipped (primitives have no drop; nested
        // structs land in a follow-up when struct fields participate in
        // composition). See StructsViewsStatus.md S6.4.
        llvm::Function* getOrCreateDropFunction() override;

        // Total byte size of an instance — driven by LLVM's data layout
        // computation over the (compiler-chosen, naturally-aligned) struct
        // body. Used by the alloca path in StackField. Returns 0 until
        // generatePrototype runs.
        uint64_t getFixedSize() const;

    private:
        // S9.2 — emit a static vtable global for each implemented
        // interface. Called from generatePrototype after method
        // prototypes exist (so each method's LLVM function pointer can
        // be harvested). Naming convention:
        //   `struct.<sanitized-struct-canonical>_iface_<sanitized-iface-canonical>_VTable`
        void synthesizeInterfaceVTables();
    };

} // namespace cajeta
