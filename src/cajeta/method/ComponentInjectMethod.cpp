//
// AspectModel.md § A9: synthesized __cajeta_inject() body.
//
// Pattern:
//   static <C> __cajeta_inject() {
//       if (__cajeta_singleton_<C> != null) return __cajeta_singleton_<C>;
//       <C> instance = malloc(sizeof <C>);
//       instance.vtable = &__vtable_<C>;
//       <C>::<C>(instance);                          // constructor
//       instance.fieldA = <FieldA>::__cajeta_inject();
//       ...
//       __cajeta_singleton_<C> = instance;
//       return instance;
//   }
//
// Emitted directly as IR rather than through an AST tree. The
// branch is shaped as:
//
//   entry:
//     %cur = load ptr, @__cajeta_singleton_<C>
//     %hit = icmp ne ptr %cur, null
//     br i1 %hit, label %cached, label %fresh
//   cached:
//     ret ptr %cur
//   fresh:
//     ... allocate + ctor + injections + store ...
//     ret ptr %inst
//

#include "ComponentInjectMethod.h"
#include "../type/CajetaClass.h"
#include "../type/StructureProperty.h"
#include "../util/MemoryManager.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

namespace cajeta {
    ComponentInjectMethod::ComponentInjectMethod(
        CajetaModulePtr module,
        CajetaClassPtr parent,
        CajetaModule::ComponentDescriptorPtr descriptor)
        : Method(module, std::string("__cajeta_inject"), parent, parent),
          descriptor(std::move(descriptor)) {
        // Method's base constructor leaves llvmFunction /
        // llvmFunctionType uninitialized — user-written methods rely
        // on CajetaClass::generatePrototype calling generatePrototype
        // on each method right after construction. Our synthesized
        // method is added AFTER the class's generatePrototype has
        // run (during resolveDependencyGraph), so we'd inherit
        // garbage pointers and skip the lazy prototype-build path
        // (getLlvmFunctionType only triggers generatePrototype when
        // its cached pointer is null). Clear them explicitly here
        // so the lazy path works for our class.
        llvmFunctionType = nullptr;
        llvmFunction = nullptr;
        // Static; no `this`. The Method base treats static methods
        // correctly during generatePrototype (skips the implicit
        // this param).
        addModifier(STATIC);
    }

    void ComponentInjectMethod::generateCode() {
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Lazy-create the module-level singleton storage on the
        // first generateCode for this descriptor. Cached on the
        // descriptor so transitive callers reuse the same global.
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

        // Entry block: load singleton, branch on null check.
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

        // Cached path — return existing singleton.
        builder->SetInsertPoint(cached);
        builder->CreateRet(current);

        // Fresh path — alloc + ctor + inject fields + cache + return.
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

        // Constructor — invokeMethod dispatches the matching ctor
        // (default or user-declared) with no user-supplied args.
        // The instance pointer is the `this`. Returns nothing
        // structured we need; we keep the instance ptr from malloc.
        std::string ctorName = parent->getQName()->getTypeName();
        std::vector<ParameterEntry> noArgs;
        parent->invokeMethod(ctorName, noArgs,
                             /*isConstructor=*/true, instance,
                             /*callerModule=*/module);

        // Field injection. The IR shape depends on the resolved
        // dependency's allocate mode:
        //
        //   Singleton (default) — call target.__cajeta_inject() and
        //     store the cached singleton pointer. Repeated injection
        //     sites share the same instance.
        //
        //   OwnerScope / Transient — emit fresh alloc + vtable +
        //     ctor inline; the resulting instance is owned by the
        //     containing class. Drop-chain hookup of these owner-
        //     scoped allocations is a v1 known gap (leak at owner
        //     destruction). At field-level granularity the two
        //     modes collapse to the same shape; they diverge for
        //     constructor-parameter injection which isn't wired.
        //
        //   Optional with null target — no candidate matched and
        //     the user marked the field optional. Store a null
        //     pointer; user code reads-and-checks.
        for (auto& rd : descriptor->resolvedFields) {
            if (!rd.field) continue;

            unsigned fieldIdx =
                (unsigned) parent->getFieldLlvmIndex(rd.field);
            llvm::Value* slot = builder->CreateStructGEP(
                structTy, instance, fieldIdx,
                rd.field->getName() + "_slot");

            // S9.5.3 — if the field's declared type is an interface, the
            // slot is a 24-byte fat-pointer body inline in the instance,
            // not an 8-byte pointer cell. Compute depPtr as before
            // (the underlying class instance), then either write three
            // words (interface field) or one word (plain class ref).
            CajetaClassPtr fieldIface;
            if (auto fc = std::dynamic_pointer_cast<CajetaClass>(rd.field->getType())) {
                if (fc->isInterface()) fieldIface = fc;
            }

            llvm::Value* depPtr = nullptr;
            if (!rd.target || !rd.target->klass) {
                // Optional injection with no candidate → store null.
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
                // Cross-module call: in a multi-source compile the
                // target's __cajeta_inject lives in the target's
                // llvm::Module, not the calling (owner) class's
                // module. ensureFunctionVisible inserts a
                // declaration in the calling module when needed so
                // the resulting CreateCall references a module-
                // local Function (the JIT links definitions across
                // modules by symbol name at load time).
                llvm::Function* targetFn = CajetaModule::ensureFunctionVisible(
                    builder, targetInject->getLlvmFunction(),
                    targetInject->getLlvmFunctionType());
                depPtr = builder->CreateCall(
                    targetInject->getLlvmFunctionType(),
                    targetFn,
                    {},
                    rd.field->getName() + "_dep");
            } else {
                // OwnerScope or Transient — fresh alloc per field.
                // Mirror the alloc + vtable + ctor pattern from
                // __cajeta_inject's fresh path, scoped to this
                // single field. No singleton cache, no atexit
                // registration. Field-injected dependencies of
                // the target (the target's own @Inject fields) are
                // NOT walked here — those would recurse via the
                // target's __cajeta_inject if SINGLETON, or via a
                // dedicated make_X helper for non-SINGLETON
                // nested. v1 single-level: the freshly alloc'd
                // target's fields stay zero-initialized except
                // for what the user constructor sets. Tests in
                // this commit avoid deep dependency graphs under
                // non-SINGLETON modes.
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

            if (fieldIface) {
                // Interface field: write the 24-byte fat pointer in place.
                // word 0 = data (depPtr — underlying class instance)
                // word 1 = vtable (per-(target class, iface) global from S9.5.2)
                // word 2 = kind = IFACE_KIND_BORROWED_CLASS
                //   DI singletons are conceptually borrowed by every
                //   holder — the singleton-cache global owns the lifecycle;
                //   the holder must not drop on scope exit. OWNED kinds
                //   become reachable in Session 10.
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
                llvm::Constant* vtableRef = nullptr;
                if (rd.target && rd.target->klass) {
                    if (auto gv = rd.target->klass->getInterfaceVTable(ifaceCanonical)) {
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

        // @PostConstruct dispatch (AspectModel.md § A11). The spec
        // is clear that the user's hook runs after every @Inject
        // field has been assigned — so it lands here, between field
        // injection above and the singleton-cache store below. The
        // hook must be an instance method (gets `this`), with no
        // user-facing parameters. The cached entry below ensures
        // the hook fires exactly once per singleton.
        //
        // v1 takes the first @PostConstruct in declaration order;
        // multiple hooks on one class are reserved for a later
        // commit (spec doesn't yet enumerate ordering rules).
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->findAnnotation("PostConstruct")) continue;
            // Skip statics — @PostConstruct on a static makes no
            // sense, and dispatching with `this` would mismatch
            // the signature. The Method base passes static via the
            // STATIC modifier set; consult that.
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

        // @PreDestroy registration (AspectModel.md § A11 follow-up).
        // The user's @PreDestroy method has signature
        // `void (this:pointer)`, which is ABI-compatible with the
        // C `void (*)(void*)` the atexit registry expects — no
        // thunk synthesis needed. Register only on the fresh path
        // so a singleton lands in the registry exactly once,
        // matching the @PostConstruct firing pattern. The atexit
        // registry stays in the runtime's stable memory; tests
        // fire it explicitly via `Cajeta.runAtExit()`.
        for (auto& [mkey, m] : parent->getMethods()) {
            if (!m || !m->findAnnotation("PreDestroy")) continue;
            if (m->getModifiers().find(STATIC) != m->getModifiers().end()) {
                continue;
            }
            llvm::Function* userFn = m->getLlvmFunction();
            if (!userFn) {
                // Force prototype generation so the function pointer
                // exists by the time the inject helper is called.
                m->getLlvmFunctionType();
                userFn = m->getLlvmFunction();
            }
            if (!userFn) break;   // give up — leave compile-time error to the spec follow-up
            llvm::Function* pushFn = module->getRuntimeFunction("__cajeta_atexit_push");
            if (!pushFn) break;
            builder->CreateCall(pushFn, {userFn, instance});
            break;
        }

        // Cache and return the fresh instance.
        builder->CreateStore(instance, singletonGV);
        builder->CreateRet(instance);
    }
}
