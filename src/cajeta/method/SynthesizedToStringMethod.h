// Compiler-synthesized `String toString()` for classes annotated `@ToString`.
//
// v1 surface (cajeta-docs/stdlib/Annotations.md § @ToString):
//   - TO_STRING_PROPERTIES (default): `ClassName(field1=val,field2=val,...)`
//   - TO_STRING_JSON: deferred to the cajeta.codec.json library
//     (S-1101/S-1102 in Features.md); requesting this in v1 raises
//     CAJETA_ERROR_TOSTRING_JSON_NOT_IMPLEMENTED.
//
// Field-kind coverage in v1:
//   - boolean / int8..int64 / uint8..uint64 / float32 / float64 → OK
//   - char → emitted as a 1-char string
//   - String → printed as-is
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

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedToStringMethod : public Method {
    public:
        SynthesizedToStringMethod(CajetaModulePtr module,
                                   CajetaClassPtr parent);

        void generateCode() override;
    };
}
