// Compiler-synthesized methods for the @Builder annotation
// (docs/stdlib/Annotations.md § Builders). Three method shapes:
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
        // `methodName` is the chained setter's name on Builder. With
        // @Builder(setterPrefix="with") on a field `name`, the caller
        // passes `withName`; otherwise the bare field name.
        SynthesizedBuilderSetterMethod(CajetaModulePtr module,
                                        CajetaClassPtr builder,
                                        StructurePropertyPtr field,
                                        const std::string& methodName);

        void initParameter();
        void generateCode() override;

    private:
        StructurePropertyPtr field;
    };

    class SynthesizedBuildMethod : public Method {
    public:
        // `methodName` defaults to "build" (Lombok parity) but
        // @Builder(buildMethodName="...") renames it.
        SynthesizedBuildMethod(CajetaModulePtr module,
                                CajetaClassPtr builder,
                                CajetaClassPtr outer,
                                const std::string& methodName = "build");

        void generateCode() override;

    private:
        CajetaClassPtr outer;
    };

    class SynthesizedBuilderFactoryMethod : public Method {
    public:
        // Per-field default to apply at builder() time. The mirror
        // field is the Builder's struct slot to write; the initializer
        // is the outer's parsed `field = expr` AST node (only
        // populated for fields annotated @Builder.Default).
        struct DefaultEntry {
            StructurePropertyPtr mirrorField;
            AbstractSyntaxNodePtr initializer;
        };

        // `methodName` defaults to "builder" but
        // @Builder(builderMethodName="...") renames it.
        // `defaults` carries the @Builder.Default field initializers
        // that the factory body evaluates and stores into the
        // freshly-allocated Builder. The user-facing semantic:
        // builder() returns a Builder whose @Builder.Default slots
        // already hold their declared defaults; setter calls overwrite
        // them; build() consumes whatever's in the slots.
        SynthesizedBuilderFactoryMethod(CajetaModulePtr module,
                                         CajetaClassPtr outer,
                                         CajetaClassPtr builder,
                                         const std::string& methodName = "builder",
                                         std::vector<DefaultEntry> defaults = {});

        void generateCode() override;

    private:
        CajetaClassPtr builder;
        std::vector<DefaultEntry> defaults;
    };
}
