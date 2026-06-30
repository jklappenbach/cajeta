#pragma once

#include "../type/CajetaClass.h"

namespace cajeta {

    // Populate a @GenerateMock-generated `Mock<Name>` class by synthesizing its
    // body (a dev.cajeta.unit.MockEngine field, a ctor, and a forwarding override
    // of every overridable target method) as Cajeta SOURCE and re-parsing it into
    // `mock` — reusing the whole front-end instead of hand-writing LLVM IR per
    // signature. `mock` already extends `target` (set by synthesizeMock via
    // fillFromDeclaration). The caller runs `mock->generatePrototype()` afterward.
    //
    // See docs/specs/mock-codegen-spec.md.
    void fillMockClassBody(const CajetaClassPtr& mock,
                           const CajetaClassPtr& target,
                           const CajetaModulePtr& module);
}
