//
// CajetaStruct — stub for the stack-struct construct until S6 wires the
// real codegen (alloca on declaration, class-ref fields, inline composition
// into classes, interface dispatch). See StructsViewsStatus.md.
//

#include "CajetaStruct.h"
#include "../error/Exception.h"

namespace cajeta {

    void CajetaStruct::generatePrototype() {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "struct '%s' cannot be compiled yet — stack-struct semantics "
            "land in Session 6 of the rollout (see StructsViewsStatus.md). "
            "Use `view` for memory-overlay types in the meantime.",
            qName ? qName->toCanonical().c_str() : "?");
        throw Exception(buf, "CAJETA_ERROR_STRUCT_UNIMPLEMENTED");
    }

} // namespace cajeta
