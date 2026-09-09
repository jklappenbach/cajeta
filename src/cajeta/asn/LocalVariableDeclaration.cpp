//
// Created by James Klappenbach on 11/4/22.
//

#include "LocalVariableDeclaration.h"
#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../compile/ScriptUnitSynthesis.h"
#include "cajeta/ownership/OwnedBindCheck.h"
#include "cajeta/ownership/ReturnTitleAudit.h"
#include "cajeta/ownership/TitleClassifier.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../field/ParameterField.h"
#include "../field/StackField.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaFunctionType.h"
#include "expression/Expression.h"
#include "Statement.h"
#include "expression/LiteralExpression.h"
#include "expression/DotExpression.h"
#include "expression/Identifier.h"
#include "expression/MethodCallExpression.h"
#include "expression/CallExpression.h"
#include "../method/Method.h"
#include "expression/BinaryOpExpression.h"
#include "expression/NewExpression.h"
#include "expression/CreatorRest.h"
#include "expression/AggregateInitializerExpression.h"
#include "../method/Method.h"
#include "../error/CajetaExceptions.h"
#include "../logging/CajetaLogger.h"

namespace cajeta {
    // Sizes in bytes of the runtime's drop-entry structs on the target
    // platform. Release shape is 32 bytes (obj + drop_fn + prev + active +
    // padding); debug shape is 40 (release + alloc_line + alloc_file). The
    // compiler picks which size to allocate based on CompilerFlags::sourceTags.
    // Runtime exposes __cajeta_drop_entry_size() / _size_debug() if these
    // ever need to be validated against the actual C struct sizes.
    static constexpr unsigned DROP_ENTRY_BYTES = 32;
    static constexpr unsigned DROP_ENTRY_BYTES_DEBUG = 40;

    // Pick the right entry-allocation size + push helper name + push-arg
    // shape for the active CompilerFlags. When sourceTags is on, the
    // returned helper takes 5 args (entry, obj, drop_fn, alloc_file,
    // alloc_line); when off, the original 3-arg push.
    struct DropPushChoice {
        llvm::Function* pushFn;
        unsigned entryBytes;
        bool debug;
    };
    static DropPushChoice pickDropPush(CajetaModulePtr module) {
        DropPushChoice c{};
        if (module->getFlags().sourceTags) {
            c.pushFn = module->getRuntimeFunction("__cajeta_drop_push_debug");
            c.entryBytes = DROP_ENTRY_BYTES_DEBUG;
            c.debug = true;
        } else {
            c.pushFn = module->getRuntimeFunction("__cajeta_drop_push");
            c.entryBytes = DROP_ENTRY_BYTES;
            c.debug = false;
        }
        return c;
    }

    // Script units (script-units spec §4): a top-level owner in the
    // synthesized entry belongs to the SESSION, not the entry's drop frame.
    // When the declaring method is the script entry and the name is one of
    // the unit's collected session bindings, register the owner with the
    // runtime session registry — same obj, same drop fn a drop entry would
    // carry — and emit NO function-local entry, so the value survives the
    // entry's return and drops at __cajeta_session_drop_all (or earlier at a
    // rebind, which the runtime handles). Returns true when promoted.
    static bool maybeEmitSessionBind(CajetaModulePtr module, FieldPtr field,
                                     llvm::Value* dropFn) {
        if (!module->isScriptUnit()) return false;
        if (!module->isScriptBindingName(field->getName())) return false;
        // 4.2.4(b) — a block-local shadowing a binding NAME stays local.
        if (!module->isScriptEntryTopLevel()) return false;
        auto* builder = module->getBuilder();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (parentFn == nullptr
            || parentFn->getName().find(scriptEntryName())
                   == llvm::StringRef::npos) {
            return false;
        }
        llvm::Function* bindFn =
            module->getRuntimeFunction("__cajeta_session_bind");
        if (!bindFn) return false;
        auto& ctx = *module->getLlvmContext();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* nameStr =
            builder->CreateGlobalString(field->getName());
        llvm::Value* ownerPtr =
            builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        builder->CreateCall(bindFn, {nameStr, ownerPtr, dropFn});
        field->setSessionBound(true);
        return true;
    }

    // Script units (script-units spec §4) — the BORROW half of session
    // binding, and the third case after owners and primitives.
    //
    // maybeEmitSessionBind rides the drop-entry choke point, which only
    // OWNERS reach. A reference initialized from something the unit does not
    // own — `String tag = "x";` (a literal is a mode-1 view over static
    // bytes), `String b = a;`, a borrow-returning call — deliberately gets no
    // drop entry, and so was never registered at all: `__cajeta_session_get`
    // returned null in the next cell and the binding silently VANISHED. A
    // String was the common case, and nothing covered it.
    //
    // Registered with a NULL drop_fn: the session can see the value but does
    // not own it, so drop_all must not free it. The registry already guards
    // on drop_fn before calling it.
    static void maybeEmitSessionBindBorrow(CajetaModulePtr module,
                                           FieldPtr field, CajetaTypePtr type) {
        if (!module->isScriptUnit() || !field || !type) return;
        if (field->isSessionBound()) return;          // owner path took it
        if (type->getTypeFlags() & PRIMITIVE_FLAG) return;  // boxed instead
        if (!module->isScriptBindingName(field->getName())) return;
        if (!module->isScriptEntryTopLevel()) return;  // 4.2.4(b)
        // Only pointer-shaped storage: a slot holding an inline aggregate is
        // not a reference to register, and reading it as one would hand the
        // registry the address of a dead frame.
        llvm::AllocaInst* slot = field->getOrCreateAllocation();
        if (!slot || !slot->getAllocatedType()->isPointerTy()) return;
        auto* builder = module->getBuilder();
        if (!builder || !builder->GetInsertBlock()) return;
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (parentFn == nullptr
            || parentFn->getName().find(scriptEntryName())
                   == llvm::StringRef::npos) {
            return;
        }
        llvm::Function* bindFn =
            module->getRuntimeFunction("__cajeta_session_bind");
        if (!bindFn) return;
        auto& ctx = *module->getLlvmContext();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* nameStr = builder->CreateGlobalString(field->getName());
        llvm::Value* ref = builder->CreateLoad(ptrTy, slot);
        builder->CreateCall(bindFn, {nameStr, ref,
                                     llvm::ConstantPointerNull::get(ptrTy)});
        field->setSessionBound(true);
    }

    // Script units (script-units spec §4) — the PRIMITIVE half of session
    // binding. maybeEmitSessionBind above rides the drop-entry choke point,
    // which only owners reach; a primitive has no drop entry, so before this
    // nothing ever registered `int32 seed = 40` and a later unit's read found
    // an empty slot. Copy the value into a session-owned box instead. Called
    // once per top-level declaration, after the initializer has stored.
    static void maybeEmitSessionBindValue(CajetaModulePtr module,
                                          FieldPtr field, CajetaTypePtr type) {
        if (!module->isScriptUnit() || !field || !type) return;
        if (!(type->getTypeFlags() & PRIMITIVE_FLAG)) return;
        if (!module->isScriptBindingName(field->getName())) return;
        if (!module->isScriptEntryTopLevel()) return;  // 4.2.4(b)
        auto* builder = module->getBuilder();
        if (!builder || !builder->GetInsertBlock()) return;
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (parentFn == nullptr
            || parentFn->getName().find(scriptEntryName())
                   == llvm::StringRef::npos) {
            return;
        }
        llvm::Function* bindFn =
            module->getRuntimeFunction("__cajeta_session_bind_value");
        if (!bindFn) return;
        llvm::AllocaInst* slot = field->getOrCreateAllocation();
        if (!slot) return;
        // Size from the SLOT's allocated type: that is the storage the box
        // must mirror byte for byte.
        auto& dl = module->getLlvmModule()->getDataLayout();
        uint64_t size = dl.getTypeStoreSize(slot->getAllocatedType());
        if (size == 0) return;
        auto& ctx = *module->getLlvmContext();
        llvm::Value* nameStr = builder->CreateGlobalString(field->getName());
        builder->CreateCall(bindFn, {nameStr, slot,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), size)});
        // The assignment path keys session re-binds on this flag (4.2.4(b));
        // without it a top-level `k += 2` skips the re-box and a later cell
        // reads the stale box.
        field->setSessionBound(true);
    }

    // Emit drop-chain wiring for an owner local. Allocates a DropEntry blob on
    // the stack at function entry, pushes it onto the runtime's chain right
    // after the owner is materialized, and records the entry on both the field
    // and the enclosing method so scope-exit emits the matching pop. In debug
    // mode (CompilerFlags::sourceTags) the push variant carries the LVD's
    // source file + line for runtime diagnostics.
    // Unit 4 — a Runtime title arms the entry from `flag` (0 = borrowed, so the
    // lender keeps its single drop). Null `flag` is the static owner: no set_flag.
    static void armEntryFlag(CajetaModulePtr& module, FieldPtr& field,
                             llvm::Value* entryPtr, llvm::Value* flag) {
        if (!flag) return;
        // Unit 6 — a CONSTANT 1 is already the push's own state, so it needs no call
        // and the local is a STATIC owner. A constant 0 keeps the call: it disarms.
        if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(flag)) {
            if (k->isOne()) return;
        }
        if (llvm::Function* setFlagFn = module->getRuntimeFunction(
                "__cajeta_drop_set_flag")) {
            module->getBuilder()->CreateCall(setFlagFn, {entryPtr, flag});
            field->setRuntimeConditionalOwner(true);
        }
    }

    static void emitDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                  const std::string& dropFnName,
                                  int allocLine = 0,
                                  llvm::Value* flag = nullptr) {
        DropPushChoice push = pickDropPush(module);
        llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
        if (!push.pushFn || !dropFn) return;
        if (!flag && maybeEmitSessionBind(module, field, dropFn)) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Allocate the DropEntry in the function's entry block so its address
        // is stable across the function's lifetime (matters because the chain
        // threads pointers through it).
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));

        // Load the owner pointer to pass as the drop function's `obj` arg.
        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        // Unit 9 — a flag that is not the constant 1 arms the entry in the push call itself.
        bool fuse = flag != nullptr;
        if (auto* k = llvm::dyn_cast_or_null<llvm::ConstantInt>(flag)) {
            if (k->isOne()) fuse = false;
        }
        llvm::Function* pushFn = push.pushFn;
        if (fuse) {
            if (llvm::Function* f = module->getRuntimeFunction(
                    push.debug ? "__cajeta_drop_push_flag_debug" : "__cajeta_drop_push_flag")) {
                pushFn = f;
            } else {
                fuse = false;
            }
        }
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            if (fuse) builder->CreateCall(pushFn, {entryPtr, ownerPtr, dropFn, fileConst, lineConst, flag});
            else      builder->CreateCall(pushFn, {entryPtr, ownerPtr, dropFn, fileConst, lineConst});
        } else {
            if (fuse) builder->CreateCall(pushFn, {entryPtr, ownerPtr, dropFn, flag});
            else      builder->CreateCall(pushFn, {entryPtr, ownerPtr, dropFn});
        }
        if (fuse) field->setRuntimeConditionalOwner(true);
        else armEntryFlag(module, field, entryPtr, flag);

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    // title-stores §3.2 — per-stride walk+free wrapper for local
    // bit-array drop entries. Element slots stride by the element
    // STRUCT's alloc size (a pre-existing class-element array layout
    // convention), so the stride is a per-type constant the one-arg
    // drop-entry ABI can't carry — synthesize a tiny fn per (hs, es).
    // `arrKind` < 0: class elements (__cajeta_tail_elem_drop_walk).
    // `arrKind` >= 0: ARRAY elements (title-stores §3.4) — the jagged walk
    // with that inner drop kind.
    static llvm::Function* getOrCreateTailDropFree(CajetaModulePtr module,
            uint64_t hs, uint64_t es, int arrKind = -1) {
        llvm::Module* m =
            module->getBuilder()->GetInsertBlock()->getParent()->getParent();
        std::string name = "cajeta_tail_dropfree_hs" + std::to_string(hs)
            + "_es" + std::to_string(es)
            + (arrKind >= 0 ? "_ak" + std::to_string(arrKind) : "");
        if (llvm::Function* f = m->getFunction(name)) return f;
        auto& fctx = m->getContext();
        auto* fptrTy = llvm::PointerType::get(fctx, 0);
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(fctx), {fptrTy}, false);
        auto* f = llvm::Function::Create(fnTy,
            llvm::Function::InternalLinkage, name, m);
        auto* bb = llvm::BasicBlock::Create(fctx, "entry", f);
        llvm::IRBuilder<> fb(bb);
        llvm::Function* walk = module->getRuntimeFunction(arrKind >= 0
            ? "__cajeta_tail_arrelem_drop_walk"
            : "__cajeta_tail_elem_drop_walk");
        llvm::Function* freeA = module->getRuntimeFunction(
            "__cajeta_free_array");
        auto* fi64 = llvm::Type::getInt64Ty(fctx);
        if (walk) {
            std::vector<llvm::Value*> wargs = {f->getArg(0),
                llvm::ConstantInt::get(fi64, hs),
                llvm::ConstantInt::get(fi64, es)};
            if (arrKind >= 0) {
                wargs.push_back(llvm::ConstantInt::get(fi64, arrKind));
            }
            fb.CreateCall(walk, wargs);
        }
        if (freeA) fb.CreateCall(freeA, {f->getArg(0)});
        fb.CreateRetVoid();
        return f;
    }

    // Public bridge for post-declaration ownership creation (9.3.1
    // move-assign into a bare-declared local). Same wiring; binds the
    // slot's CURRENT value, so callers invoke it after their store.
    // xref-lint-emission-gap Unit 3 — see the header. Registers each declared
    // name with its DECLARED type so a later `local.field` has a typed
    // receiver during a resolve-only walk. Gated on the module's
    // resolution-only mode: in a build, codegen registers the real
    // slot-carrying field and this must not shadow it (the build export has to
    // stay byte-identical, plan 2.1.1).
    void LocalVariableDeclaration::resolveTypes(CajetaModulePtr module) {
        if (!module || !module->isResolutionOnly()) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        for (auto& declarator : variableDeclarators) {
            if (!declarator) continue;
            // The initializer's own types first: a receiver inside it (`heap
            // Probe()`, a call argument) is resolvable on the same terms.
            //
            // Resolve the Initializer NODE, not a cast of it. Initializer
            // derives from AbstractSyntaxNode and NOT from Expression, so a
            // dynamic_pointer_cast<Expression> here always failed and silently
            // skipped every initializer — which is why `Counter c = stack
            // Counter()` recorded no constructor edge (4.2.3). The base walk
            // reaches the wrapped expression as a child.
            if (InitializerPtr init = declarator->getInitializer()) {
                try { init->resolveTypes(module); } catch (...) { }
            }
            if (!type) continue;
            const string name = declarator->getIdentifier();
            if (name.empty() || scope->containsField(name)) continue;
            scope->putField(make_shared<StackField>(module, name, type));
        }
    }

    void LocalVariableDeclaration::emitOwnerDropEntry(CajetaModulePtr module,
            FieldPtr field, const std::string& dropFnName, int allocLine) {
        emitDropEntryFor(module, field, dropFnName, allocLine);
    }

    // slices 9.2.1 / title-tracking Unit 4 — one extra chain entry per
    // OWNING element-tracked array local (String[] via the 9.2.1 family,
    // plain class elements via the Unit 4 slot-bit family), pushed right
    // after the storage (free_array) entry so LIFO runs the element walk
    // BEFORE the buffer frees. obj = a stack sidecar {arr_slot,
    // storage_entry, owned_bits, owned_cap, stride, header} the
    // element-store helpers and the walk share; store sites reach it via
    // field->getElemOwnSidecar(). Pushed in the DECLARING frame (the 9.3.1
    // placement rule) so loop-body / inner-block stores can't be freed at
    // the wrong scope exit. `walkDropFn` picks the element family:
    // __cajeta_string_array_owned_drop or __cajeta_class_array_owned_drop.
    static void emitArrayElemDropEntry(CajetaModulePtr module,
            FieldPtr field, const shared_ptr<CajetaArray>& arrType,
            const char* walkDropFn, int allocLine = 0,
            llvm::Value* flag = nullptr) {
        DropPushChoice push = pickDropPush(module);
        llvm::Function* dropFn = module->getRuntimeFunction(walkDropFn);
        if (!push.pushFn || !dropFn) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));
        llvm::Value* sidecar = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i64Ty, 6), nullptr, "str_arr_sidecar");

        const llvm::DataLayout& dl =
            module->getLlvmModule()->getDataLayout();
        uint64_t strideBytes = dl.getTypeAllocSize(
            arrType->getElementLlvmType(&ctx));
        uint64_t headerBytes = dl.getTypeAllocSize(arrType->getLlvmType());
        auto slotAt = [&](unsigned i, const char* nm) -> llvm::Value* {
            return builder->CreateInBoundsGEP(i64Ty, sidecar,
                llvm::ConstantInt::get(i64Ty, i), nm);
        };
        builder->CreateStore(field->getOrCreateAllocation(),
            slotAt(0, "sc.arrslot"));
        llvm::Value* storageEntry = field->getDropEntry();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        builder->CreateStore(storageEntry
                ? storageEntry
                : (llvm::Value*) llvm::ConstantPointerNull::get(ptrTy),
            slotAt(1, "sc.storage"));
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0),
            slotAt(2, "sc.bits"));
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0),
            slotAt(3, "sc.cap"));
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, strideBytes),
            slotAt(4, "sc.stride"));
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, headerBytes),
            slotAt(5, "sc.header"));

        {
            // Unit 9 — push + arm in one call when a flag arrives.
            llvm::Function* pushFn = push.pushFn;
            bool fuse = flag != nullptr;
            if (fuse) {
                if (llvm::Function* f = module->getRuntimeFunction(
                        push.debug ? "__cajeta_drop_push_flag_debug" : "__cajeta_drop_push_flag")) {
                    pushFn = f;
                } else {
                    fuse = false;
                }
            }
            if (push.debug) {
                llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                    module->getSourcePath());
                llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
                if (fuse) builder->CreateCall(pushFn, {entryPtr, sidecar, dropFn, fileConst, lineConst, flag});
                else      builder->CreateCall(pushFn, {entryPtr, sidecar, dropFn, fileConst, lineConst});
            } else {
                if (fuse) builder->CreateCall(pushFn, {entryPtr, sidecar, dropFn, flag});
                else      builder->CreateCall(pushFn, {entryPtr, sidecar, dropFn});
            }
            if (!fuse && flag) {
                if (llvm::Function* setFlagFn = module->getRuntimeFunction(
                        "__cajeta_drop_set_flag")) {
                    builder->CreateCall(setFlagFn, {entryPtr, flag});
                }
            }
        }
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
        field->setElemOwnSidecar(sidecar);
    }

    // title-tracking 5.2.3 — runtime-owner LOCALS. A class-typed call result
    // carries its title in the return-flag TLS, so the receiving local's drop
    // entry is ARMED FROM THAT FLAG rather than from a compile-time fact: a
    // `#`-forwarded value drops here, a lent one leaves the entry disarmed and
    // the original owner keeps its single drop. `flag` must be read at the
    // earliest point after the call (see generateCode) — any intervening cajeta
    // call would overwrite the thread-local.
    static void emitFlaggedDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                         const std::string& dropFnName,
                                         llvm::Value* flag,
                                         int allocLine = 0) {
        if (!flag) return;
        emitDropEntryFor(module, field, dropFnName, allocLine, flag);
    }

    // Variant for shared-capable VALUE locals (slice-spec §6.1 drop hook):
    // the drop entry's obj is the value's stack slot ITSELF (the alloca is
    // the storage, not a pointer to be loaded), and the drop fn is the
    // class's synthesized per-field release walk.
    static void emitValueDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                       llvm::Function* releaseFn,
                                       int allocLine = 0) {
        DropPushChoice push = pickDropPush(module);
        if (!push.pushFn || !releaseFn) return;
        releaseFn = CajetaModule::ensureFunctionInModule(
            module->getLlvmModule(), releaseFn);
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));

        llvm::Value* ownerPtr = field->getOrCreateAllocation();
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, releaseFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, releaseFn});
        }

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    // Variant for class-instance locals — same drop-chain wiring, but
    // takes an already-resolved drop function (the class's synthesized
    // wrapper) directly instead of a runtime-helper name. The wrapper
    // is specific to the class, so there's no global symbol to look up
    // via getRuntimeFunction.
    static void emitDropEntryForFn(CajetaModulePtr module, FieldPtr field,
                                    llvm::Function* dropFn,
                                    int allocLine = 0,
                                    llvm::Value* flag = nullptr) {
        DropPushChoice push = pickDropPush(module);
        if (!push.pushFn || !dropFn) return;
        // Cross-module: when the class whose drop fn we're pushing
        // lives in a different llvm::Module (a stdlib class
        // referenced from user code), substitute a module-local
        // extern decl so the merge step resolves the Constant.
        dropFn = CajetaModule::ensureFunctionInModule(
            module->getLlvmModule(), dropFn);
        if (!flag && maybeEmitSessionBind(module, field, dropFn)) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));

        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn});
        }
        armEntryFlag(module, field, entryPtr, flag);

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    // slices plan 4.2.1 — the LOCAL classification for the borrow downgrade.
    //
    // The default AbstractSyntaxNode child walk is NOT enough here: most
    // statement types keep their subtrees in private members, not in
    // `children` (ExpressionStatement's own comment: "The wrapped expression
    // isn't in `children` so the default walk skips it"), and method-call /
    // constructor arguments live in MethodCallParameter vectors. An analysis
    // whose safe direction is over-approximation (slice-spec §4.3) must see
    // ALL of them — missing a use meant the borrow downgrade fired on views
    // that escape via `out.add(#g)` (NgramIndexTests regression). This
    // collector enumerates children plus every hidden subtree.
    static void collectSubNodes(const AbstractSyntaxNodePtr& node,
                                std::vector<AbstractSyntaxNodePtr>& out) {
        for (auto& child : node->getChildren()) out.push_back(child);
        if (auto es = dynamic_pointer_cast<ExpressionStatement>(node)) {
            if (es->getExpression()) out.push_back(es->getExpression());
        }
        if (auto ls = dynamic_pointer_cast<LabelStatement>(node)) {
            if (ls->getBlock()) out.push_back(ls->getBlock());
        }
        if (auto sc = dynamic_pointer_cast<ScopeStatement>(node)) {
            if (sc->getBlock()) out.push_back(sc->getBlock());
        }
        if (auto is = dynamic_pointer_cast<IfStatement>(node)) {
            if (is->getCondition()) out.push_back(is->getCondition());
            if (is->getThenBranch()) out.push_back(is->getThenBranch());
            if (is->getElseBranch()) out.push_back(is->getElseBranch());
        }
        if (auto fs = dynamic_pointer_cast<ForStatement>(node)) {
            if (fs->getInit()) out.push_back(fs->getInit());
            if (fs->getCondition()) out.push_back(fs->getCondition());
            for (auto& u : fs->getUpdate()) if (u) out.push_back(u);
            if (fs->getBody()) out.push_back(fs->getBody());
        }
        if (auto efs = dynamic_pointer_cast<EnhancedForStatement>(node)) {
            if (efs->getIterableExpr()) out.push_back(efs->getIterableExpr());
            if (efs->getBody()) out.push_back(efs->getBody());
        }
        if (auto ws = dynamic_pointer_cast<WhileStatement>(node)) {
            if (ws->getCondition()) out.push_back(ws->getCondition());
            if (ws->getBody()) out.push_back(ws->getBody());
        }
        if (auto ds = dynamic_pointer_cast<DoStatement>(node)) {
            if (ds->getBody()) out.push_back(ds->getBody());
            if (ds->getCondition()) out.push_back(ds->getCondition());
        }
        if (auto ts = dynamic_pointer_cast<TryStatement>(node)) {
            if (ts->getTryBlock()) out.push_back(ts->getTryBlock());
            for (auto& c : ts->getCatchClauses()) if (c.body) out.push_back(c.body);
            if (ts->getFinallyBlock()) out.push_back(ts->getFinallyBlock());
        }
        if (auto sw = dynamic_pointer_cast<SwitchStatement>(node)) {
            if (sw->getSubject()) out.push_back(sw->getSubject());
            for (auto& g : sw->getGroups()) {
                for (auto& v : g.caseValues) if (v) out.push_back(v);
                for (auto& st : g.statements) if (st) out.push_back(st);
            }
        }
        if (auto rs = dynamic_pointer_cast<ReturnStatement>(node)) {
            if (rs->getExpression()) out.push_back(rs->getExpression());
        }
        if (auto th = dynamic_pointer_cast<ThrowStatement>(node)) {
            if (th->getExpression()) out.push_back(th->getExpression());
        }
        if (auto mc = dynamic_pointer_cast<MethodCallExpression>(node)) {
            for (auto& pm : mc->getParameters())
                if (pm.expression) out.push_back(pm.expression);
        }
        if (auto ne = dynamic_pointer_cast<NewExpression>(node)) {
            if (ne->getCreatorRest()) out.push_back(ne->getCreatorRest());
        }
        if (auto cr = dynamic_pointer_cast<ClassCreatorRest>(node)) {
            for (auto& pm : cr->getParameters())
                if (pm.expression) out.push_back(pm.expression);
        }
    }

    // A name's use BLOCKS the downgrade when it appears under a return, a
    // `#`-move, a lambda (capture), or a `#`-TRANSFERRED call/ctor argument:
    // those carry the VIEW itself out of the scope. Plain (borrow) call args
    // and field stores do NOT block — the value resolves at its own escape
    // site (4.2.2 (a)). Name-based and conservative.
    static bool subtreeUsesName(const AbstractSyntaxNodePtr& node,
                                const std::string& name) {
        if (!node) return false;
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(node)) {
            if (id->getTextValue() == name) return true;
        }
        std::vector<AbstractSyntaxNodePtr> subs;
        collectSubNodes(node, subs);
        for (auto& child : subs) {
            if (subtreeUsesName(child, name)) return true;
        }
        return false;
    }
    // reassign-leak family (2026-09-07) — does any assignment to `name` in
    // this subtree carry an OWNED-shaped right-hand side? Shape-only and
    // conservative: a call result or a conditional MAY carry a title at
    // runtime, so they count. A false positive costs one inactive drop entry
    // (a push/pop pair); a false negative would leak the assigned value.
    static bool nameReassignedOwned(const AbstractSyntaxNodePtr& node,
                                    const std::string& name) {
        if (!node) return false;
        if (auto bop = dynamic_pointer_cast<BinaryOpExpression>(node)) {
            auto& bk = bop->getChildren();
            if (bop->getBinaryOp() == BINARY_OP_ASSIGN && bk.size() >= 2) {
                auto lhsId = dynamic_pointer_cast<IdentifierExpression>(bk[0]);
                auto rhs = bk[1];
                if (lhsId && lhsId->getTextValue() == name && rhs) {
                    if (isMoveKind(rhs)
                            || dynamic_pointer_cast<MethodCallExpression>(rhs)
                            || dynamic_pointer_cast<CallExpression>(rhs)
                            || isConditionalKind(dynamic_pointer_cast<Expression>(rhs))) {
                        return true;
                    }
                    if (auto ne = dynamic_pointer_cast<NewExpression>(rhs)) {
                        if (!ne->getStackAlloc()) return true;
                    }
                    if (auto ag = dynamic_pointer_cast<AggregateInitializerExpression>(rhs)) {
                        if (!ag->getStackAlloc()) return true;
                    }
                    if (auto rb = dynamic_pointer_cast<BinaryOpExpression>(rhs)) {
                        if (rb->getBinaryOp() == BINARY_OP_ADD) return true;
                    }
                    if (auto al = dynamic_pointer_cast<ArrayLiteralExpression>(rhs)) {
                        if (!al->isStackAlloc()) return true;
                    }
                }
            }
        }
        std::vector<AbstractSyntaxNodePtr> subs;
        collectSubNodes(node, subs);
        for (auto& child : subs) {
            if (nameReassignedOwned(child, name)) return true;
        }
        return false;
    }
    static bool nameEscapesScope(const AbstractSyntaxNodePtr& node,
                                 const std::string& name) {
        if (!node) return false;
        bool blocking = dynamic_pointer_cast<ReturnStatement>(node)
            || isMoveKind(node)
            || dynamic_pointer_cast<LambdaExpression>(node);
        if (blocking) return subtreeUsesName(node, name);
        // A `#name` transfer at argument position is a move, but lands as
        // MethodCallParameter.callerTransferred with a plain identifier
        // expression rather than a MoveExpression node — treat it as
        // blocking or the callee receives a stakeless borrow whose root
        // dies with this scope.
        if (auto mc = dynamic_pointer_cast<MethodCallExpression>(node)) {
            for (const auto& pm : mc->getParameters()) {
                if (pm.callerTransferred && subtreeUsesName(pm.expression, name)) {
                    return true;
                }
            }
        }
        if (auto cr = dynamic_pointer_cast<ClassCreatorRest>(node)) {
            for (const auto& pm : cr->getParameters()) {
                if (pm.callerTransferred && subtreeUsesName(pm.expression, name)) {
                    return true;
                }
            }
        }
        std::vector<AbstractSyntaxNodePtr> subs;
        collectSubNodes(node, subs);
        for (auto& child : subs) {
            if (nameEscapesScope(child, name)) return true;
        }
        return false;
    }

    /**
     * If we have a primitive variable, we can store in on the stack and will immediately create an currentRegister.
     * Otherwise, we will create an currentRegister for a structure reference.  If the variable receives a new operator,
     * we'll just let the malloc call create the register
     *
     * @param module
     * @return
     */
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModulePtr module) {

        // Arrays and class instances live on the heap; their local slot is a pointer.
        // Only true primitives (int32, float64, bool, etc.) — and @ValueType POD
        // values, which are by-value/Copy like primitives — get an inline-value
        // alloca holding the struct itself. See plans/value-type-overloading-plan.md.
        bool isArray = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool wantsInlineSlot = type->hasValueSemantics() && !isArray;
        for (auto& declarator: variableDeclarators) {
            InitializerPtr initializer = declarator->getInitializer();
            // Array-literal initializer (`int32[] xs = {1, 2, 3}`): the
            // literal has no type of its own, so push the element type
            // down here before codegen so the literal knows how big the
            // slots are and how to coerce its values.
            if (isArray) {
                if (auto arrInit = dynamic_pointer_cast<ArrayInitializer>(initializer)) {
                    if (auto arrType = dynamic_pointer_cast<CajetaArray>(type)) {
                        arrInit->setElementType(arrType->getElementType());
                    }
                } else if (auto varInit =
                        dynamic_pointer_cast<VariableInitializer>(initializer)) {
                    // `int32[] xs = [1, 2, 3]` — the `[...]` literal is wrapped
                    // in a VariableInitializer. Push the declared element type
                    // as the target (array-literals §3.2) so it wins over the
                    // unify fallback (widening) and gives an empty `[]` its type.
                    auto& kids = varInit->getChildren();
                    if (!kids.empty()) {
                        if (auto arrLit =
                                dynamic_pointer_cast<ArrayLiteralExpression>(kids[0])) {
                            if (auto arrType = dynamic_pointer_cast<CajetaArray>(type)) {
                                arrLit->setElementType(arrType->getElementType());
                            }
                        }
                    }
                }
            }
            // collection-literals §2 (Unit 1) — a bare `[...]` literal against a
            // class target (a collection) rewrites to a from-array constructor
            // call `heap Target([...])`, so `ArrayList<int32> xs = [1,2,3]`
            // builds the list. Runs before getOrCreateAllocation generates the
            // initializer. Array targets are handled by the isArray block above.
            if (!isArray) {
                if (auto varInit =
                        dynamic_pointer_cast<VariableInitializer>(initializer)) {
                    auto& kids = varInit->getChildren();
                    if (!kids.empty()) {
                        if (auto expr =
                                dynamic_pointer_cast<Expression>(kids[0])) {
                            if (auto ctor =
                                    collectionLiteralFromArray(type, expr)) {
                                kids[0] = ctor;
                            } else if (auto agg = dynamic_pointer_cast<
                                    AggregateInitializerExpression>(expr)) {
                                // collection-literals §4 — prefixless `{…}`
                                // infers the declared type (only a class target
                                // can back an aggregate; a primitive target
                                // leaves it uninferred → clean NO_TYPE error).
                                if (dynamic_pointer_cast<CajetaClass>(type)) {
                                    agg->setExpectedType(type);
                                }
                            } else if (auto mapLit = dynamic_pointer_cast<
                                    MapLiteralExpression>(expr)) {
                                // collection-literals §3 — `[k: v]` infers the
                                // declared map type (K,V from HashMap<K,V>).
                                if (dynamic_pointer_cast<CajetaClass>(type)) {
                                    mapLit->setExpectedType(type);
                                }
                            }
                        }
                    }
                }
            }
            // Function-typed initializer with a lambda RHS: push the LHS's
            // function type down to the lambda so it can use the declared
            // return type (and, eventually, expected param types) rather
            // than trying to infer them from a body whose own resolvedType
            // isn't always populated. See docs/specification/lang/Lambdas.md.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        lambda->setExpectedType(type);
                    } else if (auto mref = dynamic_pointer_cast<MethodReferenceExpression>(children[0])) {
                        // M5(b) adapter — sret-form LHS + borrow-returning
                        // target needs the method-ref to know the expected
                        // ABI so it can pick the sret-shaped fnType and
                        // synthesize the borrow→sret adapter thunk.
                        mref->setExpectedType(type);
                    }
                }
            }
            // slices plan 4.2.2 — local-borrow downgrade. A String local
            // initialized from `recv.substring(...)` / `recv.trim()` whose
            // name never appears under a return / `#`-move / lambda in this
            // method gets the BORROW slice (zero rc): rewrite the call to
            // the *View twin before the initializer generates. Escapes of
            // the VALUE (field stores, args) resolve at their own sites.
            {
                auto declClass = dynamic_pointer_cast<CajetaClass>(type);
                bool declIsString = declClass && declClass->getQName()
                    && declClass->getQName()->getTypeName() == "String"
                    && declClass->getQName()->getPackageName() == "cajeta.lang";
                if (declIsString && initializer) {
                    if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                        auto& kids = varInit->getChildren();
                        // §4.6 spells this receipt `String w #= s.substring(…)`
                        // because `substring` returns `#String`, and that wraps
                        // the call in a MoveExpression — so looking for the
                        // call directly found nothing and the downgrade
                        // silently stopped firing, turning every local
                        // substring into a stake-taking share. The receipt
                        // spelling records WHO OWNS the result; it says nothing
                        // about whether the local escapes, which is the only
                        // question this rewrite asks. Unwrap and carry on.
                        // (`substringView`/`trimView` are `#String` too, so the
                        // rewritten call still satisfies the receipt.)
                        auto init0 = kids.empty() ? nullptr
                            : dynamic_pointer_cast<Expression>(kids[0]);
                        if (isMoveKind(init0)) init0 = moveInner(init0);   // the `#=` wrapper's operand
                        auto mc = dynamic_pointer_cast<MethodCallExpression>(init0);
                        const std::string mcName = mc ? mc->getMethodCallName() : "";
                        if (mc && (mcName == "substring" || mcName == "trim")) {
                            auto& mck = mc->getChildren();
                            auto recv = mck.empty() ? nullptr
                                : dynamic_pointer_cast<Expression>(mck[0]);
                            if (recv) {
                                if (!recv->getResolvedType()) recv->resolveTypes(module);
                                auto recvCls = dynamic_pointer_cast<CajetaClass>(
                                    recv->getResolvedType());
                                bool recvIsString = recvCls && recvCls->getQName()
                                    && recvCls->getQName()->getTypeName() == "String"
                                    && recvCls->getQName()->getPackageName() == "cajeta.lang";
                                if (recvIsString) {
                                    auto m = module->getCurrentMethod();
                                    BlockPtr body = m ? m->getBlock() : nullptr;
                                    if (body && !nameEscapesScope(body,
                                            declarator->getIdentifier())) {
                                        mc->setMethodCallName(
                                            mcName == "substring" ? "substringView"
                                                                  : "trimView");
                                    }
                                }
                            }
                        }
                    }
                }
            }
            FieldPtr field;
            if (wantsInlineSlot) {
                field = make_shared<StackField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            } else {
                field = make_shared<HeapField>(module, declarator->getIdentifier(), type,
                    declarator->isReference(), modifiers, annotations, initializer);
            }
            // title-stores §2.1 — `T x #= #src[i]` (the DOUBLE-Move shape
            // `T x #= src[i]` / `T x #= p.f` — the SLOT CLAIM, and it is
            // MODE-CARRYING: the local arms from the slot's actual bit rather
            // than demanding a title. `#=` means "preserve borrow or take
            // ownership", so the source's SHAPE must not change the spelling —
            // `#= #src[i]` is the transfer written twice and stays rejected
            // (CAJETA_ERROR_DOUBLE_TRANSFER).
            //
            // Demanding a title here panicked CAJETA_PANIC_TITLE_MISS on any
            // borrowed slot, which is the common case now that collections do
            // not own by default: it broke Heap.pop, HashMap.remove and
            // LinkedList.popHead, all three declaring PLAIN returns that never
            // promised ownership.
            if (auto fwdVi = dynamic_pointer_cast<VariableInitializer>(
                    declarator->getInitializer())) {
                auto& fwdKids = fwdVi->getChildren();
                if (!fwdKids.empty()) {
                    if (auto outerMv = isMoveKind(fwdKids[0])
                            ? std::static_pointer_cast<MoveExpression>(fwdKids[0]) : nullptr) {
                        if (!outerMv->getChildren().empty()) {
                            // Walk down any nested moves: `#= src[i]` gives
                            // one MoveExpression, the redundant `#= #src[i]`
                            // gives two, and both must forward. The flag goes
                            // on whichever move directly wraps the slot — that
                            // is the one whose generateCode emits the take.
                            auto slotMv = outerMv;
                            while (slotMv && !slotMv->getChildren().empty()) {
                                auto src = slotMv->getChildren()[0];
                                if (dynamic_pointer_cast<ArrayIndexExpression>(src)
                                        || dynamic_pointer_cast<DotExpression>(src)) {
                                    slotMv->setForwardingSlotMove(true);
                                    break;
                                }
                                slotMv = isMoveKind(src) ? std::static_pointer_cast<MoveExpression>(src) : nullptr;
                            }
                        }
                    }
                }
            }
            module->getScopeStack().peek()->putField(field);
            // script-units U4 (spec §4.2/§4.3) — a redeclaration of a session
            // binding is the rebind form across units (last-write-wins,
            // spec §5): the fresh declaration replaced any seeded field
            // above; clear a moved-out mark carried over from an earlier
            // unit so reads are legal again.
            if (module->isScriptUnit()
                && module->isScriptBindingName(field->getName())) {
                auto* sessPfn = module->getBuilder()->GetInsertBlock()
                                    ->getParent();
                if (sessPfn && sessPfn->getName().find(scriptEntryName())
                                   != llvm::StringRef::npos) {
                    module->getScopeStack().peek()->restoreOwnership(
                        field->getName());
                }
            }
            field->getOrCreateAllocation();

            // Unit 4 (spec §2.3, role Bind) — ONE classification of the initializer
            // decides the binding. It runs HERE, right after the initializer's IR: a
            // call's TLS bit and a move's stashed flag are readable only now.
            ExpressionPtr initExpr;
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(
                    initializer)) {
                auto& kids = varInit->getChildren();
                if (!kids.empty()) {
                    initExpr = dynamic_pointer_cast<Expression>(kids[0]);
                }
            }
            const ownership::TitleShape initShape = initExpr
                ? ownership::classify(initExpr, module)
                : ownership::TitleShape();
            const ownership::TitleVerdict initVerdict =
                ownership::policy(initShape, ownership::ConsumerRole::Bind);
            if (ownership::TitleShapeAudit::enabled() && initExpr) {
                ownership::observeTitle(initExpr, module,
                                        ownership::ConsumerRole::Bind);
            }
            // Borrow and Runtime both keep the static owner paths off; StackBound takes the stack-drop path.
            const bool initIsBorrow =
                initVerdict.answer == ownership::TitleAnswer::Borrow
                || initVerdict.answer == ownership::TitleAnswer::Runtime;
            const bool initIsFlaggedCall =
                initVerdict.answer == ownership::TitleAnswer::Runtime;
            const bool initIsStackAlloc =
                initVerdict.answer == ownership::TitleAnswer::StackBound;
            // The flag, once, and only when the declared type can hold an entry that reads it.
            llvm::Value* initTitleFlag = nullptr;
            if (initIsFlaggedCall) {
                auto flagCls = dynamic_pointer_cast<CajetaClass>(type);
                if (flagCls && !dynamic_pointer_cast<CajetaView>(type)
                        && !flagCls->isValueType()
                        && !flagCls->isSharedCapableValue()) {
                    initTitleFlag = ownership::titleFlag(initShape, module);
                }
            }

            // slice-spec §6.1 copy/drop hooks for shared-capable VALUE locals
            // (Utf8 / value aggregates embedding it). The init copy already
            // happened inside getOrCreateAllocation:
            //   - init from an LVALUE (identifier / field / element / record
            //     slice-cast) duplicated existing stakes -> retain each.
            //   - init from an RVALUE (call result, aggregate-init, stack
            //     ctor) carries its stakes with the bytes -> no retain.
            // Every such local registers a release drop entry (obj = the
            // slot itself); return-of-local deactivates it (move-out).
            if (wantsInlineSlot) {
                auto vClass = dynamic_pointer_cast<CajetaClass>(type);
                if (vClass && vClass->isSharedCapableValue()) {
                    bool initFromLvalue = false;
                    if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                        auto& kids = varInit->getChildren();
                        if (!kids.empty()) {
                            initFromLvalue =
                                dynamic_pointer_cast<IdentifierExpression>(kids[0])
                                || dynamic_pointer_cast<DotExpression>(kids[0])
                                || dynamic_pointer_cast<ArrayIndexExpression>(kids[0])
                                || dynamic_pointer_cast<CastExpression>(kids[0]);
                        }
                    }
                    auto* builder = module->getBuilder();
                    if (initFromLvalue) {
                        vClass->emitValueSharedOp(*builder,
                            field->getOrCreateAllocation(), module,
                            builder->GetInsertBlock()->getModule(),
                            /*retain=*/true);
                    }
                    emitValueDropEntryFor(module, field,
                        vClass->getOrCreateValueReleaseFunction(),
                        getSourceLine());
                }
            }

            // Debugger CP5/CP7-1b: this local is registered in the current
            // debug frame at the END of this method (see emitDbgLocal below),
            // NOT here. The ownership facet is sourced from field->getDropEntry(),
            // and the drop-chain wiring (emitDropEntryFor / setIsOwningView)
            // that sets it runs later in this same method — registering here
            // would always read a null drop entry and mislabel every owner.

            // Polymorphic-MI upcast adjustment. After HeapField stored
            // the RHS pointer into the slot, check whether the static
            // RHS type is a non-self class subclass of the LHS class
            // type. If so — and the LHS isn't on the RHS's first-parent
            // chain (offset != 0) — reload, shift to the LHS's sub-
            // object start, and store back. This makes
            // `B b = c` actually point at C's B sub-object rather than
            // at C itself, so dispatch via b lands on the secondary
            // vtable + B-shaped field offsets.
            if (initializer && type) {
                auto dstClass = dynamic_pointer_cast<CajetaClass>(type);
                if (dstClass && !dstClass->isInterface()) {
                    auto varInit = dynamic_pointer_cast<VariableInitializer>(
                        initializer);
                    CajetaTypePtr srcType;
                    if (varInit && !varInit->getChildren().empty()) {
                        if (auto expr = dynamic_pointer_cast<Expression>(
                                varInit->getChildren()[0])) {
                            if (!expr->getResolvedType()) {
                                expr->resolveTypes(module);
                            }
                            srcType = expr->getResolvedType();
                        }
                    }
                    auto srcClass = dynamic_pointer_cast<CajetaClass>(srcType);
                    // Record conversions are never implicit — an upcast
                    // SLICES (data loss stays visible at the cast site).
                    if (srcClass && srcClass.get() != dstClass.get()
                            && (srcClass->isRecordType() || dstClass->isRecordType())) {
                        throw Exception(
                            "cannot implicitly convert '" + srcClass->toCanonical()
                                + "' to '" + dstClass->toCanonical()
                                + "' — record upcasts slice; write an explicit "
                                  "cast: (" + dstClass->getQName()->getTypeName()
                                + ") value",
                            "CAJETA_ERROR_RECORD_IMPLICIT_CAST");
                    }
                    if (srcClass && srcClass.get() != dstClass.get()
                            && !srcClass->isInterface()) {
                        uint64_t off = srcClass->getSubObjectByteOffset(
                            dstClass.get());
                        if (off != 0) {
                            auto* builder = module->getBuilder();
                            auto& ctx = *module->getLlvmContext();
                            llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
                            llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                            llvm::Value* slot = field->getOrCreateAllocation();
                            llvm::Value* raw = builder->CreateLoad(
                                ptrTy, slot, "upcast_raw");
                            llvm::Value* adjusted = builder->CreateInBoundsGEP(
                                i8Ty, raw,
                                llvm::ConstantInt::get(
                                    llvm::Type::getInt64Ty(ctx), off),
                                "upcast_subobj");
                            builder->CreateStore(adjusted, slot);
                        }
                    }
                }
            }

            // P3 — definite-assignment tracking. A local declared without
            // an initializer enters the scope's NYA set; reading it before
            // an assignment is a compile error. Applies to both class-
            // typed locals (a null reference is meaningful, but reading
            // it would be a runtime null-deref) and primitive locals (the
            // alloca contents are undefined until written). Skipped for
            // view/struct locals that get their body pointer wired via
            // the S6.1 implicit alloca path below — those are stack-
            // resident with zero-init bodies, "assigned" by virtue of
            // pointing at a real body. Also skipped for interface-typed
            // locals (the interface-local handling block above stores a
            // body ptr in the slot regardless of initializer).
            if (!initializer) {
                bool implicitZeroInit =
                    dynamic_pointer_cast<CajetaView>(type) != nullptr;
                bool isInterface = false;
                if (auto kc = dynamic_pointer_cast<CajetaClass>(type)) {
                    isInterface = kc->isInterface();
                }
                if (!implicitZeroInit && !isInterface) {
                    module->getScopeStack().peek()->markNotYetAssigned(
                        declarator->getIdentifier());
                }
            }

            // `class Foo f;` without an initializer lands as an NYA-marked
            // null class ref via the path above. Instantiation is always
            // explicit: `heap Foo()` / `stack Foo()` / `Foo { x: 1, y: 2 }`.

            // Interface local handling. An interface local's
            // HeapField slot holds a `ptr` pointing at a 24-byte
            // fat-pointer body. Three init shapes:
            //   - No initializer: allocate empty body, zero-init, store
            //     body ptr in slot.
            //   - Initializer RHS is an interface value (already a body
            //     ptr after loadIfLValue's S9.5.4 branch): HeapField
            //     stored the body ptr in the slot; the local aliases the
            //     source body (borrow). No additional work here.
            //   - Initializer RHS is a non-interface class value (e.g.
            //     `Greeter g = new Hello()`): HeapField stored the
            //     class instance pointer in the slot, but the local
            //     needs a 24-byte fat-pointer body. Build the body
            //     (data = class ptr, vtable = per-(class, iface) global,
            //     kind = BORROWED_CLASS for v1 — `#`-marked owned land
            //     in S10.2) and overwrite the slot to point at it.
            if (auto ifaceKlass = dynamic_pointer_cast<CajetaClass>(type)) {
                if (ifaceKlass->isInterface()) {
                    auto* builder = module->getBuilder();
                    auto& lctx = *module->getLlvmContext();
                    llvm::Type* bodyTy = type->getLlvmType();
                    llvm::Type* ptrTy = llvm::PointerType::get(lctx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(lctx);

                    if (!initializer) {
                        llvm::Value* bodyAlloca = builder->CreateAlloca(bodyTy);
                        builder->CreateStore(
                            llvm::Constant::getNullValue(bodyTy), bodyAlloca);
                        builder->CreateStore(bodyAlloca,
                            field->getOrCreateAllocation());
                    } else {
                        auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer);
                        ExpressionPtr rhsExpr;
                        if (varInit && !varInit->getChildren().empty()) {
                            rhsExpr = dynamic_pointer_cast<Expression>(
                                varInit->getChildren()[0]);
                            if (rhsExpr && !rhsExpr->getResolvedType()) {
                                rhsExpr->resolveTypes(module);
                            }
                        }
                        CajetaTypePtr rhsType = rhsExpr
                            ? rhsExpr->getResolvedType() : nullptr;
                        auto rhsClass = dynamic_pointer_cast<CajetaClass>(rhsType);
                        bool rhsIsInterface = rhsClass && rhsClass->isInterface();

                        if (rhsClass && !rhsIsInterface) {
                            // Re-load whatever HeapField stored in the slot.
                            // For a class instance RHS that's the heap class
                            // pointer; for a struct RHS that's the struct
                            // body pointer (S6.2 / S6.7 / aliasing flows
                            // all hand off ptr-to-body as the struct value).
                            // Either way the value goes into the fat
                            // pointer's data slot.
                            llvm::Value* sourcePtr = builder->CreateLoad(
                                ptrTy, field->getOrCreateAllocation());

                            llvm::Value* bodyAlloca = builder->CreateAlloca(bodyTy);
                            llvm::Value* dataSlot = builder->CreateStructGEP(
                                bodyTy, bodyAlloca, 0, "iface_data");
                            llvm::Value* vtSlot = builder->CreateStructGEP(
                                bodyTy, bodyAlloca, 1, "iface_vtable");
                            llvm::Value* kindSlot = builder->CreateStructGEP(
                                bodyTy, bodyAlloca, 2, "iface_kind");
                            builder->CreateStore(sourcePtr, dataSlot);

                            std::string ifaceCanonical =
                                ifaceKlass->getQName()->toCanonical();
                            llvm::Constant* vtableRef = nullptr;
                            if (auto gv = rhsClass->getInterfaceVTable(ifaceCanonical)) {
                                vtableRef = CajetaModule::ensureGlobalInModule(
                                    module->emitTargetLlvmModule(), gv);
                            }
                            if (!vtableRef) {
                                vtableRef = llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            }
                            builder->CreateStore(vtableRef, vtSlot);

                            // Unit 4 (spec 5.7) — the kind is the initializer's
                            // title: OWNED for a fresh value, a `#R` result or a
                            // `#x` move; a Runtime answer selects on its flag.
                            llvm::Constant* ownedK = llvm::ConstantInt::get(
                                i64Ty, (uint64_t) IFACE_KIND_OWNED_CLASS);
                            llvm::Constant* borrowedK = llvm::ConstantInt::get(
                                i64Ty, (uint64_t) IFACE_KIND_BORROWED_CLASS);
                            llvm::Value* kindVal = borrowedK;
                            if (initVerdict.answer
                                    == ownership::TitleAnswer::Owned) {
                                kindVal = ownedK;
                            } else if (initTitleFlag) {
                                kindVal = builder->CreateSelect(
                                    builder->CreateICmpNE(initTitleFlag,
                                        llvm::ConstantInt::get(i64Ty, 0)),
                                    ownedK, borrowedK, "iface_kind");
                            }
                            builder->CreateStore(kindVal, kindSlot);
                            builder->CreateStore(bodyAlloca,
                                field->getOrCreateAllocation());
                        }
                        // RHS is interface: loadIfLValue returned a body
                        // ptr, HeapField stored it. Local now aliases the
                        // source body (BORROWED). No fix-up needed.
                    }
                }
            }

            // L3-2 escape-check wiring: a function-typed local initialized
            // from a lambda or bound method reference inherits the RHS's
            // borrow-capture state. After the initializer has run
            // (putField → getOrCreateAllocation triggered the RHS's
            // generateCode and its capture analysis), copy the flag onto
            // the field so a later `return fnLocal` can surface the
            // dangling-borrow error before LLVM verify.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        if (lambda->getHasBorrowCaptures()) {
                            field->setHasBorrowCaptures(true);
                        }
                    }
                    if (auto methodRef = dynamic_pointer_cast<MethodReferenceExpression>(children[0])) {
                        if (methodRef->getHasBorrowCaptures()) {
                            field->setHasBorrowCaptures(true);
                        }
                    }
                    // Ownership-transfer (option a) for spawn → local. The
                    // spawn pushed its own drop entry inside its
                    // generateCode so that bare-statement `spawn foo();`
                    // (no local to attach to) still gets freed at scope
                    // exit. When the result IS bound to a named local,
                    // that local's class-instance drop entry below
                    // becomes the canonical owner — mark the spawn's
                    // transient entry inactive so it doesn't double-fire.
                    // Mirrors how `#`-move-out marks the source inactive.
                    // See AsyncStatus.md § Plan: Task<T> as user-typeable
                    // template / Ownership-transfer model.
                    if (auto spawn = dynamic_pointer_cast<SpawnExpression>(children[0])) {
                        if (llvm::Value* spawnEntry = spawn->getDropEntry()) {
                            if (llvm::Function* markInactive = module->getRuntimeFunction(
                                    "__cajeta_drop_mark_inactive")) {
                                module->getBuilder()->CreateCall(
                                    markInactive, {spawnEntry});
                            }
                        }
                    }
                    // View-aliasing: when the initializer is a view
                    // construction call like `Header(bytes)`, record which
                    // field the view aliases. ReturnStatement consults this
                    // to reject returning a view of a same-scope local
                    // buffer. The shape we recognize:
                    //   - MethodCallExpression with NO receiver
                    //   - MCE's name matches a registered CajetaView
                    //   - Exactly one parameter, an IdentifierExpression
                    //     resolving to a field in the current scope
                    // Anything else (multi-arg ctor, dynamically-built
                    // buffer arg, etc.) is left untracked for v1 — the
                    // common footgun is `Header h = Header(localBytes);
                    // return h;` and that's what we catch.
                    if (auto mce = dynamic_pointer_cast<MethodCallExpression>(children[0])) {
                        bool isViewCtor = mce->getChildren().empty()
                            && mce->getParameters().size() == 1
                            && dynamic_pointer_cast<CajetaView>(
                                CajetaType::of(mce->getMethodCallName())) != nullptr;
                        if (isViewCtor) {
                            auto& mceParams = mce->getParameters();
                            auto argExpr = mceParams[0].expression;
                            // Owning vs borrow form (Views.md § Construction).
                            // `View(#buf)` transfers buffer ownership to the
                            // view; `View(buf)` borrows. Pre-Phase-1 the `#`
                            // produced a MoveExpression wrapper at the arg
                            // site; post-Phase-1 (#68) it sets
                            // MethodCallParameter::callerTransferred and the
                            // inner expression is the bare identifier. Either
                            // signal is the owning-form discriminator.
                            bool isOwning =
                                mceParams[0].callerTransferred
                                || isMoveKind(argExpr);
                            field->setIsOwningView(isOwning);
                            if (!isOwning) {
                                if (auto idArg = dynamic_pointer_cast<IdentifierExpression>(argExpr)) {
                                    auto scope = module->getScopeStack().peek();
                                    FieldPtr src = scope
                                        ? scope->getField(idArg->getTextValue())
                                        : nullptr;
                                    if (src) {
                                        field->setViewSource(src);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // spec 5.10 — an array local remembers which slots lend frame locals, from
            // the literal or the array it was bound from; escaping positions refuse it.
            if (initShape.leaf && initShape.leaf->kind() == ExprKind::ArrayLiteral) {
                auto lit = static_pointer_cast<ArrayLiteralExpression>(initShape.leaf);
                for (auto& p : lit->getBorrowedLocalSlots()) {
                    field->addSlotBorrowedLocal(p.first, p.second);
                }
            } else if (initShape.family == ownership::TitleFamily::Move
                       && initShape.field && initShape.field != field.get()) {
                field->copySlotBorrowedLocalsFrom(*initShape.field);
            }

            // Unit 4 — what remains here are the side effects the shape does not carry: borrow provenance and the §4.6 owned-bind check.
            if (initShape.leaf) {
                switch (initShape.leaf->kind()) {
                    case ExprKind::Identifier: {
                        // U3 (spec §7.2) — straight-line capture: `T v = p` off a plain
                        // formal makes `v` a borrow, so record the origin on the FIELD
                        // (identity, not name — names collide across methods).
                        auto srcId = static_pointer_cast<IdentifierExpression>(
                            initShape.leaf);
                        if (auto sc = module->getScopeStack().peek()) {
                            const string& srcName = srcId->getTextValue();
                            FieldPtr srcF = sc->getField(srcName);
                            if (dynamic_pointer_cast<ParameterField>(srcF)) {
                                field->setParamBorrowOrigin(srcName);
                                if (FieldPtr own = sc->getField(
                                        declarator->getIdentifier())) {
                                    own->setParamBorrowOrigin(srcName);
                                }
                            } else if (srcF
                                    && !srcF->getParamBorrowOrigin().empty()) {
                                // Chained: `T a = p; T b = a;` — carry it.
                                field->setParamBorrowOrigin(
                                    srcF->getParamBorrowOrigin());
                            }
                        }
                        break;
                    }
                    case ExprKind::MethodCall: {
                        auto mc = static_pointer_cast<MethodCallExpression>(
                            initShape.leaf);
                        // The callee the shape resolved — exact, since the call's own codegen already ran.
                        Method* rm = initShape.callee;
                        // 8.2.7 (spec §4.6) — a `#`-returning result bound with PLAIN `=`
                        // is rejected: `#=` puts the acquisition in the reader's view.
                        if (rm && rm->isReturnsOwnership()) {
                            auto holder = module->getCurrentMethod();
                            std::string in = holder
                                ? (holder->getParent()
                                    ? holder->getParent()->toCanonical() + "."
                                    : std::string())
                                    + holder->getName()
                                : std::string("<none>");
                            std::string calleeKey =
                                (rm->getParent()
                                    ? rm->getParent()->toCanonical() + "."
                                    : std::string()) + rm->getName();
                            // The audit is the sizing channel; in warn mode the check below records the same site more richly.
                            if (ownership::ReturnTitleAudit::enabled()
                                    && !ownership::ownedBindWarns()) {
                                // 8.1.4 — withhold the line inside a monomorphization: it is not a line of the file.
                                const bool synth = holder
                                    && (holder->isMethodTemplateInstantiation()
                                        || (holder->getParent()
                                            && holder->getParent()->isInstantiation()));
                                ownership::ReturnTitleAudit::ownedBind(
                                    calleeKey, in,
                                    synth ? 0 : mc->getSourceLine());
                            }
                            // Classpath demotion: a site inside a released dependency's internals is a note, never an error.
                            bool cpOrigin = module->isClasspathOrigin();
                            if (!cpOrigin && holder && holder->getParent()
                                    && holder->getParent()->getModule()) {
                                cpOrigin = holder->getParent()->getModule()
                                    ->isClasspathOrigin();
                            }
                            ownership::rejectPlainOwnedBind(
                                calleeKey, field->getName(),
                                module->getSourcePath(),
                                (int) mc->getSourceLine(), in, cpOrigin);
                        }
                        // 8.2.8 — the callee PROVED a borrow of its receiver's interior,
                        // so record which call lent this local (on both identities) and a
                        // later `#local` is rejected. Keyed on the callee, not the shape.
                        if (rm && !rm->isReturnsOwnership()
                                && (rm->isReturnsView()
                                    || (!rm->returnsStackValue()
                                        && rm->returnsInteriorView()))) {
                            auto rt = dynamic_pointer_cast<CajetaClass>(
                                rm->getReturnType());
                            // Only title-bearing results can be wrongly surrendered.
                            if (rt && !rt->isValueType()
                                    && !rt->isSharedCapableValue()) {
                                const string origin =
                                    mc->getMethodCallName() + "()";
                                const string& declName =
                                    declarator->getIdentifier();
                                field->setCallBorrowOrigin(origin);
                                if (auto sc = module->getScopeStack().peek()) {
                                    sc->recordCallBorrow(declName, origin);
                                    if (FieldPtr sf = sc->getField(declName)) {
                                        sf->setCallBorrowOrigin(origin);
                                    }
                                }
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            // script-units U4 (spec §4.6) — a session binding cannot HOLD a
            // borrow: the binding outlives the unit's frame, but the
            // borrowed owner may drop or rebind in any later unit, so the
            // borrow would dangle across the seam. Only the alias shapes
            // reject (identifier / field-read / element-read); literal
            // borrows of static storage and runtime-flagged call results
            // stay legal at top level.
            if (initIsBorrow && !initIsFlaggedCall && module->isScriptUnit()
                && module->isScriptBindingName(field->getName())) {
                // The alias shapes of a class-like value — a struct field or element read, a closure alias — stay legal.
                bool aliasShaped = false;
                if (initShape.leaf
                        && dynamic_pointer_cast<CajetaClass>(
                               initShape.leaf->getResolvedType())) {
                    ExprKind lk = initShape.leaf->kind();
                    aliasShaped = lk == ExprKind::Identifier
                        || ((lk == ExprKind::Dot || lk == ExprKind::ArrayIndex)
                            && !initShape.has(ownership::TitleShape::kView));
                }
                if (aliasShaped) {
                    auto* escPfn = module->getBuilder()->GetInsertBlock()
                                       ->getParent();
                    if (escPfn && escPfn->getName().find(scriptEntryName())
                                      != llvm::StringRef::npos) {
                        throw Exception(
                            "top-level binding `" + field->getName()
                                + "` cannot hold a borrow: session bindings "
                                  "outlive the unit, and the borrowed owner "
                                  "may drop or rebind in a later unit. Fix: "
                                  "transfer ownership (`" + field->getName()
                                + " #= ...`), bind a fresh value, or borrow "
                                  "inside a { } block for frame-local "
                                  "lifetime",
                            "CAJETA_ERROR_SESSION_BORROW_ESCAPE",
                            module->getScriptHostName(), getSourceLine(),
                            getSourceColumn());
                    }
                }
            }

            // Arena-eligible locals (frame-arena-plan U2/U3) register NO drop entry
            // — the scope-exit arena reset reclaims them in bulk. The escape pre-pass
            // guarantees they don't leave the frame. Name-keyed, so it covers both
            // the owned-String concat case (U2) and the primitive-array case (U3).
            bool arenaEligible = false;
            if (auto cm = module->getCurrentMethod()) {
                arenaEligible = cm->isArenaEligibleLocal(declarator->getIdentifier());
            }

            // Array-typed locals own the heap header; register
            // __cajeta_free_array unless this local is a borrow.
            // The borrow case (e.g. `T[] alias = paramArr` or
            // `T[] xs = obj.field`) already has an owner upstream;
            // duplicating the drop here double-frees at scope exit.
            // A Runtime answer arms the same entries from the flag.
            llvm::Value* arrFlag = initIsBorrow ? initTitleFlag : nullptr;
            if (isArray && (!initIsBorrow || arrFlag) && !arenaEligible) {
                // title-stores §3.2 — bit-capable-element arrays use ONE
                // walk+free entry so a move-out disarms both behaviors.
                shared_ptr<CajetaArray> arrT0 =
                    dynamic_pointer_cast<CajetaArray>(type);
                bool lvdElemTitled = arrT0 && !arrT0->isInlineArray()
                    && CajetaClass::arrayElementCarriesSlotBits(
                           arrT0->getElementType());
                bool lvdArrElem = arrT0 && !arrT0->isInlineArray()
                    && CajetaClass::arrayElementCarriesArraySlotBits(
                           arrT0->getElementType());
                bool lvdMemberBits = arrT0 && !arrT0->isInlineArray()
                    && CajetaClass::arrayElementCarriesMemberBits(
                           arrT0->getElementType());
                if (lvdElemTitled || lvdArrElem) {
                    const llvm::DataLayout& ldl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Function* wf = getOrCreateTailDropFree(module,
                        ldl.getTypeAllocSize(arrT0->getLlvmType()),
                        arrT0->elementStrideBytes(ldl,
                            module->getLlvmContext()),
                        lvdArrElem
                            ? CajetaClass::arrayElementInnerDropKind(
                                  arrT0->getElementType())
                            : -1);
                    emitDropEntryForFn(module, field, wf, getSourceLine(), arrFlag);
                } else if (lvdMemberBits) {
                    // title-stores §3.3.2 — value-struct elements: ONE
                    // member-walk+free entry (same fused shape as the
                    // tail family, same move-out disarm reason).
                    const llvm::DataLayout& ldl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Module* lm = module->getBuilder()
                        ->GetInsertBlock()->getParent()->getParent();
                    llvm::Function* wf = CajetaClass::getOrCreateMemberWalk(
                        module, lm,
                        dynamic_pointer_cast<CajetaClass>(
                            arrT0->getElementType()),
                        ldl.getTypeAllocSize(arrT0->getLlvmType()),
                        arrT0->elementStrideBytes(ldl,
                            module->getLlvmContext()),
                        /*withFree=*/true);
                    if (wf) {
                        emitDropEntryForFn(module, field, wf, getSourceLine(), arrFlag);
                    } else {
                        emitDropEntryFor(module, field,
                            "__cajeta_free_array", getSourceLine(), arrFlag);
                    }
                } else {
                    emitDropEntryFor(module, field, "__cajeta_free_array",
                        getSourceLine(), arrFlag);
                }
                // slices 9.2.1 — a local String[] owns what its stores TOOK
                // (the array-slot store already deactivates an identifier
                // source's drop entry); register the element walk that
                // finally reclaims them.
                // title-tracking Unit 4 — plain class elements get the same
                // sidecar with title semantics: the store's SPELLING marks
                // the slot bit (`a[i] = #x` owned, plain store borrowed),
                // the walk releases marked slots via the vtable drop.
                if (auto arrT = dynamic_pointer_cast<CajetaArray>(type)) {
                    auto elemCls = dynamic_pointer_cast<CajetaClass>(
                        arrT->getElementType());
                    bool elemIsString = elemCls
                        && !dynamic_pointer_cast<CajetaView>(arrT->getElementType())
                        && elemCls->getQName()
                        && elemCls->getQName()->getTypeName() == "String"
                        && elemCls->getQName()->getPackageName() == "cajeta.lang";
                    bool elemTitled = !elemIsString
                        && CajetaClass::arrayElementCarriesSlotBits(
                               arrT->getElementType());
                    if (elemTitled && !arrT->isInlineArray()) {
                        // title-stores §3.2 — the walk rides the single
                        // walk+free entry registered above; just make sure
                        // the element class's vtable drop is patched. The
                        // class-element sidecar retires; Strings keep it.
                        elemCls->patchVirtualTableDropFn();
                    } else if (elemIsString && !arrT->isInlineArray()) {
                        emitArrayElemDropEntry(module, field, arrT,
                            "__cajeta_string_array_owned_drop",
                            getSourceLine(), arrFlag);
                        // spec 5.10 — a literal's String slots are the array's: mark them so the teardown walk frees them.
                        if (initShape.leaf
                                && initShape.leaf->kind() == ExprKind::ArrayLiteral
                                && field->getElemOwnSidecar()) {
                            if (llvm::Function* markFn = module->getRuntimeFunction(
                                    "__cajeta_string_array_sidecar_mark_all")) {
                                module->getBuilder()->CreateCall(markFn, {
                                    field->getElemOwnSidecar(),
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(*module->getLlvmContext()),
                                        (uint64_t) initShape.leaf->getChildren().size())});
                            }
                        }
                    }
                }
            }

            // Gap 4 — record a live read-borrow on the scope so a later
            // write through the borrowed path (or any prefix of it)
            // rejects with CAJETA_ERROR_MOVE_OF_BORROW before clobbering
            // the borrowed slot. Triggered for the borrow shapes
            // detected above: field/array reads of class refs
            // (`String alias = p.name`) and local-to-local aliases of
            // class refs (`String alias = other`). Struct-typed locals
            // initIsBorrow shape ALSO goes through here, which the
            // existing alias machinery already treats as a move — the
            // borrow record is harmless there since the source is
            // simultaneously marked moved.
            // Gap 4 (live read-borrows) and Gap 5 (owned String drops).
            // A String / class / array local with a path-shaped
            // initializer (`String alias = p.name;`, `Foo b = a;`)
            // aliases its source — record a live borrow so a later
            // write to the source's path rejects.
            // A String local initialized from a known-allocating
            // string helper (`String r = "hello".concat(" world");`
            // or `String r = a + b;`) owns the malloc'd buffer —
            // register a free drop entry so the buffer doesn't leak
            // at scope exit. Two paths handled here:
            //   1. MethodCallExpression on a String receiver with an
            //      allocating method name (concat/substring/
            //      toUpperCase/toLowerCase/trim/replace).
            //   2. BinaryOpExpression with operator + producing a
            //      pointer-typed result (lowered via
            //      __cajeta_str_concat in BinaryOpExpression::ADD).
            //
            // This drop registration applies only to the LEGACY
            // primitive-alias String path (i8* C-strings, with malloc'd
            // buffers that need free). cajeta.lang.String (the class
            // form) follows the never-drop rule per
            // docs/specification/lang/String.md § Memory model — its
            // method implementations don't register drops at all, and
            // its substring is a view, not an allocation.
            //
            // The two categories are mutually exclusive: only one of
            // borrow / owned applies to a given initializer.
            // Borrow recording fires for any local that aliases its
            // source rather than copying. Two conditions admit:
            //   (a) llvmType is a pointer (legacy primitive-String /
            //       array shapes — heap header is a malloc'd buffer).
            //   (b) type is a CajetaClass (class instance — the local
            //       holds a heap pointer in a struct-typed slot).
            // Interfaces and views aren't borrow candidates (their
            // own slots are fat-pointers / inline structs).
            bool typeIsClass = std::dynamic_pointer_cast<CajetaClass>(type) != nullptr
                && type
                && !(type->getQName()
                    && std::dynamic_pointer_cast<CajetaClass>(type)->isInterface());
            bool ptrLike = type && type->getLlvmType()
                && type->getLlvmType()->isPointerTy();
            if (field && initializer && type && (ptrLike || typeIsClass)) {
                if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                    auto& children = varInit->getChildren();
                    if (!children.empty()) {
                        auto child = children[0];

                        // Borrow recording (Gap 4).
                        string borrowedPath;
                        if (auto dot = dynamic_pointer_cast<DotExpression>(child)) {
                            borrowedPath = DotExpression::buildPath(dot);
                        } else if (auto id = dynamic_pointer_cast<IdentifierExpression>(child)) {
                            borrowedPath = id->getTextValue();
                        }
                        if (!borrowedPath.empty()) {
                            if (auto sc = module->getScopeStack().peek()) {
                                sc->recordLiveBorrow(field->getName(), borrowedPath);
                            }
                        }
                    }
                }
            }

            // Owning view (`View(#buf)`): the view took ownership of the
            // buffer from its source (the MoveExpression deactivated the
            // source's drop entry inside MoveExpression::generateCode).
            // Register a fresh drop entry against the view's data pointer
            // — __cajeta_view_drop_owned reconstructs the array header by
            // subtracting the 8-byte header offset and frees it.
            if (field->isOwningView()) {
                emitDropEntryFor(module, field, "__cajeta_view_drop_owned", getSourceLine());
            }

            // L3-3: function-typed locals own the closure record they
            // point at (for capturing closures) and need a drop entry
            // that fires __cajeta_closure_drop at scope exit. Non-
            // capturing closures store a stack-allocated record with
            // drop_fn=null, so the runtime helper no-ops on them; the
            // entry shape is therefore safe for every function-typed
            // local regardless of what it holds. ReturnStatement
            // deactivates the entry when the local is returned so
            // ownership transfers to the caller without a double-free.
            //
            // ...but ONLY the OWNING binding may drop. A function-typed
            // local that merely ALIASES an existing closure — `f = other`
            // (an identifier) or `f = obj.field` (a stored handler, e.g.
            // `h = srv.handler`) — is a BORROW: the original binding still
            // owns the shared heap record. Registering a second drop here
            // double-frees a CAPTURING closure's record when both fire
            // (the escaping-closure-in-a-server crash: the server stores
            // the handler, several locals/copies alias it, and each freed
            // the one record). A fresh `(x) -> …` lambda OWNS; a `#move`
            // (MoveExpression, not a bare identifier/dot) transfers
            // ownership and OWNS; a call returning an owned `#` closure
            // (e.g. MiddlewareChain.compose) OWNS. Only a bare
            // identifier / dot borrow is skipped — the exact borrow rule
            // Strings use (initIsBorrow) for the same aliasing hazard.
            if (dynamic_pointer_cast<CajetaFunctionType>(type)) {
                // A bare identifier or field read aliases an existing closure — a borrow.
                // A `#x` of a runtime owner arms the entry from the source's flag.
                bool closureIsBorrow = false;
                llvm::Value* clFlag = nullptr;
                if (initShape.leaf) {
                    ExprKind ck = initShape.leaf->kind();
                    closureIsBorrow = ck == ExprKind::Identifier || ck == ExprKind::Dot;
                    if (initShape.family == ownership::TitleFamily::Move
                            && initShape.answer != ownership::TitleAnswer::Owned) {
                        clFlag = ownership::titleFlag(initShape, module);
                        if (auto* c0 = llvm::dyn_cast<llvm::ConstantInt>(clFlag)) {
                            if (c0->isZero()) closureIsBorrow = true;
                            clFlag = nullptr;
                        }
                    }
                }
                if (!closureIsBorrow) {
                    emitDropEntryFor(module, field, "__cajeta_closure_drop",
                                     getSourceLine(), clFlag);
                }
            }

            // User-defined-drop wiring for class-instance locals.
            // Arrays and structs are handled separately above; struct
            // values live inline (no heap), and arrays have their own
            // __cajeta_free_array path. For everything else that's a
            // class — including the constructor-ref result — register
            // the class's synthesized drop wrapper so the instance is
            // reclaimed at scope exit (running any user-declared
            // `drop()` method first). ReturnStatement deactivates this
            // entry when the local is returned, transferring ownership
            // to the caller.
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            bool isStructType = dynamic_pointer_cast<CajetaView>(type) != nullptr;
            // P3 — when there's no initializer, the slot's contents are
            // undefined at declaration time. Registering a drop here
            // would capture the slot's value at PUSH time (garbage), so
            // a later `c = heap Counter()` assignment would leave the
            // drop entry pointing at the original garbage and free that
            // address at scope exit (crash). Skip the drop registration
            // in the no-initializer case; this leaks the heap instance
            // assigned via the later `c = heap X()` but doesn't crash.
            // Follow-on work: defer drop registration until first
            // assignment (or shift the drop entry to load the slot at
            // fire time instead of push time).
            // cajeta.lang.String is process-lifetime — view-mode
            // literals live in static storage, owned-mode allocations
            // (concat results, substring copies, etc.) are intentionally
            // never reclaimed per the never-drop spec
            // (docs/specification/lang/String.md § Memory model). Skip
            // the drop wiring entirely; vtable.drop_fn stays NULL.
            // (Reclaiming would require a boundary-transfer mechanism
            // for cajeta heap that escapes to C++ via the JIT lookup
            // — MD5/SipHash/XXHash3 test frameworks return `s.bytes` to
            // the test and read from it after the cajeta function
            // returns. Without that mechanism, enabling drops here
            // turns those tests into use-after-frees.)
            bool isCajetaString = klass && klass->getQName()
                && klass->getQName()->getTypeName() == "String"
                && klass->getQName()->getPackageName() == "cajeta.lang";
            // Owned cajeta.lang.String locals: register the mode-aware string
            // drop (docs/specification/lang/String.md § Memory model — the
            // owned/view distinction the drop chain was designed for). At scope
            // exit __cajeta_string_drop frees the byte buffer ONLY for owned
            // (mode 0) strings — concat results (`"k" + i`), substring/upper/
            // lower/trim/replace copies — and is a no-op for view-mode literals
            // and slices (mode 1, bytes borrowed) and static wrappers (live-set
            // claim fails). Borrowed aliases (`String b = a;`, field/element
            // reads) set initIsBorrow above and skip here, so no double-free.
            // `#`-transfer into a container deactivates this entry (the container
            // takes the drop). Returning the local deactivates it too
            // (ReturnStatement), transferring ownership to the caller. This
            // retires the former never-drop policy that leaked every dynamically
            // built String.
            // Arena-eligible concat locals (frame-arena-plan U2) register NO drop
            // entry — the scope-exit arena reset reclaims them in bulk. The escape
            // pre-pass guarantees they don't leave the frame. `arenaEligible` was
            // computed once above (name-keyed; covers String U2 + array U3).
            // A Runtime answer arms the mode-aware string drop from its flag, so a title riding out of a plain call is not leaked.
            if (isCajetaString && !isArray && !isStructType && initializer
                    && !initIsStackAlloc && !arenaEligible
                    && (!initIsBorrow || initTitleFlag)) {
                llvm::Value* sFlag = initIsBorrow ? initTitleFlag : nullptr;
                if (auto* cflag = llvm::dyn_cast_or_null<llvm::ConstantInt>(sFlag)) {
                    if (!cflag->isZero()) {
                        emitDropEntryFor(module, field, "__cajeta_string_drop",
                                         getSourceLine());
                    }
                } else {
                    emitDropEntryFor(module, field, "__cajeta_string_drop",
                                     getSourceLine(), sFlag);
                }
            }
            // 9.3.1 — a BARE String declaration (`String d;`) registers a
            // NULL-obj entry in its OWN frame so a later move-assign —
            // possibly in an inner block — can retarget it in place
            // (BinaryOpExpression stores the moved ptr into e->obj). The
            // drop chain is strict LIFO, so the entry must be registered at
            // the declaration, never at the assignment. If no move ever
            // lands, the drop of the null obj no-ops. The slot is nulled
            // first so the entry captures a well-defined obj (bare class
            // slots are otherwise uninitialized; definite-assignment still
            // gates reads).
            if (isCajetaString && !isArray && !isStructType && !initializer) {
                auto* b = module->getBuilder();
                auto& lctx2 = *module->getLlvmContext();
                b->CreateStore(
                    llvm::ConstantPointerNull::get(
                        llvm::PointerType::get(lctx2, 0)),
                    field->getOrCreateAllocation());
                emitDropEntryFor(module, field, "__cajeta_string_drop", getSourceLine());
            }
            // title-tracking 2.2.4 — the same null-obj entry for a BARE
            // class-typed declaration (`Cell keep;`), so an inner-block
            // move-assign can retarget it in the declaring frame. Virtual
            // drop of null no-ops (generalizes the String-only 9.3.1 fix).
            if (klass && !isCajetaString && !isArray && !isStructType
                    && !initializer && !klass->isInterface()
                    && !klass->isValueType()
                    && klass->hasVtablePointerAtSlotZero()) {
                auto* b = module->getBuilder();
                auto& lctx2 = *module->getLlvmContext();
                b->CreateStore(
                    llvm::ConstantPointerNull::get(
                        llvm::PointerType::get(lctx2, 0)),
                    field->getOrCreateAllocation());
                klass->patchVirtualTableDropFn();
                emitDropEntryFor(module, field,
                    "__cajeta_class_virtual_drop", getSourceLine());
            }
            // @ValueType locals are Copy PODs living inline in their slot —
            // never heap-backed, no owned fields, no destructor. They must NOT
            // enter the drop chain: a drop-push here would load the slot's
            // first word (the vtable pointer) and register the value-type body
            // for a spurious stack/virtual drop at scope exit. Skip entirely.
            // A Runtime answer arms the same entry from the flag, where a static owner pushes alone.
            llvm::Value* cFlag = initIsBorrow ? initTitleFlag : nullptr;
            if (klass && !isArray && !isStructType && !klass->isInterface()
                    && initializer && !isCajetaString && !klass->isValueType()
                    && (!initIsBorrow || (cFlag && !field->getDropEntry()))) {
                // P7.1/P7.2 — stack-allocated class locals (init via
                // `stack ClassName(...)` or `stack ClassName { ... }`)
                // get the stack-drop variant: walks owned class-ref
                // fields + recurses into embedded structs, but does
                // NOT free the body (function epilogue handles that).
                // Stack allocation fixes the dynamic type (alloca size
                // is sized for the declared class), so static dispatch
                // here is correct.
                //
                // Heap-class locals go through __cajeta_class_virtual_drop
                // — the dispatcher loads the instance's vtable and calls
                // the drop_fn slot, so a base-typed local holding a
                // derived instance (`Animal a = heap Dog()`) fires
                // ~Dog() rather than ~Animal() (MemoryModel.md Gap 1).
                // The static per-class drop wrappers are still emitted
                // and reachable: they live in vtable.drop_fn and the
                // dispatcher routes through them.
                if (initIsStackAlloc) {
                    // Script units (spec §4.7): a session binding outlives the
                    // entry's frame, so `stack` cannot bind at top level —
                    // uniform rule, single- and multi-unit hosts alike.
                    if (module->isScriptUnit()
                        && module->isScriptBindingName(field->getName())) {
                        auto* pfn = module->getBuilder()->GetInsertBlock()
                                        ->getParent();
                        if (pfn && pfn->getName().find(scriptEntryName())
                                       != llvm::StringRef::npos) {
                            throw Exception(
                                "top-level binding '" + field->getName()
                                    + "' cannot be stack-allocated: session"
                                      " bindings outlive the unit's frame —"
                                      " use `heap`, or bind inside a { }"
                                      " block for frame-local lifetime",
                                "CAJETA_ERROR_SESSION_STACK_BINDING");
                        }
                    }
                    // Record the storage class for later analyses — the `#`
                    // return escape check needs it (stack-return-transfer-error
                    // spec §2.1) and this declaration is the only site that
                    // knows: storage lives on the construction, not the type.
                    field->setStackInstance(true);
                    // Skip the drop entry entirely when the stack drop is a
                    // no-op (primitive-only value types like Instant/LocalDate):
                    // registering + running an empty drop per scope is the whole
                    // cost in tight loops (time-* ~230x — see codegen-perf-levers
                    // plan / reference_noop_drop_stack_value_type_tax). Stack
                    // allocation fixes the dynamic type, so static triviality is
                    // sound here.
                    if (!klass->hasTrivialStackDrop()) {
                        if (llvm::Function* stackDropFn =
                                klass->getOrCreateStackDropFunction()) {
                            emitDropEntryForFn(module, field, stackDropFn, getSourceLine());
                        }
                    }
                } else if (klass->hasVtablePointerAtSlotZero()) {
                    // Patch this class's vtable drop_fn slot with the
                    // synthesized heap-drop wrapper, then register the
                    // runtime dispatcher (Gap 1 — virtual dispatch on
                    // drop). The dispatcher loads vtable.drop_fn from
                    // the instance at scope exit, so a base-typed local
                    // holding a derived instance fires the derived's
                    // destructor.
                    klass->patchVirtualTableDropFn();
                    emitDropEntryFor(module, field,
                        "__cajeta_class_virtual_drop", getSourceLine(), cFlag);
                } else {
                    // Custom-layout classes (CajetaTask<T> — no vtable
                    // pointer at slot 0). The virtual dispatcher can't
                    // safely read from instance[0], so fall back to
                    // static dispatch. These types are monomorphic by
                    // construction (no user subclassing), so the static
                    // per-class drop fn is both correct and sufficient.
                    if (llvm::Function* dropFn =
                            klass->getOrCreateDropFunction()) {
                        emitDropEntryForFn(module, field, dropFn, getSourceLine(), cFlag);
                    }
                }
            }

            // Stack-resident class instances flow through the regular
            // stack-drop path via klass->getOrCreateStackDropFunction()
            // wired earlier in this method.

            // reassign-leak family (2026-09-07) — a local that holds a BORROW
            // (or an arena value) now may be assigned an OWNED value later
            // (`Cell k = h.a; ... k = heap Cell(7);`). The drop chain is
            // strict LIFO, so the entry that assignment will re-arm has to
            // exist in THIS frame: register it now, INACTIVE (a drop of a
            // borrow is a double free), and let the assignment's
            // inline re-arm (5.2.3) release what it displaces.
            // Only when some assignment to this name in the method body has
            // an owned-shaped right-hand side — every other local pays
            // nothing. Pinned by ReassignOwnershipTests.
            // Not for a SESSION binding: the session registry owns it and the rebind
            // protocol drops the displaced value, so a frame entry would double-drop.
            if (!field->getDropEntry() && !field->isSessionBound()
                    && initializer && !isStructType
                    && (isCajetaString || isArray
                        || (klass && !klass->isInterface()
                            && !klass->isValueType()
                            && !klass->isSharedCapableValue()
                            && klass->hasVtablePointerAtSlotZero()))) {
                auto mR = module->getCurrentMethod();
                BlockPtr bodyR = mR ? mR->getBlock() : nullptr;
                if (bodyR && nameReassignedOwned(bodyR, declarator->getIdentifier())) {
                    const char* reDrop = isCajetaString ? "__cajeta_string_drop"
                        : isArray ? "__cajeta_free_array"
                                  : "__cajeta_class_virtual_drop";
                    if (!isCajetaString && !isArray) klass->patchVirtualTableDropFn();
                    emitFlaggedDropEntryFor(module, field, reDrop,
                        llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(*module->getLlvmContext()), 0),
                        getSourceLine());
                    field->setRuntimeConditionalOwner(true);
                }
            }

            // Interface local drop entry. Pushes a drop entry
            // pointing at __cajeta_iface_drop, the kind-tag dispatcher.
            // The helper reads the fat pointer's kind word and either
            // invokes the per-(class, iface) vtable's drop slot
            // (OWNED_CLASS) or no-ops (BORROWED_*).
            // Unit 4 — NOT for a proven borrow: the local then aliases a body someone
            // else owns, and the entry would free it. A Runtime answer arms from its flag.
            if (klass && klass->isInterface()
                    && initVerdict.answer != ownership::TitleAnswer::Borrow) {
                emitDropEntryFor(module, field, "__cajeta_iface_drop",
                                 getSourceLine(),
                                 initIsFlaggedCall ? initTitleFlag : nullptr);
            }

            // Debugger CP5/CP7-1b: register this local in the current debug
            // frame so it shows up in DAP `variables`. No-op unless --debug-info.
            // Emitted HERE, after all drop-chain wiring above, so the ownership
            // facet sees the final drop entry (an owner local — including an
            // owning View — has a non-null drop entry by now; a borrow does
            // not). alloc class comes from the StackField/HeapField choice
            // (primitive inline value vs heap pointer). `shared`/`transferred`
            // stay deferred (CP7 defers XPU placement + move tracking).
            if (type) {
                dbg::FieldFacetInputs facetIn;
                // The StackField/HeapField split describes the SLOT (inline
                // value vs pointer), which for a class local is always a
                // pointer — so `stack Point p = stack Point(3,4)` reported
                // heap. The alloc facet describes where the INSTANCE lives, so
                // a `stack` creator wins over the slot kind.
                facetIn.isStackField = initIsStackAlloc
                    || dynamic_pointer_cast<StackField>(field) != nullptr;
                facetIn.isHeapField  = !initIsStackAlloc
                    && dynamic_pointer_cast<HeapField>(field) != nullptr;
                facetIn.isReference  = field->isReference();
                facetIn.ownsDrop     = field->getDropEntry() != nullptr;
                dbg::emitDbgLocal(module, field->getName(),
                                  type->toCanonical(),
                                  field->getOrCreateAllocation(),
                                  dbg::classifyField(facetIn),
                                  field->getDropEntry());
            }

            // Session binding for the two cases the owner path can't reach —
            // last, so both read a fully-initialized slot.
            maybeEmitSessionBindValue(module, field, type);
            maybeEmitSessionBindBorrow(module, field, type);
        }

        return nullptr;
    }

} // code