// The static accessor synthesized on a @Factory class for each all-injected
// provider: it resolves the factory singleton, calls the provider with each
// @Inject argument resolved, and memoizes the product when it is @Singleton.

#pragma once

#include "Method.h"
#include "../compile/CajetaModule.h"
#include "llvm/IR/IRBuilder.h"

namespace cajeta {
    class FactoryProviderMethod : public Method {
    public:
        FactoryProviderMethod(CajetaModulePtr module,
                              CajetaClassPtr factoryClass,
                              CajetaModule::FactoryDescriptorPtr descriptor,
                              int providerIdx);

        // Emits the accessor body as raw IR: the @Singleton memo check and its cached
        // return first, then the factory instance, the provider call with every
        // @Inject argument resolved, and the store into the memo global.
        void generateCode() override;
        bool emitsReturnFlag() override { return false; }  // raw-IR body: never stores the return flag

        static std::string accessorName(CajetaClassPtr factoryClass,
                                        CajetaTypePtr providedType);

        // The value for a resolved @Inject param: the component's
        // __cajeta_inject, or a nested provider accessor. Returns a null pointer
        // constant when unresolved, since the graph has already errored.
        static llvm::Value* emitInjectArg(
            llvm::IRBuilder<>* builder,
            CajetaModulePtr module,
            CajetaModule::FactoryProvider::Param& fp);

    private:
        CajetaModule::FactoryDescriptorPtr descriptor;
        int providerIdx;
        llvm::GlobalVariable* singletonGlobal = nullptr;   // @Singleton memo
    };
}
