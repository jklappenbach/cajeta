// Synthesized `__cajeta_inject()` static method on each @Component class. It returns
// the lazy singleton, the instance in the active scope's table, or for a Transient
// component a fresh instance the caller owns.

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
        // Declared `scope = "Transient"`: every call builds, and the caller owns the result.
        bool isTransient() const {
            return descriptor && descriptor->scope
                && descriptor->scope->kind == CajetaModule::ScopePublication::Kind::Builtin
                && descriptor->scope->name == "Transient";
        }

    private:
        CajetaModule::ComponentDescriptorPtr descriptor;
        llvm::Value* singletonOf(const CajetaModule::ComponentDescriptorPtr& target);
        llvm::Value* emitMultibinding(const CajetaModule::ResolvedDependency& rd);
    };
}
