// Compiler-synthesized methods for @Builder: the chained setters, build() on
// the Builder, and the static builder() factory. Added by synthesizeBuilder.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedBuilderSetterMethod : public Method {
    public:
        // Chained setter `Outer.Builder methodName(T v)`: stores to the matching
        // Builder field and returns `this`. @Builder(setterPrefix) decides
        // whether `methodName` is prefixed or the bare field name.
        SynthesizedBuilderSetterMethod(CajetaModulePtr module,
                                        CajetaClassPtr builder,
                                        StructurePropertyPtr field,
                                        const std::string& methodName);

        void initParameter();
        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        StructurePropertyPtr field;
    };

    class SynthesizedBuildMethod : public Method {
    public:
        // `Outer methodName()` on the Builder: allocates an Outer, initializes its
        // vtable slot, and calls Outer's all-args ctor with the Builder's slots.
        SynthesizedBuildMethod(CajetaModulePtr module,
                                CajetaClassPtr builder,
                                CajetaClassPtr outer,
                                const std::string& methodName = "build");

        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        CajetaClassPtr outer;
    };

    class SynthesizedBuilderFactoryMethod : public Method {
    public:
        // One @Builder.Default: the Builder slot to write, and the outer's
        // parsed `field = expr` node that builder() evaluates into it.
        struct DefaultEntry {
            StructurePropertyPtr mirrorField;
            AbstractSyntaxNodePtr initializer;
        };

        // `static Outer.Builder methodName()`: allocates a Builder, initializes
        // its vtable slot, and stores `defaults` into the matching slots. The
        // alloc zero-fills, so no call to Builder's ctor is emitted.
        SynthesizedBuilderFactoryMethod(CajetaModulePtr module,
                                         CajetaClassPtr outer,
                                         CajetaClassPtr builder,
                                         const std::string& methodName = "builder",
                                         std::vector<DefaultEntry> defaults = {});

        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        CajetaClassPtr builder;
        std::vector<DefaultEntry> defaults;
    };
}
