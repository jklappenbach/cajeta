#pragma once

#include "../type/CajetaClass.h"
#include <string>
#include <utility>
#include <vector>

namespace cajeta {

    // Populates a @GenerateMock `Mock<Name>` by synthesizing its body — a MockEngine field,
    // a ctor, forwarding overrides — as Cajeta SOURCE and re-parsing it into `mock`.
    void fillMockClassBody(const CajetaClassPtr& mock,
                           const CajetaClassPtr& target,
                           const CajetaModulePtr& module);

    // Synthesizes a no-arg constructor on `owner` initializing each field-level @Mock field
    // from (fieldName, mockCanonicalName) pairs, since the default ctor runs no initializers.
    void addMockFieldInitCtor(
        const CajetaClassPtr& owner,
        const std::vector<std::pair<std::string, std::string>>& inits,
        const CajetaModulePtr& module);
}
