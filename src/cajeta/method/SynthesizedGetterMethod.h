// Compiler-synthesized field getter for classes / fields annotated `@Getter`.
//
// Naming follows size()-style: the getter for field `name` is `name()`,
// NOT `getName()`. Returns the field's declared type. Visibility defaults
// to public; `@Getter(level="private")` tightens it (v1: only `public` and
// `private` accepted, default public).
//
// Wired from CajetaClass::synthesizeGetters() which iterates the property
// list and emits one method per qualifying field, skipping any field for
// which the user already declared a same-name no-arg method (user wins).

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedGetterMethod : public Method {
    public:
        SynthesizedGetterMethod(CajetaModulePtr module,
                                 CajetaClassPtr parent,
                                 StructurePropertyPtr field);

        void generateCode() override;

    private:
        StructurePropertyPtr field;
    };
}
