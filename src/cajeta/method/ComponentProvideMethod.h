// Synthesized `static void __cajeta_provide(#C value)` on each component with a
// published scope: Components.provide's body, placing a value in the active scope.

#pragma once

#include "Method.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    class ComponentProvideMethod : public Method {
    public:
        ComponentProvideMethod(CajetaModulePtr module,
                               CajetaClassPtr parent,
                               CajetaModule::ComponentDescriptorPtr descriptor);

        // The one `#C value` formal; called once the method is shared.
        void initParameters();

        // Publishes the value into the active scope's table, owned when it was
        // transferred. Refuses, freeing an owned value, when the scope is inactive
        // or already holds one.
        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

    private:
        CajetaModule::ComponentDescriptorPtr descriptor;
    };
}
