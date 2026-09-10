// Synthesized provider accessor for an all-injected @Factory provider: the
// lazy-singleton IR of ComponentInjectMethod, built by the provider method.

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
        // Registered after the factory class's generatePrototype already ran, so
        // clear the cached prototype pointers to take the lazy path.
        llvmFunctionType = nullptr;
        llvmFunction = nullptr;
        addModifier(STATIC);
    }

    void FactoryProviderMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();
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

        llvm::Value* factoryInstance = callInject(parent, "factory");

        std::vector<ParameterEntry> args;
        for (auto& fp : provider.params) {
            if (!fp.injected) continue;
            llvm::Value* dep = emitInjectArg(builder, module, fp);
            args.emplace_back(fp.param->getType(), std::string(), dep);
        }

        std::string makeName = provider.method->getName();
        llvm::Value* product = parent->invokeMethod(
            makeName, args, /*isConstructor=*/false, factoryInstance,
            /*callerModule=*/module);

        if (isSingleton) {
            builder->CreateStore(product, singletonGlobal);
        }
        builder->CreateRet(product);
    }
}
