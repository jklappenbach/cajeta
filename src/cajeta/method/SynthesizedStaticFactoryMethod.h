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

        // Mirrors `fields` into the parameter list, one per field, in order. Idempotent,
        // and must run before generateCode, which forwards the arguments positionally.
        void initParameters();
        // Emits the whole body: malloc the parent, store its vtable, call the wrapped
        // ctor with the arguments, return the owned instance. Throws
        // CAJETA_ERROR_STATIC_FACTORY_NO_LAYOUT / _NO_CTOR on codegen-ordering failures.
        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        std::shared_ptr<SynthesizedConstructorMethod> ctor;
        std::vector<StructurePropertyPtr> fields;
    };
}
