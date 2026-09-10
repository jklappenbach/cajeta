// AspectModel.md § A9 — the synthesized `static <C> __cajeta_inject()` body,
// emitted as raw IR: return the cached singleton when set, else malloc + vtable
// + constructor, inject each field, run @PostConstruct, cache, and return.

#include "ComponentInjectMethod.h"
#include "../type/CajetaClass.h"
#include "../type/StructureProperty.h"
#include "../util/MemoryManager.h"

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

        // Cached on the descriptor, so transitive callers share the one global.
        if (!descriptor->singletonGlobal) {
            std::string globalName =
                "__cajeta_singleton_" + parent->getQName()->toCanonical();
            descriptor->singletonGlobal = new llvm::GlobalVariable(
                *lmod, ptrTy, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrTy)),
                globalName);
        }
        llvm::GlobalVariable* singletonGV = descriptor->singletonGlobal;

        llvm::BasicBlock* entry =
            llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::BasicBlock* cached =
            llvm::BasicBlock::Create(ctx, "cached", llvmFunction);
        llvm::BasicBlock* fresh =
            llvm::BasicBlock::Create(ctx, "fresh", llvmFunction);

        builder = new llvm::IRBuilder<>(entry);
        module->setBuilder(builder);
        module->setCurrentMethod(shared_from_this());

        llvm::Value* current =
            builder->CreateLoad(ptrTy, singletonGV, "cur");
        llvm::Value* isNull = builder->CreateICmpEQ(
            current, llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrTy)),
            "isnull");
        builder->CreateCondBr(isNull, fresh, cached);

        builder->SetInsertPoint(cached);
        builder->CreateRet(current);

        builder->SetInsertPoint(fresh);

        llvm::Type* structTy = parent->getLlvmType();
        const llvm::DataLayout& dl = lmod->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx),
            dl.getTypeAllocSize(structTy));
        llvm::CallInst* instance = MemoryManager::createMallocInstruction(
            module, allocSize, fresh);

        // Vtable init — slot 0 of the new instance.
        if (auto vtable = parent->getVirtualTableGlobal()) {
            llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                lmod, vtable);
            llvm::Value* vtableSlot = builder->CreateStructGEP(
                structTy, instance, /*idx=*/0, "vtable_slot");
            builder->CreateStore(vtableRef, vtableSlot);
        }

        std::string ctorName = parent->getQName()->getTypeName();
        std::vector<ParameterEntry> noArgs;
        parent->invokeMethod(ctorName, noArgs,
                             /*isConstructor=*/true, instance,
                             /*callerModule=*/module);

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
            } else if (rd.allocate == CajetaModule::AllocateMode::Singleton) {
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
                // OwnerScope or Transient: mirror the alloc + vtable + ctor pattern
                // for this one field, with no singleton cache. Single-level in v1 —
                // the fresh target's own @Inject fields are NOT walked here.
                CajetaClassPtr targetClass = rd.target->klass;
                llvm::Type* targetStructTy = targetClass->getLlvmType();
                llvm::Constant* targetSize = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx),
                    dl.getTypeAllocSize(targetStructTy));
                llvm::CallInst* freshInst = MemoryManager::createMallocInstruction(
                    module, targetSize, builder->GetInsertBlock());
                if (auto vt = targetClass->getVirtualTableGlobal()) {
                    llvm::Constant* vtRef = CajetaModule::ensureGlobalInModule(
                        lmod, vt);
                    llvm::Value* vts = builder->CreateStructGEP(
                        targetStructTy, freshInst, /*idx=*/0, "vtable_slot");
                    builder->CreateStore(vtRef, vts);
                }
                std::string ctorN = targetClass->getQName()->getTypeName();
                std::vector<ParameterEntry> noArgs2;
                targetClass->invokeMethod(ctorN, noArgs2,
                                          /*isConstructor=*/true, freshInst,
                                          /*callerModule=*/module);
                depPtr = freshInst;
            }

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
                        (uint64_t) IFACE_KIND_BORROWED_CLASS),
                    kindSlot);
            } else {
                builder->CreateStore(depPtr, slot);
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

        // @PreDestroy registration: the user method's `void (this:pointer)` is
        // ABI-compatible with the `void (*)(void*)` the atexit registry expects, so
        // no thunk. Registered on the fresh path only, so a singleton registers once.
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->findAnnotation("PreDestroy")) continue;
            if (m->getModifiers().find(STATIC) != m->getModifiers().end()) {
                continue;
            }
            llvm::Function* userFn = m->getLlvmFunction();
            if (!userFn) {
                // Force prototype generation so the function pointer exists.
                m->getLlvmFunctionType();
                userFn = m->getLlvmFunction();
            }
            if (!userFn) break;
            llvm::Function* pushFn = module->getRuntimeFunction("__cajeta_atexit_push");
            if (!pushFn) break;
            builder->CreateCall(pushFn, {userFn, instance});
            break;
        }

        builder->CreateStore(instance, singletonGV);
        builder->CreateRet(instance);
    }
}
