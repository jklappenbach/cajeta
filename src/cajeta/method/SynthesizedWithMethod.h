// Compiler-synthesized `withFieldName(T v)` copy-with mutators for `@With`
// (Annotations.md § Immutability friend). Lombok's `withCamelCase` naming avoids a
// collision with @Setter/@Getter; the body allocs, memcpys, then stores the field.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedWithMethod : public Method {
    public:
        SynthesizedWithMethod(CajetaModulePtr module,
                               CajetaClassPtr parent,
                               StructurePropertyPtr field);

        // Wires the value parameter's parent once the shared_ptr exists, as SynthesizedSetterMethod does.
        void initParameter();

        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        StructurePropertyPtr field;
    };
}
