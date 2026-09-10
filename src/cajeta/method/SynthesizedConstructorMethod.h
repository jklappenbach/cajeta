// Compiler-synthesized constructor for @NoArgsConstructor, @AllArgsConstructor
// and @RequiredArgsConstructor; the caller picks the field subset. There is no
// implicit `super(args)` chaining: the body only stores params into fields.

#pragma once

#include "Method.h"
#include "../type/StructureProperty.h"

#include <vector>

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedConstructorMethod : public Method {
    public:
        // `fields` are bound from the parameter list in order; every other
        // non-static field is zero-initialized in the body.
        SynthesizedConstructorMethod(CajetaModulePtr module,
                                      CajetaClassPtr parent,
                                      std::vector<StructurePropertyPtr> fields);

        // Owner calls this after the shared_ptr exists, so that
        // FormalParameter::setParent can use shared_from_this(). Idempotent.
        void initParameters();

        // Emit the body in three passes: zero-init every non-static field,
        // run per-field initializers, then store the incoming params.
        void generateCode() override;

    private:
        std::vector<StructurePropertyPtr> fields;
    };
}
