// Compiler-synthesized static factory for `@AllArgsConstructor(staticName="of")`
// and the same arg on the other ctor annotations: the generated ctor becomes
// PRIVATE, and the factory heap-allocates, inits the vtable, calls it, and returns.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;
    class SynthesizedConstructorMethod;

    class SynthesizedStaticFactoryMethod : public Method {
    public:
        // `ctor` is the synthesized constructor this factory wraps; their parameter
        // shapes mirror each other, minus the implicit `this`.
        SynthesizedStaticFactoryMethod(
            CajetaModulePtr module,
            CajetaClassPtr parent,
            std::shared_ptr<SynthesizedConstructorMethod> ctor,
            const std::string& methodName,
            std::vector<StructurePropertyPtr> fields);

        void initParameters();
        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        std::shared_ptr<SynthesizedConstructorMethod> ctor;
        std::vector<StructurePropertyPtr> fields;
    };
}
