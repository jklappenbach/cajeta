//
// CajetaStruct — stack-allocated value aggregate (Structs.md).
//
// Rollout note (StructsViewsStatus.md / S2): the struct keyword parses but
// generatePrototype() throws CAJETA_ERROR_STRUCT_UNIMPLEMENTED until S6.
// S6 lands stack alloca, class-ref fields, and aggregate initialization.
// S7 adds inline composition into class fields. S8 adds direct-call
// methods. S9-S11 add interface dispatch via tagged fat pointer.
//
// Sibling of CajetaView under CajetaAggregate. Codegen sites that need to
// match "any struct-shaped aggregate" use CajetaAggregate; sites that are
// specifically struct or specifically view cast to the leaf type.
//

#pragma once

#include "CajetaAggregate.h"

namespace cajeta {

    class CajetaStruct;
    typedef shared_ptr<CajetaStruct> CajetaStructPtr;

    class CajetaStruct : public CajetaAggregate {
    public:
        CajetaStruct(CajetaModulePtr module) : CajetaAggregate(module) { }
        CajetaStruct(CajetaModulePtr module, QualifiedNamePtr qName)
            : CajetaAggregate(module, qName) { }

        // Stack-struct codegen lands in S6. Until then this throws
        // CAJETA_ERROR_STRUCT_UNIMPLEMENTED — the keyword parses cleanly
        // so syntax tests can exist; instantiation fails fast with a
        // message pointing at the rollout doc.
        void generatePrototype() override;
    };

} // namespace cajeta
