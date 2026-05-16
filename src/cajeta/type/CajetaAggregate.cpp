//
// CajetaAggregate — common base for CajetaStruct and CajetaView. See header
// for the design rationale.
//

#include "CajetaAggregate.h"

namespace cajeta {

    bool CajetaAggregate::isVariableSize(const StructurePropertyPtr& property) {
        if (!property || !property->getType()) return false;
        auto qn = property->getType()->getQName();
        if (qn && qn->getTypeName() == "String") return true;
        // CajetaArray-typed fields will follow in S5 (multiple variable-size
        // fields + post-variable fields).
        return false;
    }

} // namespace cajeta
