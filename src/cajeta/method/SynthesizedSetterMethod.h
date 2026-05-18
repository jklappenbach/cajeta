// Compiler-synthesized field setter for classes / fields annotated `@Setter`.
//
// Naming follows size()-style: the setter for field `name` is `name(T v)`,
// NOT `setName(T v)`. Returns void. Visibility defaults to public.
//
// Skipped for `final` fields (Lombok parity) and for any field where the
// user already declared a same-signature method (user wins).
//
// Class-ref fields: the synthesizer stores the incoming pointer into the
// slot. v1 does NOT drop the previous holder — the auto-field-drop
// machinery at scope exit + the live-set discrimination handles the
// ownership story; if a user's setter assignment needs explicit drop of
// the old value, they can declare `name(T v)` by hand.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedSetterMethod : public Method {
    public:
        SynthesizedSetterMethod(CajetaModulePtr module,
                                 CajetaClassPtr parent,
                                 StructurePropertyPtr field);

        // Called by the owner (CajetaClass::synthesizeSetters) AFTER the
        // shared_ptr has been created. We can't take shared_from_this()
        // in the ctor, but the FormalParameter needs us as its parent
        // (StructureMetadata::createParameterType derefs parent->toCanonical()).
        void initParameter();

        void generateCode() override;

    private:
        StructurePropertyPtr field;
    };
}
