// Compiler-synthesized `String toString()` for classes annotated `@ToString`,
// rendering `ClassName(f=v,...)` or JSON. Class-typed fields recurse through
// their own toString(); array, view and interface fields are a compile error.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"
#include <vector>

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    enum class ToStringFormat {
        PROPERTIES,
        JSON,
    };

    class SynthesizedToStringMethod : public Method {
    public:
        // With `hasExplicitFieldSelection`, exactly `selectedFields` are walked in
        // order, `of={}` included; otherwise the parent's declared non-static,
        // non-@Exclude fields. `callSuper` prepends `super=<super.toString()>`.
        SynthesizedToStringMethod(CajetaModulePtr module,
                                   CajetaClassPtr parent,
                                   ToStringFormat format = ToStringFormat::PROPERTIES,
                                   std::vector<StructurePropertyPtr> selectedFields = {},
                                   bool hasExplicitFieldSelection = false,
                                   bool callSuper = false);

        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        ToStringFormat format;
        std::vector<StructurePropertyPtr> selectedFields;
        bool hasExplicitFieldSelection;
        bool callSuper;
    };
}
