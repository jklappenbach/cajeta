// Synthesized `__cajeta_inject()` static method on each @Component class: the
// spec's lazy singleton — allocate, dispatch the constructor, assign every
// @Inject field from its own __cajeta_inject(), then cache in a static global.

#pragma once

#include "Method.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    class ComponentInjectMethod : public Method {
    public:
        ComponentInjectMethod(CajetaModulePtr module,
                              CajetaClassPtr parent,
                              CajetaModule::ComponentDescriptorPtr descriptor);

        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        CajetaModule::ComponentDescriptorPtr descriptor;
    };
}
