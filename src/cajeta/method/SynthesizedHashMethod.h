// Compiler-synthesized `int64 hash()` for classes annotated `@AutoHash`: emits IR
// directly, seeding from Hash.processSeed() and combining every inherited and own
// field. Struct, array and String fields are a compile error naming the field.

#pragma once

#include "Method.h"

namespace cajeta {
    class CajetaModule;
    class CajetaClass;

    class SynthesizedHashMethod : public Method {
    public:
        SynthesizedHashMethod(CajetaModulePtr module, CajetaClassPtr parent);

        void generateCode() override;
    };
}
