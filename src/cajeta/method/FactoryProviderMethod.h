//
// Synthesized provider accessor for an all-injected @Factory provider
// (AspectModel.md § @Factory, R1–R4 / aot-di Unit 4a).
//
// For each all-injected provider method `T make(@Inject A a, @Inject B b)`
// on a @Factory class, the compiler synthesizes a static accessor on the
// factory class:
//
//   static <T> __cajeta_provide_<Factory>_<T>() {
//       // @Singleton: lazy memo (null-check branch, like __cajeta_inject)
//       <Factory> f = <Factory>::__cajeta_inject();   // the factory singleton
//       <T> product = f.make(A::__cajeta_inject(), B::__cajeta_inject());
//       // @Singleton: cache + return; @Transient: return fresh
//   }
//
// The provider method returns a fresh-owned `#T` (R2); the framework owns
// the singleton drop. Consumers' @Inject of the product type T route their
// field injection to this accessor (ComponentInjectMethod). Assisted
// providers have no accessor — their product is built via the user's manual
// call (Unit 4b threads the @Inject args there).
//

#pragma once

#include "Method.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    class FactoryProviderMethod : public Method {
    public:
        FactoryProviderMethod(CajetaModulePtr module,
                              CajetaClassPtr factoryClass,
                              CajetaModule::FactoryDescriptorPtr descriptor,
                              int providerIdx);

        void generateCode() override;

        static std::string accessorName(CajetaClassPtr factoryClass,
                                        CajetaTypePtr providedType);

    private:
        CajetaModule::FactoryDescriptorPtr descriptor;
        int providerIdx;
        llvm::GlobalVariable* singletonGlobal = nullptr;   // @Singleton memo
    };
}
