//
// Synthesized `__cajeta_inject()` static method on each
// @Component class. AspectModel.md § A9.
//
// The method realizes the spec's lazy singleton: on first call
// per component, it allocates an instance, dispatches the
// constructor, assigns every @Inject field by calling the
// dependency's own __cajeta_inject(), and caches the pointer in a
// module-level static global. Subsequent calls return the cached
// pointer. Field-level @Inject only — constructor-parameter
// injection rides A10 alongside qualifier support.
//
// One ComponentInjectMethod is constructed per @Component during
// resolveDependencyGraph's tail synthesis pass. It registers
// itself on the parent CajetaClass like any other method, so
// Phase 1's getLlvmFunctionType / Phase 2's generateCode walk
// pick it up without special handling at the compiler level.
//

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
