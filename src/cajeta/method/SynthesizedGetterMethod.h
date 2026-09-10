// Compiler-synthesized field getter for `@Getter`: the getter for field `name` is
// `name()`, not `getName()`, returning the field's declared type, public unless
// `@Getter(level="private")`. Skipped where the user declared a same-name no-arg method.

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
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        StructurePropertyPtr field;
    };
}
