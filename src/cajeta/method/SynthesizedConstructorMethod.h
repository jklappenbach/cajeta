// Compiler-synthesized constructor for classes annotated with one of:
//   - @NoArgsConstructor — zero-arg ctor, fields zero-init'd
//   - @AllArgsConstructor — one param per non-static field
//   - @RequiredArgsConstructor — only `final` (and future @NonNull) fields
//
// (See cajeta-docs/stdlib/Annotations.md § Constructors.)
//
// v1 limitations:
//   - No implicit `super(args)` chaining. The synthesized body only
//     stores the incoming params into the matching fields. Classes
//     whose parent ctors need explicit args must hand-write the
//     constructor.
//   - For @NoArgsConstructor, fields without a declared initializer
//     receive a zero (primitive) or null (class-ref / array) — the
//     planned "fail-if-non-null-field-has-no-default" diagnostic
//     lands once @NonNull does.
//
// A single class can implement this synthesizer; the caller
// (CajetaClass::synthesize{NoArgs,AllArgs,RequiredArgs}Constructor)
// picks the field subset.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

#include <vector>

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedConstructorMethod : public Method {
    public:
        // `fields` is the list of fields to bind from the parameter
        // list, in order. Fields NOT in this list are zero-initialized
        // in the body.
        SynthesizedConstructorMethod(CajetaModulePtr module,
                                      CajetaClassPtr parent,
                                      std::vector<StructurePropertyPtr> fields);

        // Owner calls this after the shared_ptr is created so
        // FormalParameter::setParent can use shared_from_this(). See
        // SynthesizedSetterMethod for the same pattern + rationale.
        void initParameters();

        void generateCode() override;

    private:
        std::vector<StructurePropertyPtr> fields;
    };
}
