// Compiler-synthesized `String toString()` for classes annotated `@ToString`.
//
// v1 surface (cajeta-docs/stdlib/Annotations.md § @ToString):
//   - TO_STRING_PROPERTIES (default): `ClassName(field1=val,field2=val,...)`
//   - TO_STRING_JSON: `{"field1":val,"field2":val,...}` — String values
//     get JSON-quoted + escaped via `__cajeta_json_quote_str`; numeric /
//     boolean fields render the same way as PROPERTIES (their natural
//     literal form is already valid JSON). Class-typed fields recurse
//     via `field.toString()`; emitted verbatim so a nested class with
//     `@ToString(TO_STRING_JSON)` produces properly nested JSON. Mixing
//     a PROPERTIES-format nested class into a JSON parent produces
//     malformed JSON — annotate the nested class consistently.
//
// Field-kind coverage in v1:
//   - boolean / int8..int64 / uint8..uint64 / float32 / float64 → OK
//   - char → emitted as a 1-char string
//   - String → printed as-is (PROPERTIES) / JSON-quoted (JSON)
//   - Class types → null-checked, then virtually dispatch to .toString()
//     (mirrors SynthesizedHashMethod's class-field handling)
//   - Array types → COMPILE ERROR (element walk not yet implemented;
//     workaround: declare toString() manually or @ToString.Exclude
//     the array field)
//   - View types / interface types → COMPILE ERROR (views are already
//     rejected as class fields; interface fields would need fat-pointer
//     vtable dispatch which @ToString doesn't yet support)
//
// `@ToString.Exclude` on a field omits it.

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
        // `selectedFields` + `hasExplicitFieldSelection`: when the flag
        // is true, the synthesizer walks EXACTLY `selectedFields` in
        // order (the `of={...}` allowlist form, including the
        // degenerate `of={}` "render no fields" case). When false,
        // the synthesizer walks the parent's declared non-static,
        // non-@Exclude fields in declaration order (default).
        //
        // `callSuper`: when true, emit `super=<super.toString()>` as
        // the first rendered entry. Mirrors Lombok's @ToString(callSuper=true).
        SynthesizedToStringMethod(CajetaModulePtr module,
                                   CajetaClassPtr parent,
                                   ToStringFormat format = ToStringFormat::PROPERTIES,
                                   std::vector<StructurePropertyPtr> selectedFields = {},
                                   bool hasExplicitFieldSelection = false,
                                   bool callSuper = false);

        void generateCode() override;

    private:
        ToStringFormat format;
        std::vector<StructurePropertyPtr> selectedFields;
        bool hasExplicitFieldSelection;
        bool callSuper;
    };
}
