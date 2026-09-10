// Compiler-synthesized field setter for `@Setter`: the setter for field `name` is
// `name(T v)`, not `setName(T v)`, returns void, and is public. Skipped for `final`
// fields and for any field the user already gave a same-signature method.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedSetterMethod : public Method {
    public:
        SynthesizedSetterMethod(CajetaModulePtr module,
                                 CajetaClassPtr parent,
                                 StructurePropertyPtr field);

        // Called by CajetaClass::synthesizeSetters AFTER the shared_ptr exists: the
        // FormalParameter needs this method as its parent, which the ctor cannot give.
        void initParameter();

        void generateCode() override;

    private:
        StructurePropertyPtr field;
    };
}
