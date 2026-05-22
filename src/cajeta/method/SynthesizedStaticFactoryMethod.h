// Compiler-synthesized static factory for `@AllArgsConstructor(staticName="of")`
// (and the same arg on @NoArgsConstructor / @RequiredArgsConstructor).
//
// Lombok-mirror semantic: when `staticName` is supplied on a ctor
// annotation, the generated ctor is marked PRIVATE and a public
// static method is synthesized whose body allocates a heap instance,
// initializes its vtable, calls the (now-private) ctor with the
// supplied args, and returns the new instance.
//
// Visibility note: the `access` arg on the constructor annotation
// applies to the FACTORY (not the ctor) when `staticName` is set
// — Lombok parity. The ctor's modifier is force-set to PRIVATE so
// future visibility-enforcement work guides callers toward the
// factory.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;
    class SynthesizedConstructorMethod;

    class SynthesizedStaticFactoryMethod : public Method {
    public:
        // `ctor` is the synthesized ctor this factory wraps. Their
        // parameter shapes mirror each other (factory excludes the
        // implicit `this` the ctor's signature carries).
        SynthesizedStaticFactoryMethod(
            CajetaModulePtr module,
            CajetaClassPtr parent,
            std::shared_ptr<SynthesizedConstructorMethod> ctor,
            const std::string& methodName,
            std::vector<StructurePropertyPtr> fields);

        void initParameters();
        void generateCode() override;

    private:
        std::shared_ptr<SynthesizedConstructorMethod> ctor;
        std::vector<StructurePropertyPtr> fields;
    };
}
