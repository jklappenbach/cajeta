//
// CajetaAggregate — common base for CajetaStruct and CajetaView. See header
// for the design rationale.
//

#include "CajetaAggregate.h"
#include "CajetaArray.h"

namespace cajeta {

    bool CajetaAggregate::isVariableSize(const StructurePropertyPtr& property) {
        if (!property || !property->getType()) return false;
        auto type = property->getType();
        // String: stored inline as i32 length + UTF-8 bytes.
        auto qn = type->getQName();
        if (qn && qn->getTypeName() == "String") return true;
        // T[] where T is fixed-size: stored inline as i32 length +
        // length * sizeof(T) bytes (S5b). Variable-size element types
        // (T = String, T = nested-var-size-view) are not supported in v1
        // — the var-size codegen path doesn't handle the nested
        // length-prefix walk for them.
        if (dynamic_pointer_cast<CajetaArray>(type)) return true;
        return false;
    }

} // namespace cajeta
