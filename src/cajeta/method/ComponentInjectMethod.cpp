// AspectModel.md § A9 — the synthesized `static <C> __cajeta_inject()` body,
// emitted as raw IR: return the cached singleton when set, else malloc + vtable
// + constructor, inject each field, run @PostConstruct, cache, and return.

#include "ComponentInjectMethod.h"
#include "../type/CajetaClass.h"
#include "../type/StructureProperty.h"
#include "../util/MemoryManager.h"
#include "../asn/expression/LiteralExpression.h"
#include "../type/CajetaType.h"
#include "../error/Exception.h"
#include "../asn/expression/Expression.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

namespace cajeta {
    // The synthesized method is added AFTER the class's generatePrototype has run,
    // so the base constructor's uninitialized llvmFunction/llvmFunctionType are
    // nulled here for the lazy prototype path to fire. Static: it takes no `this`.
    ComponentInjectMethod::ComponentInjectMethod(
        CajetaModulePtr module,
        CajetaClassPtr parent,
        CajetaModule::ComponentDescriptorPtr descriptor)
        : Method(module, std::string("__cajeta_inject"), parent, parent),
          descriptor(std::move(descriptor)) {
        llvmFunctionType = nullptr;
        llvmFunction = nullptr;
        addModifier(STATIC);
    }

    // Emit the body: entry loads the singleton and branches, `cached` returns it,
    // `fresh` allocates, constructs, injects the fields, caches and returns.
    void ComponentInjectMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Constant* nullPtr = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));
        const std::string canonical = parent->getQName()->toCanonical();
        const bool publishedScope = descriptor->scope
            && descriptor->scope->kind != CajetaModule::ScopePublication::Kind::Builtin;

        const bool transient = isTransient();
        llvm::GlobalVariable* singletonGV = nullptr;
        if (!publishedScope && !transient) {
            // Cached on the descriptor, so transitive callers share the one global.
            if (!descriptor->singletonGlobal) {
                descriptor->singletonGlobal = new llvm::GlobalVariable(
                    *lmod, ptrTy, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage, nullPtr,
                    "__cajeta_singleton_" + canonical);
            }
            singletonGV = descriptor->singletonGlobal;
        }

        llvm::BasicBlock* entry =
            llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::BasicBlock* cached =
            llvm::BasicBlock::Create(ctx, "cached", llvmFunction);
        llvm::BasicBlock* fresh =
            llvm::BasicBlock::Create(ctx, "fresh", llvmFunction);

        builder = new llvm::IRBuilder<>(entry);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        // A scoped component lives in the innermost active frame of its scope; the
        // lookup fails with ScopeNotActiveException when no frame is active. A first
        // build claims its slot, so a concurrent injection parks until it is done.
        llvm::Value* claim = nullptr;
        auto claimSlot = [&]() {
            llvm::IRBuilder<> eb(entry, entry->begin());
            llvm::AllocaInst* a = eb.CreateAlloca(
                llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), 48), nullptr, "claim");
            a->setAlignment(llvm::Align(8));
            return a;
        };
        // Routes an acquire result: 0 builds, 1 and 2 fail, anything else is built.
        auto dispatch = [&](llvm::Value* cur) {
            llvm::BasicBlock* waitFailed =
                llvm::BasicBlock::Create(ctx, "build_failed", llvmFunction);
            llvm::BasicBlock* reentered =
                llvm::BasicBlock::Create(ctx, "reentered", llvmFunction);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::SwitchInst* sw = builder->CreateSwitch(
                builder->CreatePtrToInt(cur, i64Ty), cached, 3);
            sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64Ty), 0), fresh);
            sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64Ty), 1), waitFailed);
            sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64Ty), 2), reentered);
            builder->SetInsertPoint(waitFailed);
            emitThrowStdlibException(module, "cajeta.error.ScopedBuildFailedException",
                "the build of component " + canonical + " that this injection waited on failed");
            builder->SetInsertPoint(reentered);
            emitThrowStdlibException(module, "cajeta.error.ScopedBuildFailedException",
                "component " + canonical + " was injected again while it was being built, "
                "a cycle through Scoped.get()");
            builder->SetInsertPoint(cached);
            builder->CreateRet(cur);
        };
        if (publishedScope) {
            llvm::Function* findFn = module->getRuntimeFunction("__cajeta_anchor_find");
            llvm::Function* acquireFn = module->getRuntimeFunction("__cajeta_anchor_acquire");
            if (!findFn || !acquireFn) {
                throw Exception("the runtime has no component-scope anchors",
                                "CAJETA_ERROR_INTERNAL");
            }
            llvm::Constant* scopeKey = CajetaModule::scopeKeyGlobal(
                llvmFunction->getParent(), descriptor->scope->qualified());
            llvm::Constant* componentKey = CajetaModule::componentKeyGlobal(
                llvmFunction->getParent(), canonical);
            llvm::BasicBlock* inactive =
                llvm::BasicBlock::Create(ctx, "inactive", llvmFunction);
            llvm::BasicBlock* active =
                llvm::BasicBlock::Create(ctx, "active", llvmFunction);
            llvm::Value* frame = builder->CreateCall(findFn, {scopeKey}, "frame");
            builder->CreateCondBr(builder->CreateICmpEQ(frame, nullPtr, "noframe"),
                                  inactive, active);
            builder->SetInsertPoint(inactive);
            emitThrowStdlibException(module, "cajeta.error.ScopeNotActiveException",
                "component " + canonical + " has scope \"" + descriptor->scope->qualified()
                    + "\", and no activation of it is active");
            builder->SetInsertPoint(active);
            claim = claimSlot();
            dispatch(builder->CreateCall(acquireFn, {frame, componentKey, claim}, "cur"));
        } else if (!transient) {
            llvm::Function* acquireFn = module->getRuntimeFunction("__cajeta_singleton_acquire");
            if (!acquireFn) {
                throw Exception("the runtime has no singleton claim", "CAJETA_ERROR_INTERNAL");
            }
            llvm::LoadInst* fast = builder->CreateLoad(ptrTy, singletonGV, "cur");
            fast->setAtomic(llvm::AtomicOrdering::Acquire);
            fast->setAlignment(llvm::Align(8));
            llvm::BasicBlock* hit = llvm::BasicBlock::Create(ctx, "hit", llvmFunction);
            llvm::BasicBlock* slow = llvm::BasicBlock::Create(ctx, "slow", llvmFunction);
            builder->CreateCondBr(builder->CreateICmpEQ(fast, nullPtr, "isnull"), slow, hit);
            builder->SetInsertPoint(hit);
            builder->CreateRet(fast);
            builder->SetInsertPoint(slow);
            claim = claimSlot();
            dispatch(builder->CreateCall(acquireFn, {singletonGV, claim}, "cur"));
        } else {
            builder->CreateBr(fresh);
            cached->eraseFromParent();
        }

        builder->SetInsertPoint(fresh);

        // A scoped component with no no-argument constructor is only ever provided.
        bool buildable = false;
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->isConstructor()) continue;
            size_t formals = 0;
            for (auto& p : m->getParameterList()) {
                if (p && p->getName() != "this") formals++;
            }
            if (formals == 0) {
                buildable = true;
                break;
            }
        }
        if (publishedScope && !buildable) {
            emitThrowStdlibException(module, "cajeta.error.NotProvidedException",
                "component " + canonical + " in scope \"" + descriptor->scope->qualified()
                    + "\" has no no-argument constructor, so Components.provide must place it "
                      "before it is injected, and it has not");
            return;
        }

        llvm::Type* structTy = parent->getLlvmType();
        const llvm::DataLayout& dl = lmod->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx),
            dl.getTypeAllocSize(structTy));
        llvm::CallInst* instance = MemoryManager::createMallocInstruction(
            module, allocSize, fresh);
        // Zeroed, with every vtable slot set: a scoped component is dropped at its
        // scope's end, and the drop reads the ownership word.
        parent->initInstanceLayout(module, instance, structTy, /*stackAlloc=*/false);
        std::string ctorName = parent->getQName()->getTypeName();
        std::vector<ParameterEntry> noArgs;
        parent->invokeMethod(ctorName, noArgs,
                             /*isConstructor=*/true, instance,
                             /*callerModule=*/module);

        // Sets `field`'s bit in the ownership word, so the holder's drop frees it.
        auto markOwned = [&](const StructurePropertyPtr& field) {
            int bit = parent->ownershipBitIndexOf(field);
            int wordIdx = parent->getOwnershipWordLlvmIndex();
            if (bit < 0 || wordIdx < 0) return;
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Value* wordSlot = builder->CreateStructGEP(
                structTy, instance, (unsigned) wordIdx, "own_bits_slot");
            llvm::Value* word = builder->CreateLoad(i64Ty, wordSlot);
            builder->CreateStore(
                builder->CreateOr(word, llvm::ConstantInt::get(i64Ty, 1ULL << bit)), wordSlot);
        };
        // Field injection, by the resolved dependency's allocate mode: Singleton
        // calls the target's __cajeta_inject and stores the shared pointer;
        // OwnerScope/Transient allocate inline; an optional non-match stores null.
        for (auto& rd : descriptor->resolvedFields) {
            if (!rd.field) continue;

            unsigned fieldIdx =
                (unsigned) parent->getFieldLlvmIndex(rd.field);
            llvm::Value* slot = builder->CreateStructGEP(
                structTy, instance, fieldIdx,
                rd.field->getName() + "_slot");
            if (rd.multi != CajetaModule::ResolvedDependency::MultiKind::None) {
                builder->CreateStore(emitMultibinding(rd), slot);
                continue;
            }
            if (rd.handle) {
                std::vector<ParameterEntry> none;
                builder->CreateStore(rd.container->heapConstruct(module, none), slot);
                markOwned(rd.field);
                continue;
            }

            // An interface-typed field is a 24-byte fat-pointer body inline in the
            // instance, not an 8-byte cell: compute depPtr, then write three words
            // for an interface field or one for a plain class reference.
            CajetaClassPtr fieldIface;
            if (auto fc = std::dynamic_pointer_cast<CajetaClass>(rd.field->getType())) {
                if (fc->isInterface()) fieldIface = fc;
            }

            llvm::Value* depPtr = nullptr;
            if (rd.factory) {
                // Factory-provided product: the synthesized provider accessor owns
                // the scope, and yields the product pointer to store in the field.
                auto& prov = rd.factory->providers[rd.providerIdx];
                if (prov.accessor) {
                    prov.accessor->getLlvmFunctionType();   // force prototype
                    llvm::Function* accFn = CajetaModule::ensureFunctionVisible(
                        builder, prov.accessor->getLlvmFunction(),
                        prov.accessor->getLlvmFunctionType());
                    depPtr = builder->CreateCall(
                        prov.accessor->getLlvmFunctionType(), accFn, {},
                        rd.field->getName() + "_dep");
                } else {
                    depPtr = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy));
                }
            } else if (!rd.target || !rd.target->klass) {
                depPtr = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrTy));
            } else if (rd.allocate == CajetaModule::AllocateMode::Singleton
                    || rd.allocate == CajetaModule::AllocateMode::Scoped) {
                MethodPtr targetInject;
                for (auto& [mkey, m] : rd.target->klass->getMethods()) {
                    if (m && m->getName() == "__cajeta_inject") {
                        targetInject = m;
                        break;
                    }
                }
                if (!targetInject) continue;
                // Cross-module: ensureFunctionVisible declares the target in the
                // calling module. Force the TYPE first, in its own statement —
                // getLlvmFunction() is raw and null until the prototype exists.
                llvm::FunctionType* targetTy =
                    targetInject->getLlvmFunctionType();
                llvm::Function* targetFn = CajetaModule::ensureFunctionVisible(
                    builder, targetInject->getLlvmFunction(), targetTy);
                depPtr = builder->CreateCall(
                    targetTy,
                    targetFn,
                    {},
                    rd.field->getName() + "_dep");
            } else {
                // A target that declares Transient builds fully through its own
                // accessor. Otherwise the site asked for a fresh instance of an
                // undeclared component: one level, its own @Inject fields unfilled.
                CajetaClassPtr targetClass = rd.target->klass;
                MethodPtr targetInject;
                if (rd.target->scope && rd.target->scope->name == "Transient"
                        && rd.target->scope->kind
                               == CajetaModule::ScopePublication::Kind::Builtin) {
                    for (auto& [mkey, m] : targetClass->getMethods()) {
                        if (m && m->getName() == "__cajeta_inject") {
                            targetInject = m;
                            break;
                        }
                    }
                }
                if (targetInject) {
                    llvm::FunctionType* targetTy = targetInject->getLlvmFunctionType();
                    llvm::Function* targetFn = CajetaModule::ensureFunctionVisible(
                        builder, targetInject->getLlvmFunction(), targetTy);
                    depPtr = builder->CreateCall(targetTy, targetFn, {},
                                                 rd.field->getName() + "_dep");
                } else {
                    llvm::Type* targetStructTy = targetClass->getLlvmType();
                    llvm::Constant* targetSize = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(ctx),
                        dl.getTypeAllocSize(targetStructTy));
                    llvm::CallInst* freshInst = MemoryManager::createMallocInstruction(
                        module, targetSize, builder->GetInsertBlock());
                    targetClass->initInstanceLayout(module, freshInst, targetStructTy,
                                                    /*stackAlloc=*/false);
                    std::string ctorN = targetClass->getQName()->getTypeName();
                    std::vector<ParameterEntry> noArgs2;
                    targetClass->invokeMethod(ctorN, noArgs2,
                                              /*isConstructor=*/true, freshInst,
                                              /*callerModule=*/module);
                    depPtr = freshInst;
                }
            }
            // A fresh instance belongs to the holder, which frees it in its own drop.
            const bool freshOwned = rd.target && !rd.factory
                && (rd.allocate == CajetaModule::AllocateMode::OwnerScope
                    || rd.allocate == CajetaModule::AllocateMode::Transient);

            // Test-only @Inject override (DI-override-hook.md): in a test build a
            // TestContext binding, keyed by the field type's reflect.Class object,
            // wins over the static provider. v1: singleton, class-typed fields.
            if (CajetaModule::getActiveProfile() == "test"
                    && !fieldIface
                    && rd.target && rd.target->klass
                    && rd.allocate == CajetaModule::AllocateMode::Singleton) {
                CajetaClassPtr fieldType =
                    std::dynamic_pointer_cast<CajetaClass>(rd.field->getType());
                llvm::GlobalVariable* classObjG =
                    fieldType ? fieldType->getClassObjectGlobal() : nullptr;
                if (classObjG) {
                    llvm::Constant* classObjRef =
                        CajetaModule::ensureGlobalInModule(lmod, classObjG);
                    llvm::FunctionType* getFnTy =
                        llvm::FunctionType::get(ptrTy, {ptrTy}, false);
                    llvm::FunctionCallee getFn = lmod->getOrInsertFunction(
                        "__cajeta_inject_override_get", getFnTy);
                    llvm::Value* ovr = builder->CreateCall(
                        getFn, {classObjRef}, rd.field->getName() + "_ovr");
                    llvm::Value* hasOvr = builder->CreateICmpNE(
                        ovr, llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy)),
                        rd.field->getName() + "_hasovr");
                    depPtr = builder->CreateSelect(
                        hasOvr, ovr, depPtr,
                        rd.field->getName() + "_sel");
                }
            }

            if (fieldIface) {
                // The fat pointer, written in place: word 0 data, word 1 the
                // per-(class, iface) vtable, word 2 kind = IFACE_KIND_BORROWED_CLASS
                // — the singleton cache owns the lifecycle, so no holder may drop it.
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
                llvm::Type* fatTy = fieldIface->getLlvmType();
                llvm::Value* dataSlot = builder->CreateStructGEP(
                    fatTy, slot, 0, rd.field->getName() + "_data");
                llvm::Value* vtableSlot = builder->CreateStructGEP(
                    fatTy, slot, 1, rd.field->getName() + "_vtable");
                llvm::Value* kindSlot = builder->CreateStructGEP(
                    fatTy, slot, 2, rd.field->getName() + "_kind");
                builder->CreateStore(depPtr, dataSlot);

                std::string ifaceCanonical =
                    fieldIface->getQName()->toCanonical();
                // The implementing class: the resolved @Component, or for a
                // factory-provided field the provider's product type.
                CajetaClassPtr underlying;
                if (rd.target) {
                    underlying = rd.target->klass;
                } else if (rd.factory) {
                    underlying = std::dynamic_pointer_cast<CajetaClass>(
                        rd.factory->providers[rd.providerIdx].providedType);
                }
                llvm::Constant* vtableRef = nullptr;
                if (underlying) {
                    if (auto gv = underlying->getInterfaceVTable(ifaceCanonical)) {
                        vtableRef = CajetaModule::ensureGlobalInModule(lmod, gv);
                    }
                }
                if (!vtableRef) {
                    vtableRef = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy));
                }
                builder->CreateStore(vtableRef, vtableSlot);
                builder->CreateStore(
                    llvm::ConstantInt::get(i64Ty,
                        (uint64_t) (freshOwned ? IFACE_KIND_OWNED_CLASS
                                               : IFACE_KIND_BORROWED_CLASS)),
                    kindSlot);
            } else {
                builder->CreateStore(depPtr, slot);
                if (freshOwned) markOwned(rd.field);
            }
        }

        // @PostConstruct (AspectModel.md § A11) runs once every @Inject field is
        // assigned and before the cache store below, so it fires exactly once per
        // singleton. v1 takes the first hook in declaration order.
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->findAnnotation("PostConstruct")) continue;
            // A static hook would mismatch the `this` dispatch, so skip it.
            if (m->getModifiers().find(STATIC) != m->getModifiers().end()) {
                continue;
            }
            std::vector<ParameterEntry> hookArgs;
            std::string hookName = m->getName();
            parent->invokeMethod(hookName, hookArgs,
                                 /*isConstructor=*/false, instance,
                                 /*callerModule=*/module);
            break;
        }

        // The @PreDestroy hook, whose `void (this:pointer)` is ABI-compatible with the
        // `void (*)(void*)` both the atexit registry and a scope table expect.
        llvm::Value* preDestroy = nullptr;
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->findAnnotation("PreDestroy")) continue;
            if (m->getModifiers().find(STATIC) != m->getModifiers().end()) {
                continue;
            }
            llvm::FunctionType* hookTy = m->getLlvmFunctionType();
            if (m->getLlvmFunction()) {
                preDestroy = CajetaModule::ensureFunctionVisible(
                    builder, m->getLlvmFunction(), hookTy);
            }
            break;
        }

        if (publishedScope) {
            // The scope's table owns the instance from here, and ends it at the
            // scope's end: @PreDestroy, then the virtual drop.
            llvm::Function* publishFn = module->getRuntimeFunction("__cajeta_anchor_publish");
            llvm::Function* dropFn = module->getRuntimeFunction("__cajeta_class_virtual_drop");
            if (!publishFn || !dropFn) {
                throw Exception("the runtime has no component-scope anchors",
                                "CAJETA_ERROR_INTERNAL");
            }
            builder->CreateCall(publishFn, {claim, instance, dropFn,
                                            preDestroy ? preDestroy : nullPtr});
            builder->CreateRet(instance);
            return;
        }

        // A transient instance belongs to the caller, which frees it.
        if (transient) {
            builder->CreateRet(instance);
            return;
        }

        // Registered on the fresh path only, so a singleton registers once.
        if (preDestroy) {
            if (llvm::Function* pushFn = module->getRuntimeFunction("__cajeta_atexit_push")) {
                builder->CreateCall(pushFn, {preDestroy, instance});
            }
        }

        llvm::StoreInst* store = builder->CreateStore(instance, singletonGV);
        store->setAtomic(llvm::AtomicOrdering::Release);
        store->setAlignment(llvm::Align(8));
        if (llvm::Function* publishFn = module->getRuntimeFunction("__cajeta_singleton_publish")) {
            builder->CreateCall(publishFn, {claim, instance});
        }
        builder->CreateRet(instance);
    }
}

namespace cajeta {
    // The target's singleton through its own __cajeta_inject, or null when the
    // descriptor has no helper yet.
    llvm::Value* ComponentInjectMethod::singletonOf(
            const CajetaModule::ComponentDescriptorPtr& target) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        MethodPtr targetInject;
        if (target && target->klass) {
            for (auto& [mkey, m] : target->klass->getMethods()) {
                if (m && m->getName() == "__cajeta_inject") {
                    targetInject = m;
                    break;
                }
            }
        }
        if (!targetInject) {
            return llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrTy));
        }
        llvm::FunctionType* targetTy = targetInject->getLlvmFunctionType();
        llvm::Function* targetFn = CajetaModule::ensureFunctionVisible(
            builder, targetInject->getLlvmFunction(), targetTy);
        return builder->CreateCall(targetTy, targetFn, {}, "member");
    }

    // Build the site's container and fill it with every member's singleton. The
    // calls carry no transfer word, so the container records borrows and its
    // drop frees no member: the graph owns them.
    llvm::Value* ComponentInjectMethod::emitMultibinding(
            const CajetaModule::ResolvedDependency& rd) {
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();
        CajetaClassPtr cont = rd.container;
        llvm::Type* contTy = cont->getLlvmType();
        llvm::Constant* size = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx), dl.getTypeAllocSize(contTy));
        llvm::CallInst* container = MemoryManager::createMallocInstruction(
            module, size, builder->GetInsertBlock());
        if (auto vt = cont->getVirtualTableGlobal()) {
            llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(lmod, vt);
            llvm::Value* vts = builder->CreateStructGEP(
                contTy, container, 0, "vtable_slot");
            builder->CreateStore(vtRef, vts);
        }
        std::string ctorName = cont->getTemplateOrigin()
            ? cont->getTemplateOrigin()->getQName()->getTypeName()
            : cont->getQName()->getTypeName();
        std::vector<ParameterEntry> noArgs;
        cont->invokeMethod(ctorName, noArgs, /*isConstructor=*/true, container,
                           /*callerModule=*/module);
        CajetaTypePtr stringTy = CajetaType::of("String");
        for (auto& member : rd.members) {
            if (!member || !member->klass) continue;
            llvm::Value* ptr = singletonOf(member);
            if (rd.multi == CajetaModule::ResolvedDependency::MultiKind::List) {
                std::string addName = "add";
                std::vector<ParameterEntry> args;
                args.emplace_back(member->klass, "", ptr);
                cont->invokeMethod(addName, args, /*isConstructor=*/false,
                                   container, /*callerModule=*/module);
            } else {
                std::string key = member->name.empty()
                    ? member->klass->getQName()->getTypeName() : member->name;
                TextLiteralExpression lit("\"" + key + "\"", LITERAL_TYPE_STRING);
                lit.resolveTypes(module);
                llvm::Value* keyVal = lit.generateCode(module);
                std::string putName = "put";
                std::vector<ParameterEntry> args;
                args.emplace_back(stringTy, "", keyVal);
                args.emplace_back(member->klass, "", ptr);
                cont->invokeMethod(putName, args, /*isConstructor=*/false,
                                   container, /*callerModule=*/module);
            }
        }
        return container;
    }
}
