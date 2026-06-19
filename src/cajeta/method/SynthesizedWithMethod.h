// Compiler-synthesized `withFieldName(T newValue)` per-field copy-with
// mutators for classes annotated `@With`
// (docs/specification/reflect/Annotations.md § Immutability friend).
//
// Naming follows Lombok's `withCamelCase` convention rather than Cajeta's
// size()-style: the with for field `x` is `withX(T v)`. Using the field
// name as the method name would collide with @Setter / @Getter; the
// `with` prefix disambiguates. Field name's first character is upper-
// cased; remainder is preserved.
//
// Body shape: __cajeta_alloc(sizeof(class)) for a fresh instance,
// memcpy the source instance's body (preserves vtable + every field),
// store the new value into the named field's slot, return the new ptr.
// The auto-field-drop machinery's live-allocation set keeps double-free
// safe — aliased class-ref fields no-op on the second drop attempt.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedWithMethod : public Method {
    public:
        SynthesizedWithMethod(CajetaModulePtr module,
                               CajetaClassPtr parent,
                               StructurePropertyPtr field);

        // Wire the value parameter's parent now that the shared_ptr
        // exists. Same pattern as SynthesizedSetterMethod.
        void initParameter();

        void generateCode() override;

    private:
        StructurePropertyPtr field;
    };
}
