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
    // Byte sizes of the runtime's DropEntry structs: 32 release (obj, drop_fn,
    // prev, active, padding), 40 in debug, which appends alloc_line and
    // alloc_file. CompilerFlags::sourceTags picks between them.
    static constexpr unsigned DROP_ENTRY_BYTES = 32;
    static constexpr unsigned DROP_ENTRY_BYTES_DEBUG = 40;

    struct DropPushChoice {
        llvm::Function* pushFn;
        unsigned entryBytes;
        bool debug;
    };
    // Entry size and push helper for the active CompilerFlags. The sourceTags
    // (debug) helper takes 5 args — entry, obj, drop_fn, alloc_file, alloc_line
    // — where the release one takes 3.
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

    // Script units §4 — promote a top-level owner to the runtime session
    // registry (same obj and drop fn an entry would carry) and emit NO frame
    // entry, so it survives the entry's return. True when promoted.
    static bool maybeEmitSessionBind(CajetaModulePtr module, FieldPtr field,
                                     llvm::Value* dropFn) {
        if (!module->isScriptUnit()) return false;
        if (!module->isScriptBindingName(field->getName())) return false;
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

    // The BORROW half of session binding: a top-level reference the unit does
    // not own (a literal, an alias, a borrow-returning call) reaches no drop
    // entry, so register it here with a NULL drop_fn — drop_all must not free.
    static void maybeEmitSessionBindBorrow(CajetaModulePtr module,
                                           FieldPtr field, CajetaTypePtr type) {
        if (!module->isScriptUnit() || !field || !type) return;
        if (field->isSessionBound()) return;
        if (type->getTypeFlags() & PRIMITIVE_FLAG) return;
        if (!module->isScriptBindingName(field->getName())) return;
        if (!module->isScriptEntryTopLevel()) return;
        // Pointer-shaped slots only: reading an inline aggregate slot as a
        // reference would hand the registry the address of a dead frame.
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

    // The PRIMITIVE half of session binding: a primitive reaches no drop entry,
    // so copy its slot into a session-owned box. Called once per top-level
    // declaration, after the initializer has stored.
    static void maybeEmitSessionBindValue(CajetaModulePtr module,
                                          FieldPtr field, CajetaTypePtr type) {
        if (!module->isScriptUnit() || !field || !type) return;
        if (!(type->getTypeFlags() & PRIMITIVE_FLAG)) return;
        if (!module->isScriptBindingName(field->getName())) return;
        if (!module->isScriptEntryTopLevel()) return;
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
        auto& dl = module->getLlvmModule()->getDataLayout();
        uint64_t size = dl.getTypeStoreSize(slot->getAllocatedType());
        if (size == 0) return;
        auto& ctx = *module->getLlvmContext();
        llvm::Value* nameStr = builder->CreateGlobalString(field->getName());
        builder->CreateCall(bindFn, {nameStr, slot,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), size)});
        // The assignment path keys its session re-bind on this flag; without it
        // a top-level `k += 2` skips the re-box and a later cell reads a stale
        // one.
        field->setSessionBound(true);
    }

    // Arm an already-pushed entry from a runtime title `flag` (0 = borrowed, so
    // the lender keeps its single drop). A null flag, or a constant 1 the push
    // already recorded, needs no call at all.
    static void armEntryFlag(CajetaModulePtr& module, FieldPtr& field,
                             llvm::Value* entryPtr, llvm::Value* flag) {
        if (!flag) return;
        if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(flag)) {
            if (k->isOne()) return;
        }
        if (llvm::Function* setFlagFn = module->getRuntimeFunction(
                "__cajeta_drop_set_flag")) {
            module->getBuilder()->CreateCall(setFlagFn, {entryPtr, flag});
            field->setRuntimeConditionalOwner(true);
        }
    }

    // Drop-chain wiring for an owner local: allocate the entry blob in the
    // function's entry block, push it once the owner is materialized, and record
    // it on the field and the method so scope exit emits the matching pop.
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

        // The chain threads pointers through this blob, so allocate it in the
        // entry block: its address must stay stable for the whole function.
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* entryPtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, push.entryBytes));

        llvm::Value* ownerPtr = builder->CreateLoad(ptrTy, field->getOrCreateAllocation());
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

    // Synthesize, once per (hs, es, arrKind), the one-arg drop fn that walks an
    // array's element titles then frees the buffer: header size `hs` and stride
    // `es` cannot ride that ABI. arrKind -1 = class elements, else array kind.
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

    void LocalVariableDeclaration::resolveTypes(CajetaModulePtr module) {
        if (!module || !module->isResolutionOnly()) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        for (auto& declarator : variableDeclarators) {
            if (!declarator) continue;
            // Initializer does NOT derive from Expression: resolve the node
            // itself, or a cast to Expression silently skips every initializer.
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

    // Push the element-walk entry for an owning element-tracked array local,
    // right after the storage entry so LIFO runs the walk BEFORE the buffer is
    // freed. obj = a stack sidecar shared with the element-store helpers.
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

    // Drop entry for a runtime-owner local, armed from the call's return flag
    // rather than a compile-time fact. `flag` must have been read immediately
    // after the call: any intervening cajeta call overwrites that thread-local.
    static void emitFlaggedDropEntryFor(CajetaModulePtr module, FieldPtr field,
                                         const std::string& dropFnName,
                                         llvm::Value* flag,
                                         int allocLine = 0) {
        if (!flag) return;
        emitDropEntryFor(module, field, dropFnName, allocLine, flag);
    }

    // Drop entry for a shared-capable VALUE local: obj is the stack slot ITSELF
    // (the alloca is the storage, not a pointer to load) and the drop fn is the
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

    // Drop entry taking an already-resolved drop function: a class's synthesized
    // wrapper is per-class, so there is no global symbol for getRuntimeFunction
    // to look up.
    static void emitDropEntryForFn(CajetaModulePtr module, FieldPtr field,
                                    llvm::Function* dropFn,
                                    int allocLine = 0,
                                    llvm::Value* flag = nullptr) {
        DropPushChoice push = pickDropPush(module);
        if (!push.pushFn || !dropFn) return;
        // The drop fn may live in another llvm::Module (a stdlib class), so
        // substitute a module-local extern decl for the merge step to resolve.
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

    // Collect every sub-node of `node` into `out`: `children` plus the subtrees
    // statements keep in private members and the call/ctor argument vectors,
    // which the default AbstractSyntaxNode walk does not reach.
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

    // True when `name` appears anywhere under `node`. Name-based and
    // conservative: no scope or shadowing analysis.
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
    // Does any assignment to `name` under `node` carry an OWNED-shaped RHS?
    // Shape-only and conservative: a call result or a conditional MAY carry a
    // title. A false positive costs one inactive entry; a miss leaks.
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
    // True when a use of `name` carries the value OUT of this scope: under a
    // return, a `#`-move, a lambda capture, or a `#`-transferred call/ctor
    // argument. Plain borrow arguments and field stores do not.
    static bool nameEscapesScope(const AbstractSyntaxNodePtr& node,
                                 const std::string& name) {
        if (!node) return false;
        bool blocking = dynamic_pointer_cast<ReturnStatement>(node)
            || isMoveKind(node)
            || dynamic_pointer_cast<LambdaExpression>(node);
        if (blocking) return subtreeUsesName(node, name);
        // A `#name` argument lands as MethodCallParameter.callerTransferred over
        // a plain identifier, not as a MoveExpression node.
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

    // Emit storage and the initializer for every declarator, then the ownership
    // wiring its title shape selects: drop entries, borrow records, session
    // bindings and the debugger's local registration. Always returns null.
    llvm::Value* LocalVariableDeclaration::generateCode(CajetaModulePtr module) {

        // Only primitives and @ValueType PODs (by-value like primitives) get an
        // inline-value alloca; arrays and class instances hold a pointer.
        bool isArray = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool wantsInlineSlot = type->hasValueSemantics() && !isArray;
        for (auto& declarator: variableDeclarators) {
            InitializerPtr initializer = declarator->getInitializer();
            // An array literal has no type of its own, so push the declared
            // element type down before it generates.
            if (isArray) {
                if (auto arrInit = dynamic_pointer_cast<ArrayInitializer>(initializer)) {
                    if (auto arrType = dynamic_pointer_cast<CajetaArray>(type)) {
                        arrInit->setElementType(arrType->getElementType());
                    }
                } else if (auto varInit =
                        dynamic_pointer_cast<VariableInitializer>(initializer)) {
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
            // A bare `[...]` against a class target rewrites to the from-array
            // constructor `heap Target([...])`, before the initializer emits.
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
                                if (dynamic_pointer_cast<CajetaClass>(type)) {
                                    agg->setExpectedType(type);
                                }
                            } else if (auto mapLit = dynamic_pointer_cast<
                                    MapLiteralExpression>(expr)) {
                                if (dynamic_pointer_cast<CajetaClass>(type)) {
                                    mapLit->setExpectedType(type);
                                }
                            }
                        }
                    }
                }
            }
            // A lambda or method-reference RHS takes the declared function type
            // as its expected type; its body cannot always infer one.
            if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                auto& children = varInit->getChildren();
                if (!children.empty()) {
                    if (auto lambda = dynamic_pointer_cast<LambdaExpression>(children[0])) {
                        lambda->setExpectedType(type);
                    } else if (auto mref = dynamic_pointer_cast<MethodReferenceExpression>(children[0])) {
                        mref->setExpectedType(type);
                    }
                }
            }
            // Local-borrow downgrade: a String local from substring/trim whose
            // name never escapes this method is rewritten to the *View twin
            // (the zero-rc borrow slice) before the initializer generates.
            {
                auto declClass = dynamic_pointer_cast<CajetaClass>(type);
                bool declIsString = declClass && declClass->getQName()
                    && declClass->getQName()->getTypeName() == "String"
                    && declClass->getQName()->getPackageName() == "cajeta.lang";
                if (declIsString && initializer) {
                    if (auto varInit = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                        auto& kids = varInit->getChildren();
                        // `#=` wraps the call in a MoveExpression: unwrap before
                        // matching, or the rewrite never fires.
                        auto init0 = kids.empty() ? nullptr
                            : dynamic_pointer_cast<Expression>(kids[0]);
                        if (isMoveKind(init0)) init0 = moveInner(init0);
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
            // `T x #= src[i]` is MODE-CARRYING: the local arms from the slot's
            // actual bit rather than demanding a title, so a borrowed slot binds
            // instead of panicking on a missing one.
            if (auto fwdVi = dynamic_pointer_cast<VariableInitializer>(
                    declarator->getInitializer())) {
                auto& fwdKids = fwdVi->getChildren();
                if (!fwdKids.empty()) {
                    if (auto outerMv = isMoveKind(fwdKids[0])
                            ? std::static_pointer_cast<MoveExpression>(fwdKids[0]) : nullptr) {
                        if (!outerMv->getChildren().empty()) {
                            // `#= #src[i]` nests two moves: flag the one that
                            // directly wraps the slot, which emits the take.
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
            // A redeclaration is the cross-unit rebind (last write wins), so
            // clear a moved-out mark carried over from an earlier unit.
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

            // ONE classification of the initializer decides the binding, and it
            // runs HERE: a call's TLS bit and a move's stashed flag are readable
            // only right after the initializer's IR.
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
            const bool initIsBorrow =
                initVerdict.answer == ownership::TitleAnswer::Borrow
                || initVerdict.answer == ownership::TitleAnswer::Runtime;
            const bool initIsFlaggedCall =
                initVerdict.answer == ownership::TitleAnswer::Runtime;
            const bool initIsStackAlloc =
                initVerdict.answer == ownership::TitleAnswer::StackBound;
            llvm::Value* initTitleFlag = nullptr;
            if (initIsFlaggedCall) {
                auto flagCls = dynamic_pointer_cast<CajetaClass>(type);
                if (flagCls && !dynamic_pointer_cast<CajetaView>(type)
                        && !flagCls->isValueType()
                        && !flagCls->isSharedCapableValue()) {
                    initTitleFlag = ownership::titleFlag(initShape, module);
                }
            }

            // A shared-capable value local retains only when initialized from an
            // LVALUE (that copy duplicated stakes; an rvalue carries its own),
            // and always registers a release entry against its own slot.
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

            // The debug-frame registration happens at the END of this method
            // (emitDbgLocal): its ownership facet needs the final drop entry.

            // Polymorphic-MI upcast: when the LHS class sits at a non-zero
            // sub-object offset in the RHS, shift the stored pointer so dispatch
            // lands on the secondary vtable and the LHS-shaped field offsets.
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

            // Definite assignment: a local declared without an initializer
            // enters the scope's not-yet-assigned set. Views and interface
            // locals are exempt — both get a body pointer wired regardless.
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

            // An interface local's slot points at a 24-byte fat-pointer body:
            // with no initializer, allocate and zero one; from a class RHS,
            // build one (data, vtable, kind); from an interface RHS, alias it.
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
                            // HeapField already stored the RHS (a class pointer
                            // or a struct body ptr); either becomes `data`.
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

                            // The kind word carries the initializer's title,
                            // selected on the flag when it is runtime-decided.
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
                        // An interface RHS needs no fix-up: the slot already
                        // holds its body ptr, and the local borrows it.
                    }
                }
            }

            // A function-typed local inherits its RHS's borrow-capture flag —
            // copied after the RHS's capture analysis has run — so a later
            // `return fnLocal` reports the dangling borrow before LLVM verify.
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
                    // A spawn pushes its own entry so a bare `spawn foo();` is
                    // still freed; bound to a local, the local's entry below is
                    // the owner, so mark the spawn's transient entry inactive.
                    // `#=` wraps the spawn in a MoveExpression: unwrap it, or
                    // both entries stay armed and the Task is freed twice.
                    auto spawnInit = dynamic_pointer_cast<Expression>(children[0]);
                    if (isMoveKind(spawnInit)) spawnInit = moveInner(spawnInit);
                    if (auto spawn = dynamic_pointer_cast<SpawnExpression>(spawnInit)) {
                        if (llvm::Value* spawnEntry = spawn->getDropEntry()) {
                            if (llvm::Function* markInactive = module->getRuntimeFunction(
                                    "__cajeta_drop_mark_inactive")) {
                                module->getBuilder()->CreateCall(
                                    markInactive, {spawnEntry});
                            }
                        }
                    }
                    // A view constructed over a local buffer records the field
                    // it aliases, so ReturnStatement can reject returning it.
                    // Recognized only for a receiverless one-identifier call.
                    if (auto mce = dynamic_pointer_cast<MethodCallExpression>(children[0])) {
                        bool isViewCtor = mce->getChildren().empty()
                            && mce->getParameters().size() == 1
                            && dynamic_pointer_cast<CajetaView>(
                                CajetaType::of(mce->getMethodCallName())) != nullptr;
                        if (isViewCtor) {
                            auto& mceParams = mce->getParameters();
                            auto argExpr = mceParams[0].expression;
                            // `View(#buf)` transfers, `View(buf)` borrows: the
                            // `#` arrives as callerTransferred or as a Move.
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

            // The side effects the shape does not carry: borrow provenance and
            // the owned-bind check.
            if (initShape.leaf) {
                switch (initShape.leaf->kind()) {
                    case ExprKind::Identifier: {
                        // `T v = p` off a plain formal borrows: record the origin
                        // by field identity, since names collide across methods.
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
                                field->setParamBorrowOrigin(
                                    srcF->getParamBorrowOrigin());
                            }
                        }
                        break;
                    }
                    case ExprKind::MethodCall: {
                        auto mc = static_pointer_cast<MethodCallExpression>(
                            initShape.leaf);
                        Method* rm = initShape.callee;
                        // A `#`-returning result bound with PLAIN `=` is
                        // rejected: `#=` keeps the acquisition in the reader's
                        // view.
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
                            if (ownership::ReturnTitleAudit::enabled()
                                    && !ownership::ownedBindWarns()) {
                                // A monomorphization's line is not a line of
                                // the file.
                                const bool synth = holder
                                    && (holder->isMethodTemplateInstantiation()
                                        || (holder->getParent()
                                            && holder->getParent()->isInstantiation()));
                                ownership::ReturnTitleAudit::ownedBind(
                                    calleeKey, in,
                                    synth ? 0 : mc->getSourceLine());
                            }
                            // A site inside a released dependency's internals
                            // is a note, never an error.
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
                        // The callee proved a borrow of its receiver's interior,
                        // so record the lending call: a later `#local` rejects.
                        if (rm && !rm->isReturnsOwnership()
                                && (rm->isReturnsView()
                                    || (!rm->returnsStackValue()
                                        && rm->returnsInteriorView()))) {
                            auto rt = dynamic_pointer_cast<CajetaClass>(
                                rm->getReturnType());
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

            // A session binding cannot HOLD a borrow: it outlives the unit's
            // frame while the lender may drop or rebind later. Only alias shapes
            // reject; literal borrows and flagged call results stay legal.
            if (initIsBorrow && !initIsFlaggedCall && module->isScriptUnit()
                && module->isScriptBindingName(field->getName())) {
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

            // Arena-eligible locals register NO drop entry: the scope-exit arena
            // reset reclaims them, and the escape pre-pass proved they stay in
            // the frame.
            bool arenaEligible = false;
            if (auto cm = module->getCurrentMethod()) {
                arenaEligible = cm->isArenaEligibleLocal(declarator->getIdentifier());
            }

            // An array local owns its heap header — register free_array unless
            // it is a borrow, whose upstream owner would then double-free. A
            // Runtime answer arms the same entries from the flag.
            llvm::Value* arrFlag = initIsBorrow ? initTitleFlag : nullptr;
            if (isArray && (!initIsBorrow || arrFlag) && !arenaEligible) {
                // Bit-capable elements take ONE walk+free entry, so a move-out
                // disarms both behaviors.
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
                // A local String[] owns what its stores TOOK: register the walk
                // that reclaims them. Class elements instead mark a slot bit per
                // store and release through the element vtable's drop.
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
                        // The walk rides the single walk+free entry registered
                        // above; only the element vtable's drop needs patching.
                        elemCls->patchVirtualTableDropFn();
                    } else if (elemIsString && !arrT->isInlineArray()) {
                        emitArrayElemDropEntry(module, field, arrT,
                            "__cajeta_string_array_owned_drop",
                            getSourceLine(), arrFlag);
                        // A literal's String slots belong to the array: mark
                        // them so the teardown walk frees them.
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

            // Gap 4 — record a live read-borrow for a local that ALIASES its
            // source (`String alias = p.name`, `Foo b = a`), so a later write
            // through that path rejects before clobbering the borrowed slot.
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

            // An owning view took the buffer from its source (whose entry the
            // move already deactivated): register one against the view's data
            // pointer, which the helper backs up 8 bytes to the array header.
            if (field->isOwningView()) {
                emitDropEntryFor(module, field, "__cajeta_view_drop_owned", getSourceLine());
            }

            // A function-typed local owns the closure record it points at, and a
            // non-capturing record carries drop_fn=null so the entry is safe
            // either way — but a local that merely ALIASES one must not drop.
            if (dynamic_pointer_cast<CajetaFunctionType>(type)) {
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

            // Class-instance locals register the class's synthesized drop
            // wrapper; arrays, structs and String take their own paths below.
            auto klass = dynamic_pointer_cast<CajetaClass>(type);
            bool isStructType = dynamic_pointer_cast<CajetaView>(type) != nullptr;
            bool isCajetaString = klass && klass->getQName()
                && klass->getQName()->getTypeName() == "String"
                && klass->getQName()->getPackageName() == "cajeta.lang";
            // An owned cajeta.lang.String local registers the mode-aware string
            // drop: it frees the byte buffer only for owned (mode 0) strings and
            // no-ops for view-mode literals and slices.
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
            // A BARE declaration registers a NULL-obj entry in its OWN frame so
            // a later move-assign — possibly in an inner block — can retarget it
            // in place; the chain is strict LIFO. Dropping the null no-ops.
            if (isCajetaString && !isArray && !isStructType && !initializer) {
                auto* b = module->getBuilder();
                auto& lctx2 = *module->getLlvmContext();
                b->CreateStore(
                    llvm::ConstantPointerNull::get(
                        llvm::PointerType::get(lctx2, 0)),
                    field->getOrCreateAllocation());
                emitDropEntryFor(module, field, "__cajeta_string_drop", getSourceLine());
            }
            // The same null-obj entry for a bare class-typed declaration.
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
            // @ValueType locals are inline Copy PODs — a push here would read
            // their first word as a vtable pointer — and a slot with no
            // initializer holds garbage the entry would capture. Both skip.
            llvm::Value* cFlag = initIsBorrow ? initTitleFlag : nullptr;
            if (klass && !isArray && !isStructType && !klass->isInterface()
                    && initializer && !isCajetaString && !klass->isValueType()
                    && (!initIsBorrow || (cFlag && !field->getDropEntry()))) {
                // A `stack` local takes the stack-drop variant, which walks
                // owned fields but never frees the body; a heap local goes
                // through the virtual dispatcher, so a derived local drops derived.
                if (initIsStackAlloc) {
                    // A session binding outlives the entry's frame, so `stack`
                    // cannot bind at top level.
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
                    // Storage lives on the construction, not the type, so this
                    // declaration is the only site that can record it.
                    field->setStackInstance(true);
                    // A trivial stack drop is skipped: registering and running
                    // an empty drop per scope dominates tight loops.
                    if (!klass->hasTrivialStackDrop()) {
                        if (llvm::Function* stackDropFn =
                                klass->getOrCreateStackDropFunction()) {
                            emitDropEntryForFn(module, field, stackDropFn, getSourceLine());
                        }
                    }
                } else if (klass->hasVtablePointerAtSlotZero()) {
                    // Patch vtable.drop_fn, then register the dispatcher, which
                    // reads it off the instance — a derived instance in a
                    // base-typed local drops as derived.
                    klass->patchVirtualTableDropFn();
                    emitDropEntryFor(module, field,
                        "__cajeta_class_virtual_drop", getSourceLine(), cFlag);
                } else {
                    // No vtable pointer at slot 0 (CajetaTask<T>): these types
                    // are monomorphic, so static dispatch is correct.
                    if (llvm::Function* dropFn =
                            klass->getOrCreateDropFunction()) {
                        emitDropEntryForFn(module, field, dropFn, getSourceLine(), cFlag);
                    }
                }
            }

            // A local holding a borrow may be assigned an OWNED value later, and
            // the entry that assignment re-arms must exist in THIS frame (strict
            // LIFO): register it now, INACTIVE, when an assignment is owned.
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

            // The interface entry points at the kind-tag dispatcher, which drops
            // through the per-(class, iface) vtable for OWNED and no-ops for
            // BORROWED — but a proven borrow gets no entry at all.
            if (klass && klass->isInterface()
                    && initVerdict.answer != ownership::TitleAnswer::Borrow) {
                emitDropEntryFor(module, field, "__cajeta_iface_drop",
                                 getSourceLine(),
                                 initIsFlaggedCall ? initTitleFlag : nullptr);
            }

            // Register the local in the debug frame (a no-op without
            // --debug-info). Emitted here, after all drop wiring, so the
            // ownership facet sees the final drop entry.
            if (type) {
                dbg::FieldFacetInputs facetIn;
                // The slot kind describes the SLOT (a class local is always a
                // pointer), so a `stack` creator wins for the alloc facet.
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

            // Last, so both read a fully-initialized slot.
            maybeEmitSessionBindValue(module, field, type);
            maybeEmitSessionBindBorrow(module, field, type);
        }

        return nullptr;
    }

} // code