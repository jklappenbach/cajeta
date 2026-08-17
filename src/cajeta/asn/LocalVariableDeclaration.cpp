//
// Created by James Klappenbach on 11/4/22.
//

#include "LocalVariableDeclaration.h"
#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../compile/ScriptUnitSynthesis.h"
#include "cajeta/ownership/OwnedBindCheck.h"
#include "cajeta/ownership/ReturnTitleAudit.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "../field/HeapField.h"
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
    }

    // Emit drop-chain wiring for an owner local. Allocates a DropEntry blob on
    // the stack at function entry, pushes it onto the runtime's chain right
    // after the owner is materialized, and records the entry on both the field
    // and the enclosing method so scope-exit emits the matching pop. In debug
    // mode (CompilerFlags::sourceTags) the push variant carries the LVD's
    // source file + line for runtime diagnostics.
    static void emitDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                  const std::string& dropFnName,
                                  int allocLine = 0) {
        DropPushChoice push = pickDropPush(module);
        llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
        if (!push.pushFn || !dropFn) return;
        if (maybeEmitSessionBind(module, field, dropFn)) return;
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
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn});
        }

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
    }

    // title-stores §3.2 — per-stride walk+free wrapper for local
    // bit-array drop entries. Element slots stride by the element
    // STRUCT's alloc size (a pre-existing class-element array layout
    // convention), so the stride is a per-type constant the one-arg
    // drop-entry ABI can't carry — synthesize a tiny fn per (hs, es).
    static llvm::Function* getOrCreateTailDropFree(CajetaModulePtr module,
            uint64_t hs, uint64_t es) {
        llvm::Module* m =
            module->getBuilder()->GetInsertBlock()->getParent()->getParent();
        std::string name = "cajeta_tail_dropfree_hs" + std::to_string(hs)
            + "_es" + std::to_string(es);
        if (llvm::Function* f = m->getFunction(name)) return f;
        auto& fctx = m->getContext();
        auto* fptrTy = llvm::PointerType::get(fctx, 0);
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(fctx), {fptrTy}, false);
        auto* f = llvm::Function::Create(fnTy,
            llvm::Function::InternalLinkage, name, m);
        auto* bb = llvm::BasicBlock::Create(fctx, "entry", f);
        llvm::IRBuilder<> fb(bb);
        llvm::Function* walk = module->getRuntimeFunction(
            "__cajeta_tail_elem_drop_walk");
        llvm::Function* freeA = module->getRuntimeFunction(
            "__cajeta_free_array");
        auto* fi64 = llvm::Type::getInt64Ty(fctx);
        if (walk) fb.CreateCall(walk, {f->getArg(0),
            llvm::ConstantInt::get(fi64, hs),
            llvm::ConstantInt::get(fi64, es)});
        if (freeA) fb.CreateCall(freeA, {f->getArg(0)});
        fb.CreateRetVoid();
        return f;
    }

    // Public bridge for post-declaration ownership creation (9.3.1
    // move-assign into a bare-declared local). Same wiring; binds the
    // slot's CURRENT value, so callers invoke it after their store.
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
            const char* walkDropFn, int allocLine = 0) {
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

        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn,
                {entryPtr, sidecar, dropFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, sidecar, dropFn});
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
        DropPushChoice push = pickDropPush(module);
        llvm::Function* dropFn = module->getRuntimeFunction(dropFnName);
        llvm::Function* setFlagFn = module->getRuntimeFunction(
            "__cajeta_drop_set_flag");
        if (!push.pushFn || !dropFn || !setFlagFn || !flag) return;
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

        llvm::Value* ownerPtr = builder->CreateLoad(
            ptrTy, field->getOrCreateAllocation());
        if (push.debug) {
            llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                module->getSourcePath());
            llvm::Constant* lineConst = llvm::ConstantInt::get(i32Ty, allocLine);
            builder->CreateCall(push.pushFn,
                {entryPtr, ownerPtr, dropFn, fileConst, lineConst});
        } else {
            builder->CreateCall(push.pushFn, {entryPtr, ownerPtr, dropFn});
        }
        builder->CreateCall(setFlagFn, {entryPtr, flag});

        field->setDropEntry(entryPtr);
        if (auto m = module->getCurrentMethod()) m->registerDropEntry(entryPtr);
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
                                    int allocLine = 0) {
        DropPushChoice push = pickDropPush(module);
        if (!push.pushFn || !dropFn) return;
        // Cross-module: when the class whose drop fn we're pushing
        // lives in a different llvm::Module (a stdlib class
        // referenced from user code), substitute a module-local
        // extern decl so the merge step resolves the Constant.
        dropFn = CajetaModule::ensureFunctionInModule(
            module->getLlvmModule(), dropFn);
        if (maybeEmitSessionBind(module, field, dropFn)) return;
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
    static bool nameEscapesScope(const AbstractSyntaxNodePtr& node,
                                 const std::string& name) {
        if (!node) return false;
        bool blocking = dynamic_pointer_cast<ReturnStatement>(node)
            || dynamic_pointer_cast<MoveExpression>(node)
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
                        if (auto mvWrap = dynamic_pointer_cast<MoveExpression>(init0)) {
                            auto& mvKids = mvWrap->getChildren();
                            init0 = mvKids.empty() ? nullptr
                                : dynamic_pointer_cast<Expression>(mvKids[0]);
                        }
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
                    if (auto outerMv = dynamic_pointer_cast<MoveExpression>(
                            fwdKids[0])) {
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
                                slotMv = dynamic_pointer_cast<MoveExpression>(src);
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

            // 5.2.3 — the initializer's IR (including the call) was emitted by
            // getOrCreateAllocation just above, so THIS is the only point where
            // the return-flag TLS still holds this call's title bit. Capture it
            // now; the drop-wiring section below decides whether to arm an entry
            // with it. Only class-typed call initializers can carry a title.
            llvm::Value* callResultFlag = nullptr;
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(
                    initializer)) {
                auto& kids = varInit->getChildren();
                // 7.2.5 — closure invocations (CallExpression) ride the same
                // paired return flag as method calls.
                if (!kids.empty()
                        && (dynamic_pointer_cast<MethodCallExpression>(kids[0])
                            || dynamic_pointer_cast<CallExpression>(kids[0]))) {
                    if (llvm::Function* getFlagFn = module->getRuntimeFunction(
                            "__cajeta_return_flag_get")) {
                        callResultFlag = module->getBuilder()->CreateCall(
                            getFlagFn, {}, "ret_flag");
                    }
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

                            // Class RHS wrapped in MoveExpression
                            // (`Greeter g = #h;` or `Greeter g = #heap Hello();`)
                            // sets OWNED_CLASS; bare class RHS sets
                            // BORROWED_CLASS. MoveExpression's own
                            // generateCode has already marked the source
                            // moved + deactivated its drop entry, so
                            // ownership transfers cleanly to the interface
                            // value's drop chain (drop dispatches on kind).
                            bool rhsIsMove = dynamic_pointer_cast<MoveExpression>(rhsExpr) != nullptr;
                            int64_t kindValue;
                            if (rhsIsMove) {
                                kindValue = IFACE_KIND_OWNED_CLASS;
                            } else {
                                kindValue = IFACE_KIND_BORROWED_CLASS;
                            }
                            builder->CreateStore(
                                llvm::ConstantInt::get(i64Ty,
                                    (uint64_t) kindValue),
                                kindSlot);
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
                                || dynamic_pointer_cast<MoveExpression>(argExpr) != nullptr;
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

            // Borrow detection runs FIRST so each drop-entry site
            // below can honor it. Without this ordering, the array
            // drop fires unconditionally and `T[] alias = paramArr`
            // double-frees at scope exit.
            //
            // When the RHS does NOT transfer ownership of a fresh
            // allocation to the local, the local must NOT register a
            // drop entry. Recognized borrow sources:
            //
            //   1. __cajeta_inject() — returns the DI singleton
            //      (A9). A drop here would double-free on the
            //      second @Inject site or free state still in use
            //      elsewhere.
            //   2. DotExpression reading a class-typed field — the
            //      local now points at an instance owned by
            //      whatever object holds the field. The owner is
            //      responsible for the drop; the borrowing local
            //      must not duplicate it.
            //
            // Both shapes are observable directly from the
            // initializer expression's AST type. NewExpression
            // (fresh malloc) and most other initializers retain
            // their ownership-transfer semantics.
            bool initIsBorrow = false;
            // 5.2.3 — the initializer is a call whose class-pointer result
            // carries a runtime title flag (armed below, not statically).
            bool initIsFlaggedCall = false;
            // P2a — `stack MyClass(args)` produces an instance owned by
            // the current frame (alloca-backed body); the class's heap
            // destructor would free a stack pointer if we registered the
            // usual drop entry. Detect by inspecting the init AST for a
            // NewExpression with stackAlloc=true and skip the heap-drop
            // registration. KNOWN LIMITATION: owned class-ref FIELDS of
            // a stack-allocated owner leak in v1 because we skip the
            // drop entirely; Phase 2b adds a stack-drop variant that
            // walks owned fields without freeing the body.
            bool initIsStackAlloc = false;
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    auto rhsExpr = dynamic_pointer_cast<Expression>(children[0]);
                    // A string-literal initializer (`String s = "x";`) aliases
                    // static/.rodata storage — a view (mode 1) that owns nothing.
                    // It must NOT get an owned-String drop entry: the drop would
                    // fire as a no-op (live-set claim fails) but is wasteful and
                    // semantically wrong (a literal alias is a borrow). Mark borrow.
                    if (auto litExpr = dynamic_pointer_cast<TextLiteralExpression>(children[0])) {
                        LiteralType lt = litExpr->getLiteralType();
                        if (lt == LITERAL_TYPE_STRING || lt == LITERAL_TYPE_TEXT_BLOCK) {
                            initIsBorrow = true;
                        }
                    }
                    if (auto newExpr = dynamic_pointer_cast<NewExpression>(children[0])) {
                        if (newExpr->getStackAlloc()) {
                            initIsStackAlloc = true;
                        }
                    }
                    // P2b — `stack MyClass { f: v }` on a plain class
                    // returns an alloca'd body pointer; the class-drop
                    // would free a stack pointer. Detect by inspecting
                    // for stack-flagged AggregateInitializerExpression.
                    if (auto aggExpr = dynamic_pointer_cast<AggregateInitializerExpression>(children[0])) {
                        if (aggExpr->getStackAlloc()) {
                            initIsStackAlloc = true;
                        }
                    }
                    // 7.2.5 — bare-name closure invocation: no Method to
                    // resolve, but the synthesized callee sets the paired
                    // return flag; arm a flag-fed entry for class-ptr
                    // returns.
                    if (auto ce = dynamic_pointer_cast<CallExpression>(children[0])) {
                        if (!ce->getResolvedType()) ce->resolveTypes(module);
                        auto ceCls = dynamic_pointer_cast<CajetaClass>(
                            ce->getResolvedType());
                        if (ceCls && !dynamic_pointer_cast<CajetaView>(
                                ce->getResolvedType())
                                && !ceCls->isValueType()) {
                            initIsBorrow = true;       // static owner path off
                            initIsFlaggedCall = true;  // the flag decides
                        }
                    }
                    // U3 (spec §7.2) — straight-line capture. `T v = p;`
                    // where `p` is a plain FORMAL makes `v` a borrow of the
                    // caller's value, so a later `this.f = v` is the same
                    // capture as `this.f = p`, one hop later. Record the
                    // origin on the FIELD (identity, not name — a by-name
                    // map matched same-named locals across methods in U2).
                    if (auto srcId = dynamic_pointer_cast<IdentifierExpression>(
                            children[0])) {
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
                    }
                    if (auto mc = dynamic_pointer_cast<MethodCallExpression>(children[0])) {
                        // stdlib-ownership-convention U2 — record which call
                        // lent this local, so a later `#local` can be
                        // rejected naming both ends (spec 3.1, 4.3).
                        //
                        // Sited here, on the call expression's OWN resolved
                        // method, rather than in the targetCls block below:
                        // that block re-resolves the callee from the
                        // receiver's type, which comes back null for a
                        // user-class receiver (`Cell c = b.borrowCell()`
                        // reaches this point but never enters it), so the
                        // provenance was being lost for exactly the code
                        // this check exists to protect.
                        if (!mc->getResolvedType()) {
                            mc->resolveTypes(module);
                        }
                        if (MethodPtr rm = mc->getResolvedMethod()) {
                            // 8.2.7 sizing (spec §4.6) — this local binds a
                            // `#`-returning result with PLAIN `=`. The `#=`
                            // form wraps its initializer in a MoveExpression
                            // and so never reaches this cast, which makes
                            // arriving here with an owning callee exactly the
                            // population §4.6 would require to change.
                            // Counted AND rejected as of 8.2.7: the sizing
                            // record below still feeds the migration harvest
                            // when the audit is on, and rejectPlainOwnedBind
                            // then applies §4.6 — plain `=` on a `#T` result
                            // throws CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER,
                            // demoted to a note under CAJETA_OWNED_BIND=warn
                            // (§5.5).
                            if (rm->isReturnsOwnership()) {
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
                                // The audit's record is the SIZING channel
                                // (8.2.7's original 148-site count). Suppress
                                // it in warn mode: the check below emits a
                                // strictly richer record for the same site, so
                                // running both would double every declaration
                                // in the harvest — and a migration counted
                                // twice is a migration nobody can size.
                                if (ownership::ReturnTitleAudit::enabled()
                                        && !ownership::ownedBindWarns()) {
                                    // 8.1.4 — withhold the line inside a
                                    // monomorphization; it counts into the
                                    // synthesized instantiation buffer, not
                                    // the file.
                                    auto auditM = module->getCurrentMethod();
                                    const bool synth = auditM
                                        && (auditM->isMethodTemplateInstantiation()
                                            || (auditM->getParent()
                                                && auditM->getParent()->isInstantiation()));
                                    ownership::ReturnTitleAudit::ownedBind(
                                        calleeKey, in,
                                        synth ? 0 : mc->getSourceLine());
                                }
                                // 8.2.7 (spec §4.6) — THE CHECK. A `#T` result
                                // bound with plain `=` leaves the acquisition
                                // invisible at the line that performs it; `#=`
                                // is what puts it in the reader's view without
                                // opening the callee, which is the whole point
                                // of the return-side redesign (§2.8).
                                ownership::rejectPlainOwnedBind(
                                    calleeKey, field->getName(),
                                    module->getSourcePath(),
                                    (int) mc->getSourceLine(), in);
                            }
                            // A plain return is NOT statically a borrow: the
                            // return flag is RUNTIME state, so a plain-return
                            // wrapper rides an inner `#` call's title through
                            // (SignatureAbiTests.tailCallThroughPlainReturn-
                            // KeepsTitle; `Stream.fold<R>` via its callback's
                            // `#R`). Exactly the symmetry that makes `#p` on a
                            // plain FORMAL legal. So record provenance only
                            // when the callee's body PROVES an interior view —
                            // every return a `this.field` read. Anything else
                            // is allowed rather than guessed at (spec §7.2).
                            // 8.2.8 — a declared `^` return is the SIGNATURE
                            // form of the same fact the body scan infers, and
                            // it covers the shapes the scan cannot (`return
                            // this;`, delegation through another `^`). Without
                            // this arm, `Cell v = w.borrowed(); return v;`
                            // inside a `#`-declared method recorded no origin,
                            // §4.5 found nothing to object to, and the caller
                            // armed a drop on the receiver's interior — the
                            // one-hop launder of exactly the UAF `^` exists to
                            // close.
                            if (!rm->isReturnsOwnership()
                                    && (rm->isReturnsView()
                                        || (!rm->returnsStackValue()
                                            && rm->returnsInteriorView()))) {
                                auto rt = dynamic_pointer_cast<CajetaClass>(
                                    rm->getReturnType());
                                // Only title-bearing results can be wrongly
                                // surrendered; primitives never are.
                                if (rt && !rt->isValueType()
                                        && !rt->isSharedCapableValue()) {
                                    const string origin =
                                        mc->getMethodCallName() + "()";
                                    const string& declName =
                                        declarator->getIdentifier();
                                    // Record on BOTH identities: the field
                                    // object built here, and the scope (which
                                    // walks to the declaring frame). The `#x`
                                    // reader resolves its own field through
                                    // the scope, and the two are not always
                                    // the same object.
                                    field->setCallBorrowOrigin(origin);
                                    if (auto sc =
                                            module->getScopeStack().peek()) {
                                        sc->recordCallBorrow(declName, origin);
                                        if (FieldPtr sf =
                                                sc->getField(declName)) {
                                            sf->setCallBorrowOrigin(origin);
                                        }
                                    }
                                }
                            }
                        }
                        if (mc->getMethodCallName() == "__cajeta_inject") {
                            initIsBorrow = true;
                        } else {
                            // Resolve the called method at decl time so a
                            // non-`#` (borrow) return doesn't register a
                            // drop entry on the receiving local — that
                            // would double-free a borrowed reference at
                            // scope exit (e.g. `Stream<T> n =
                            // cur.unwrap();` where unwrap returns a plain
                            // Stream<T> field of cur — the field's owner
                            // already tracks the drop).
                            //
                            // Walk mc's first child (the receiver, when
                            // present) to find the target class. A bare
                            // method call (no receiver) is on `this`;
                            // get it from the current class on the
                            // structure stack.
                            auto mcKids = mc->getChildren();
                            CajetaClassPtr targetCls;
                            if (!mcKids.empty()) {
                                auto recvExpr = dynamic_pointer_cast<Expression>(mcKids[0]);
                                if (recvExpr) {
                                    if (!recvExpr->getResolvedType()) {
                                        recvExpr->resolveTypes(module);
                                    }
                                    targetCls = dynamic_pointer_cast<CajetaClass>(
                                        recvExpr->getResolvedType());
                                }
                                // Static call `ClassName.factory(...)`: the
                                // receiver is a TYPE NAME, not a value, so it has
                                // no resolved value-type above. Resolve the class
                                // from the identifier (same pattern as
                                // MethodCallExpression's static-call handling) so a
                                // static value-return (e.g. Instant.ofEpochSecond)
                                // is stack-classified rather than falling to the
                                // virtual-drop branch. (codegen-perf-levers 1.2.c)
                                if (!targetCls) {
                                    if (auto recvId = dynamic_pointer_cast<
                                            IdentifierExpression>(mcKids[0])) {
                                        targetCls = dynamic_pointer_cast<CajetaClass>(
                                            CajetaType::of(recvId->getTextValue()));
                                    }
                                }
                            } else if (!module->getStructureStack().empty()) {
                                targetCls = dynamic_pointer_cast<CajetaClass>(
                                    module->getStructureStack().back());
                            }
                            if (targetCls) {
                                vector<ParameterEntry> mcEntries;
                                bool floatingAll = true;
                                // This is a best-effort pre-analysis of the
                                // callee's return-ownership to pick the local's
                                // drop variant. If any argument type can't be
                                // resolved here (e.g. a function/lambda-typed
                                // arg whose identifier resolves to a null type),
                                // skip the analysis rather than feed a null type
                                // into resolveMethod -> buildGeneric ->
                                // toGeneric (a null-deref crash). The real call
                                // codegen below handles the resolution; falling
                                // back to the default drop is sound.
                                bool anyUnresolved = false;
                                for (auto& p : mc->getParameters()) {
                                    if (!p.expression->getResolvedType()) {
                                        p.expression->resolveTypes(module);
                                    }
                                    CajetaTypePtr pType =
                                        p.expression->getResolvedType();
                                    if (!pType) { anyUnresolved = true; break; }
                                    if (p.label.empty()) floatingAll = false;
                                    mcEntries.push_back(
                                        ParameterEntry(pType, p.label, nullptr));
                                }
                                string mcName = mc->getMethodCallName();
                                // Skip resolveMethod when an arg type was
                                // unresolved (a null arg type would crash
                                // buildGeneric->toGeneric). `resolved` stays
                                // null and the fn-typed-local fallback below
                                // (M5b) handles it.
                                MethodPtr resolved = anyUnresolved ? nullptr
                                    : targetCls->resolveMethod(
                                        mcName, mcEntries,
                                        /*isConstructor=*/false, floatingAll);
                                if (resolved && !resolved->isReturnsOwnership()) {
                                    // 5.2.3 — a plain class-pointer return is
                                    // NOT statically a borrow any more: the
                                    // callee's flag decides. Keep initIsBorrow
                                    // so the static owner paths stay off, and
                                    // arm a flag-fed entry in the drop wiring.
                                    if (resolved->returnsClassPointer()) {
                                        initIsFlaggedCall = true;
                                    }
                                    if (resolved->returnsStackValue()) {
                                        // Value-return (sret + NRVO): the callee
                                        // constructed a stack instance into the
                                        // caller's sret slot, so this local owns
                                        // its fields and needs the stack-drop
                                        // variant (drops owned fields, does NOT
                                        // free the alloca). Mirrors the path a
                                        // direct `stack X(...)` initializer takes.
                                        // KNOWN LIMITATION: reassigning the local
                                        // in a loop (`o = ch.receive()` repeatedly)
                                        // doesn't fire pre-overwrite drops, so a
                                        // value type that owns heap fields leaks
                                        // one set of fields per iteration. See
                                        // docs/specification/lang/ValueReturns.md (M5).
                                        initIsStackAlloc = true;
                                    } else {
                                        // Non-# return — the local is a borrow
                                        // of whatever the callee returned.
                                        initIsBorrow = true;
                                    }
                                }
                                // M5(b) — fn-typed MCE through a function-
                                // typed local or field: when the fn-type
                                // uses sret, the local takes ownership of
                                // the caller-allocated sret slot's value
                                // (stack-drop variant), same as a direct
                                // sret-method call above.
                                if (!resolved && !initIsStackAlloc && !initIsBorrow) {
                                    CajetaFunctionTypePtr fnTy;
                                    if (mcKids.empty()) {
                                        auto scope = module->getScopeStack().peek();
                                        FieldPtr fld = scope
                                            ? scope->getField(mcName) : nullptr;
                                        if (fld) {
                                            fnTy = dynamic_pointer_cast<CajetaFunctionType>(
                                                fld->getType());
                                        }
                                    }
                                    if (!fnTy && targetCls) {
                                        auto& props = targetCls->getProperties();
                                        auto pit = props.find(mcName);
                                        if (pit != props.end()) {
                                            fnTy = dynamic_pointer_cast<CajetaFunctionType>(
                                                pit->second->getType());
                                        }
                                    }
                                    if (fnTy && fnTy->usesSret()) {
                                        initIsStackAlloc = true;
                                    } else if (fnTy) {
                                        // 7.2.5 — non-sret closure member
                                        // call (`c.supplier()`): the
                                        // synthesized callee sets the
                                        // paired return flag; arm a
                                        // flag-fed entry for class-ptr
                                        // returns.
                                        if (dynamic_pointer_cast<CajetaClass>(
                                                fnTy->getReturnType())) {
                                            initIsBorrow = true;
                                            initIsFlaggedCall = true;
                                        }
                                    }
                                }
                            }
                        }
                    } else if (dynamic_pointer_cast<DotExpression>(children[0])
                            || dynamic_pointer_cast<ArrayIndexExpression>(children[0])) {
                        // The RHS is a field read or an array element
                        // read. If the value type is class-like (and
                        // not a struct), the local is a pointer to
                        // an object owned by the receiver (for a
                        // field) or by the array (for an element).
                        // Treat the local as a borrow — registering
                        // a drop entry would double-free at scope
                        // exit since the owner still tracks the
                        // instance.
                        if (rhsExpr) {
                            if (!rhsExpr->getResolvedType()) {
                                rhsExpr->resolveTypes(module);
                            }
                            auto rhsClass = dynamic_pointer_cast<CajetaClass>(rhsExpr->getResolvedType());
                            bool rhsIsStruct = dynamic_pointer_cast<CajetaView>(rhsExpr->getResolvedType()) != nullptr;
                            if (rhsClass && !rhsIsStruct) {
                                initIsBorrow = true;
                            }
                        }
                    } else if (auto rhsId = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                        // The RHS is another named binding (local var
                        // or method parameter). Two cases:
                        //
                        //   - Class-like, non-struct: the new local is
                        //     a second pointer to the same heap object;
                        //     the original owner is responsible for the
                        //     drop. Mark as borrow so the drop chain
                        //     doesn't double-free.
                        //
                        //   - Struct: the new local aliases the source
                        //     struct's body alloca. With both locals
                        //     registering independent drop entries (the
                        //     pre-fix behavior) the struct drop fn ran
                        //     twice on the same body — double-freeing
                        //     any owned class-ref fields. Fix: treat
                        //     `Foo b = a;` as a MOVE — suppress b's
                        //     drop registration AND mark `a` as moved.
                        //     The source local's drop entry stays
                        //     active (since `a` keeps the body
                        //     pointer); reads of `a` after this point
                        //     trip CAJETA_ERROR_MOVE_OF_BORROW.
                        //
                        // This complements the field-read and array-
                        // element cases above; together they cover the
                        // three common "alias an existing heap object"
                        // shapes.
                        if (rhsExpr) {
                            if (!rhsExpr->getResolvedType()) {
                                rhsExpr->resolveTypes(module);
                            }
                            auto rhsClass = dynamic_pointer_cast<CajetaClass>(rhsExpr->getResolvedType());
                            if (rhsClass) {
                                initIsBorrow = true;
                            }
                        }
                    }
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
                bool aliasShaped = false;
                if (auto escVi = dynamic_pointer_cast<VariableInitializer>(
                        initializer)) {
                    auto& escKids = escVi->getChildren();
                    if (!escKids.empty()) {
                        aliasShaped =
                            dynamic_pointer_cast<IdentifierExpression>(
                                escKids[0]) != nullptr
                            || dynamic_pointer_cast<DotExpression>(
                                escKids[0]) != nullptr
                            || dynamic_pointer_cast<ArrayIndexExpression>(
                                escKids[0]) != nullptr;
                    }
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
            if (isArray && !initIsBorrow && !arenaEligible) {
                // title-stores §3.2 — bit-capable-element arrays use ONE
                // walk+free entry so a move-out disarms both behaviors.
                shared_ptr<CajetaArray> arrT0 =
                    dynamic_pointer_cast<CajetaArray>(type);
                bool lvdElemTitled = arrT0 && !arrT0->isInlineArray()
                    && CajetaClass::arrayElementCarriesSlotBits(
                           arrT0->getElementType());
                bool lvdMemberBits = arrT0 && !arrT0->isInlineArray()
                    && CajetaClass::arrayElementCarriesMemberBits(
                           arrT0->getElementType());
                if (lvdElemTitled) {
                    const llvm::DataLayout& ldl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Function* wf = getOrCreateTailDropFree(module,
                        ldl.getTypeAllocSize(arrT0->getLlvmType()),
                        arrT0->elementStrideBytes(ldl,
                            module->getLlvmContext()));
                    emitDropEntryForFn(module, field, wf, getSourceLine());
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
                        emitDropEntryForFn(module, field, wf, getSourceLine());
                    } else {
                        emitDropEntryFor(module, field,
                            "__cajeta_free_array", getSourceLine());
                    }
                } else {
                    emitDropEntryFor(module, field, "__cajeta_free_array",
                        getSourceLine());
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
                            getSourceLine());
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

                        // Owned-string detection. Only applies to the
                        // LEGACY primitive String shape (i8* malloc'd
                        // buffer) — the class String follows the never-
                        // drop rule (docs/specification/lang/String.md
                        // § Memory model) and registering a drop here
                        // would double-free. Gate on ptrLike: class-
                        // typed locals (typeIsClass branch) skip this
                        // block entirely and fall through to borrow
                        // recording only.
                        bool producesOwnedString = false;
                        if (ptrLike && !typeIsClass) {
                        if (auto mc = dynamic_pointer_cast<MethodCallExpression>(child)) {
                            const std::string& name = mc->getMethodCallName();
                            if (name == "concat" || name == "substring"
                                    || name == "toUpperCase" || name == "toLowerCase"
                                    || name == "trim" || name == "replace") {
                                auto& mcChildren = mc->getChildren();
                                if (!mcChildren.empty()) {
                                    auto recv = dynamic_pointer_cast<Expression>(mcChildren[0]);
                                    if (recv) {
                                        if (!recv->getResolvedType()) {
                                            recv->resolveTypes(module);
                                        }
                                        auto rt = recv->getResolvedType();
                                        if (rt && rt->getQName()
                                                && rt->getQName()->getTypeName() == "String") {
                                            producesOwnedString = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (!producesOwnedString) {
                            if (auto bo = dynamic_pointer_cast<BinaryOpExpression>(child)) {
                                if (bo->getBinaryOp() == BINARY_OP_ADD) {
                                    if (!bo->getResolvedType()) {
                                        bo->resolveTypes(module);
                                    }
                                    auto rt = bo->getResolvedType();
                                    if (rt && rt->getQName()
                                            && rt->getQName()->getTypeName() == "String"
                                            && rt->getLlvmType()
                                            && rt->getLlvmType()->isPointerTy()) {
                                        producesOwnedString = true;
                                    }
                                }
                            }
                        }
                        } // end: if (ptrLike && !typeIsClass)

                        if (producesOwnedString) {
                            emitDropEntryFor(module, field, "__cajeta_free", getSourceLine());
                        } else {
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
                bool closureIsBorrow = false;
                if (auto varInit =
                        dynamic_pointer_cast<VariableInitializer>(initializer)) {
                    auto& kids = varInit->getChildren();
                    if (!kids.empty()) {
                        auto rhs = kids[0];
                        // A bare identifier or field/dot read aliases an
                        // existing closure — borrow. `#x` parses as a
                        // MoveExpression (not an IdentifierExpression), so a
                        // move still OWNS and keeps its drop entry.
                        if (dynamic_pointer_cast<IdentifierExpression>(rhs)
                                || dynamic_pointer_cast<DotExpression>(rhs)) {
                            closureIsBorrow = true;
                        }
                    }
                }
                if (!closureIsBorrow) {
                    emitDropEntryFor(module, field, "__cajeta_closure_drop", getSourceLine());
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
            if (isCajetaString && !isArray && !isStructType
                    && !initIsBorrow && !initIsStackAlloc && initializer
                    && !arenaEligible) {
                emitDropEntryFor(module, field, "__cajeta_string_drop", getSourceLine());
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
            if (klass && !isArray && !isStructType && !klass->isInterface()
                    && !initIsBorrow && initializer && !isCajetaString
                    && !klass->isValueType()) {
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
                        "__cajeta_class_virtual_drop", getSourceLine());
                } else {
                    // Custom-layout classes (CajetaTask<T> — no vtable
                    // pointer at slot 0). The virtual dispatcher can't
                    // safely read from instance[0], so fall back to
                    // static dispatch. These types are monomorphic by
                    // construction (no user subclassing), so the static
                    // per-class drop fn is both correct and sufficient.
                    if (llvm::Function* dropFn =
                            klass->getOrCreateDropFunction()) {
                        emitDropEntryForFn(module, field, dropFn, getSourceLine());
                    }
                }
            }

            // Stack-resident class instances flow through the regular
            // stack-drop path via klass->getOrCreateStackDropFunction()
            // wired earlier in this method.

            // 5.2.3 — runtime-owner local from a call result. Drops iff the
            // callee surrendered the title (return-flag TLS, captured at the
            // call site above). Same skip list as the formal entries (5.2.2):
            // shared-capable values, interfaces, views, value types.
            if (initIsFlaggedCall && callResultFlag && klass && !isArray
                    && !isStructType && !isCajetaString
                    && !klass->isInterface() && !klass->isValueType()
                    && !klass->isSharedCapableValue()
                    && klass->hasVtablePointerAtSlotZero()
                    && !field->getDropEntry()) {
                klass->patchVirtualTableDropFn();
                emitFlaggedDropEntryFor(module, field,
                    "__cajeta_class_virtual_drop", callResultFlag,
                    getSourceLine());
                field->setRuntimeConditionalOwner(true);
            }

            // 7.2.5 — `T x = #src` where src is a RUNTIME owner (formal /
            // flagged local): forward src's stashed flag onto x's entry.
            // The unconditional owner push above armed it statically, which
            // hands a LENT source back as owned (double free at the next
            // hop). MoveExpression stashed the flag before deactivation.
            if (field->getDropEntry()) {
                if (auto varInit = dynamic_pointer_cast<VariableInitializer>(
                        initializer)) {
                    auto& mvKids = varInit->getChildren();
                    if (!mvKids.empty()) {
                        if (auto mvInit = dynamic_pointer_cast<MoveExpression>(
                                mvKids[0])) {
                            if (llvm::Value* rtf = mvInit->getRuntimeTitleFlag()) {
                                if (llvm::Function* sf = module->getRuntimeFunction(
                                        "__cajeta_drop_set_flag")) {
                                    module->getBuilder()->CreateCall(
                                        sf, {field->getDropEntry(), rtf});
                                    field->setRuntimeConditionalOwner(true);
                                }
                            }
                        }
                    }
                }
            }

            // Interface local drop entry. Pushes a drop entry
            // pointing at __cajeta_iface_drop, the kind-tag dispatcher.
            // The helper reads the fat pointer's kind word and either
            // invokes the per-(class, iface) vtable's drop slot
            // (OWNED_CLASS) or no-ops (BORROWED_*). Cheap to push for
            // every interface local because the BORROWED branches are
            // bare return statements.
            if (klass && klass->isInterface()) {
                emitDropEntryFor(module, field, "__cajeta_iface_drop", getSourceLine());
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