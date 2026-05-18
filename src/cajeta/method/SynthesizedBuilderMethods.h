// Compiler-synthesized methods for the @Builder annotation
// (cajeta-docs/stdlib/Annotations.md § Builders). Three method shapes:
//
//   - SynthesizedBuilderSetterMethod  — `Outer.Builder fieldName(T v)`
//     chained setter on the Builder. Stores the value to the matching
//     Builder field and returns `this` for chaining.
//
//   - SynthesizedBuildMethod          — `Outer build()` on the Builder.
//     Allocates a fresh Outer instance, initializes its vtable slot,
//     calls Outer's all-args ctor with the Builder's accumulated
//     field values, and returns the new instance.
//
//   - SynthesizedBuilderFactoryMethod — `static Outer.Builder builder()`
//     on Outer. Allocates a Builder, initializes its vtable slot,
//     and returns it. (No call to Builder's ctor — Builder's ctor
//     is the synthesized no-arg one, which zero-inits all fields;
//     the alloc itself zero-fills.)
//
// All three are added during CajetaClass::synthesizeBuilder, which
// also creates the Builder CajetaClass and registers it in
// canonicalMap as `<outer-canonical>.Builder`.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedBuilderSetterMethod : public Method {
    public:
        SynthesizedBuilderSetterMethod(CajetaModulePtr module,
                                        CajetaClassPtr builder,
                                        StructurePropertyPtr field);

        void initParameter();
        void generateCode() override;

    private:
        StructurePropertyPtr field;
    };

    class SynthesizedBuildMethod : public Method {
    public:
        SynthesizedBuildMethod(CajetaModulePtr module,
                                CajetaClassPtr builder,
                                CajetaClassPtr outer);

        void generateCode() override;

    private:
        CajetaClassPtr outer;
    };

    class SynthesizedBuilderFactoryMethod : public Method {
    public:
        SynthesizedBuilderFactoryMethod(CajetaModulePtr module,
                                         CajetaClassPtr outer,
                                         CajetaClassPtr builder);

        void generateCode() override;

    private:
        CajetaClassPtr builder;
    };
}
