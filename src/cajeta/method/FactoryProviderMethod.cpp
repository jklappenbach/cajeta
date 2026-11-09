// Synthesized provider accessor for an all-injected @Factory provider: the
// lazy-singleton IR of ComponentInjectMethod, built by the provider method.

#include "FactoryProviderMethod.h"
#include "../type/CajetaClass.h"
#include "../asn/expression/Expression.h"
#include "../error/Exception.h"

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
        // component-scopes §11: a product in a published scope lives in that
        // scope's table, like a scoped component, with the factory as its build.
        const bool scoped = provider.scope == CajetaModule::AllocateMode::Scoped
            && provider.scopePub;
        bool isSingleton = !scoped
            && (provider.scope != CajetaModule::AllocateMode::Transient);

        llvm::BasicBlock* entry =
            llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        builder = new llvm::IRBuilder<>(entry);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        llvm::Value* claim = nullptr;
        if (scoped) {
            const std::string canonical = (provider.providedType
                    && provider.providedType->getQName())
                ? provider.providedType->getQName()->toCanonical() : std::string("T");
            llvm::Function* findFn = module->getRuntimeFunction("__cajeta_anchor_find");
            llvm::Function* acquireFn = module->getRuntimeFunction("__cajeta_anchor_acquire");
            if (!findFn || !acquireFn) {
                throw Exception("the runtime has no component-scope anchors",
                                "CAJETA_ERROR_INTERNAL");
            }
            llvm::Constant* scopeKey = CajetaModule::scopeKeyGlobal(
                lmod, provider.scopePub->qualified());
            llvm::Constant* productKey = CajetaModule::componentKeyGlobal(lmod, canonical);
            llvm::BasicBlock* inactive = llvm::BasicBlock::Create(ctx, "inactive", llvmFunction);
            llvm::BasicBlock* active = llvm::BasicBlock::Create(ctx, "active", llvmFunction);
            llvm::Value* frame = builder->CreateCall(findFn, {scopeKey}, "frame");
            builder->CreateCondBr(builder->CreateICmpEQ(frame, nullPtr, "noframe"),
                                  inactive, active);
            builder->SetInsertPoint(inactive);
            emitThrowStdlibException(module, "cajeta.error.ScopeNotActiveException",
                "product " + canonical + " has scope \"" + provider.scopePub->qualified()
                    + "\", and no activation of it is active");
            builder->SetInsertPoint(active);
            {
                llvm::IRBuilder<> eb(entry, entry->begin());
                llvm::AllocaInst* a = eb.CreateAlloca(
                    llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), 48), nullptr, "claim");
                a->setAlignment(llvm::Align(8));
                claim = a;
            }
            llvm::Value* cur = builder->CreateCall(acquireFn, {frame, productKey, claim}, "cur");
            // 0 builds; 1 and 2 are failed or re-entered builds; 3 is a recorded
            // absent product (§11.6); anything else is the built product.
            llvm::BasicBlock* fresh = llvm::BasicBlock::Create(ctx, "fresh", llvmFunction);
            llvm::BasicBlock* cached = llvm::BasicBlock::Create(ctx, "cached", llvmFunction);
            llvm::BasicBlock* failed = llvm::BasicBlock::Create(ctx, "build_failed", llvmFunction);
            llvm::BasicBlock* reentered = llvm::BasicBlock::Create(ctx, "reentered", llvmFunction);
            llvm::BasicBlock* absent = llvm::BasicBlock::Create(ctx, "absent", llvmFunction);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            auto* i64 = llvm::cast<llvm::IntegerType>(i64Ty);
            llvm::SwitchInst* sw = builder->CreateSwitch(
                builder->CreatePtrToInt(cur, i64Ty), cached, 4);
            sw->addCase(llvm::ConstantInt::get(i64, 0), fresh);
            sw->addCase(llvm::ConstantInt::get(i64, 1), failed);
            sw->addCase(llvm::ConstantInt::get(i64, 2), reentered);
            sw->addCase(llvm::ConstantInt::get(i64, 3), absent);
            builder->SetInsertPoint(failed);
            emitThrowStdlibException(module, "cajeta.error.ScopedBuildFailedException",
                "the build of product " + canonical + " that this injection waited on failed");
            builder->SetInsertPoint(reentered);
            emitThrowStdlibException(module, "cajeta.error.ScopedBuildFailedException",
                "product " + canonical + " was injected again while it was being built");
            builder->SetInsertPoint(absent);
            builder->CreateRet(nullPtr);
            builder->SetInsertPoint(cached);
            builder->CreateRet(cur);
            builder->SetInsertPoint(fresh);
        }

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

        if (scoped) {
            // The scope's table owns the product from here and drops it at the
            // scope's end. A null product is recorded as absent (3), never dropped,
            // so the factory runs once per activation either way.
            llvm::Function* publishFn = module->getRuntimeFunction("__cajeta_anchor_publish");
            llvm::Function* dropFn = module->getRuntimeFunction("__cajeta_class_virtual_drop");
            if (!publishFn || !dropFn) {
                throw Exception("the runtime has no component-scope anchors",
                                "CAJETA_ERROR_INTERNAL");
            }
            llvm::BasicBlock* none = llvm::BasicBlock::Create(ctx, "none", llvmFunction);
            llvm::BasicBlock* some = llvm::BasicBlock::Create(ctx, "some", llvmFunction);
            builder->CreateCondBr(builder->CreateICmpEQ(product, nullPtr, "isnone"), none, some);
            builder->SetInsertPoint(none);
            llvm::Value* marker = builder->CreateIntToPtr(
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 3), ptrTy, "absent_marker");
            builder->CreateCall(publishFn, {claim, marker, nullPtr, nullPtr});
            builder->CreateRet(nullPtr);
            builder->SetInsertPoint(some);
            builder->CreateCall(publishFn, {claim, product, dropFn, nullPtr});
            builder->CreateRet(product);
            return;
        }
        if (isSingleton) {
            builder->CreateStore(product, singletonGlobal);
        }
        builder->CreateRet(product);
    }
}
