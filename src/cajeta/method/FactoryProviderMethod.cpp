//
// AspectModel.md § @Factory (aot-di Unit 4a): synthesized provider
// accessor for an all-injected @Factory provider. See the header for the
// shape. Mirrors ComponentInjectMethod's lazy-singleton IR; the product
// is built by invoking the factory's provider method (init beyond ctor,
// R2 fresh-owned) rather than a plain constructor.
//

#include "FactoryProviderMethod.h"
#include "../type/CajetaClass.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

namespace cajeta {

    std::string FactoryProviderMethod::accessorName(
            CajetaClassPtr factoryClass, CajetaTypePtr providedType) {
        std::string fc = (factoryClass && factoryClass->getQName())
            ? factoryClass->getQName()->toCanonical() : std::string("factory");
        std::string pt = (providedType && providedType->getQName())
            ? providedType->getQName()->toCanonical() : std::string("T");
        return "__cajeta_provide_" + fc + "_" + pt;
    }

    llvm::Value* FactoryProviderMethod::emitInjectArg(
            llvm::IRBuilder<>* builder, CajetaModulePtr module,
            CajetaModule::FactoryProvider::Param& fp) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* nullPtr = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));
        if (fp.resolvedFactory && fp.resolvedProviderIdx >= 0) {
            auto& nested =
                fp.resolvedFactory->providers[fp.resolvedProviderIdx];
            if (nested.accessor) {
                nested.accessor->getLlvmFunctionType();
                llvm::Function* fn = CajetaModule::ensureFunctionVisible(
                    builder, nested.accessor->getLlvmFunction(),
                    nested.accessor->getLlvmFunctionType());
                return builder->CreateCall(
                    nested.accessor->getLlvmFunctionType(), fn, {}, "idep");
            }
            return nullPtr;
        }
        if (fp.resolvedTarget && fp.resolvedTarget->klass) {
            MethodPtr inj;
            for (auto& [mk, m] : fp.resolvedTarget->klass->getMethods()) {
                if (m && m->getName() == "__cajeta_inject") { inj = m; break; }
            }
            if (inj) {
                inj->getLlvmFunctionType();
                llvm::Function* fn = CajetaModule::ensureFunctionVisible(
                    builder, inj->getLlvmFunction(), inj->getLlvmFunctionType());
                return builder->CreateCall(
                    inj->getLlvmFunctionType(), fn, {}, "idep");
            }
        }
        return nullPtr;
    }

    FactoryProviderMethod::FactoryProviderMethod(
            CajetaModulePtr module,
            CajetaClassPtr factoryClass,
            CajetaModule::FactoryDescriptorPtr descriptor,
            int providerIdx)
        : Method(module,
                 accessorName(factoryClass,
                              descriptor->providers[providerIdx].providedType),
                 descriptor->providers[providerIdx].providedType,
                 factoryClass),
          descriptor(std::move(descriptor)),
          providerIdx(providerIdx) {
        // Added after the factory class's generatePrototype already ran
        // (during resolveDependencyGraph's tail), so clear the cached
        // prototype pointers to take the lazy generatePrototype path —
        // same dance as ComponentInjectMethod.
        llvmFunctionType = nullptr;
        llvmFunction = nullptr;
        addModifier(STATIC);
    }

    void FactoryProviderMethod::generateCode() {
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* nullPtr = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));

        auto& provider = descriptor->providers[providerIdx];
        bool isSingleton =
            (provider.scope != CajetaModule::AllocateMode::Transient);

        llvm::BasicBlock* entry =
            llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        builder = new llvm::IRBuilder<>(entry);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        if (isSingleton) {
            // Lazy memo: load + null-check + branch (same shape as
            // __cajeta_inject). The framework owns this singleton's drop.
            if (!singletonGlobal) {
                std::string gname = "__cajeta_provided_"
                    + accessorName(parent, provider.providedType);
                singletonGlobal = new llvm::GlobalVariable(
                    *lmod, ptrTy, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage, nullPtr, gname);
            }
            llvm::BasicBlock* cached =
                llvm::BasicBlock::Create(ctx, "cached", llvmFunction);
            llvm::BasicBlock* fresh =
                llvm::BasicBlock::Create(ctx, "fresh", llvmFunction);
            llvm::Value* cur =
                builder->CreateLoad(ptrTy, singletonGlobal, "cur");
            llvm::Value* isNull = builder->CreateICmpEQ(cur, nullPtr, "isnull");
            builder->CreateCondBr(isNull, fresh, cached);
            builder->SetInsertPoint(cached);
            builder->CreateRet(cur);
            builder->SetInsertPoint(fresh);
        }

        // Call a static no-arg __cajeta_inject on `klass` and yield the ptr.
        auto callInject = [&](CajetaClassPtr klass,
                              const std::string& label) -> llvm::Value* {
            if (!klass) return nullPtr;
            MethodPtr inj;
            for (auto& [mk, m] : klass->getMethods()) {
                if (m && m->getName() == "__cajeta_inject") { inj = m; break; }
            }
            if (!inj) return nullPtr;
            inj->getLlvmFunctionType();   // force prototype
            llvm::Function* fn = CajetaModule::ensureFunctionVisible(
                builder, inj->getLlvmFunction(), inj->getLlvmFunctionType());
            return builder->CreateCall(
                inj->getLlvmFunctionType(), fn, {}, label);
        };

        // 1. The factory singleton — the receiver of the provider method.
        llvm::Value* factoryInstance = callInject(parent, "factory");

        // 2. Resolve each @Inject param (an all-injected provider has only
        //    these; assisted providers get no accessor at all).
        std::vector<ParameterEntry> args;
        for (auto& fp : provider.params) {
            if (!fp.injected) continue;
            llvm::Value* dep = emitInjectArg(builder, module, fp);
            args.emplace_back(fp.param->getType(), std::string(), dep);
        }

        // 3. Invoke the provider method on the factory → fresh product (R2).
        std::string makeName = provider.method->getName();
        llvm::Value* product = parent->invokeMethod(
            makeName, args, /*isConstructor=*/false, factoryInstance,
            /*callerModule=*/module);

        // 4. Cache (singleton) + return.
        if (isSingleton) {
            builder->CreateStore(product, singletonGlobal);
        }
        builder->CreateRet(product);
    }
}
