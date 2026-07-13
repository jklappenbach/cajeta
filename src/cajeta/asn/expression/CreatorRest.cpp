//
// Created by James Klappenbach on 4/19/23.
//

#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/NewExpression.h"
#include "cajeta/error/Exception.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/method/Method.h"

#include <functional>

namespace cajeta {
    shared_ptr<CreatorRest> CreatorRest::fromContext(CajetaParser::CreatorContext* ctx, antlr4::Token* token) {
        if (ctx->classCreatorRest()) {
            return make_shared<ClassCreatorRest>(ctx->classCreatorRest(), token);
        } else {
            return make_shared<ArrayCreatorRest>(ctx->arrayCreatorRest(), token);
        }
    }

    // Emits `malloc(sizeof(struct))` (or, when stackAlloc is set, an entry-
    // block alloca) then dispatches to the matching constructor with the
    // user-supplied arguments. Returns the instance pointer either way.
    llvm::Value* ClassCreatorRest::generateCode(CajetaModulePtr module) {
        if (!targetType) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();
        llvm::Type* structTy = targetType->getLlvmType();
        const llvm::DataLayout& dataLayout = module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(llvmCtx),
            dataLayout.getTypeAllocSize(structTy));

        llvm::Value* instance;
        if (nrvoTarget) {
            // NRVO: build directly into the caller's sret slot — no separate
            // alloca/malloc and no copy. The memset + vtable init + ctor below
            // all run against the caller-owned slot.
            instance = nrvoTarget;
        } else if (stackAlloc) {
            // P2a — entry-block alloca for `stack MyClass(args)`. Hoist the
            // alloca to the function entry so it lives for the whole frame
            // (LLVM convention; allocas in arbitrary blocks are legal but
            // confuse mem2reg + can grow the stack frame across loop
            // iterations). Lifetime is the enclosing scope; the borrow
            // checker rejects escape (return / heap-field-store) per the
            // S10.3-generalized check.
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                parentFn->getEntryBlock().begin());
            instance = entryBuilder.CreateAlloca(structTy);
        } else {
            instance = MemoryManager::createMallocInstruction(
                module, allocSize, builder->GetInsertBlock());
        }

        // Zero-init + vtable install (primary slot-0, secondary sub-object
        // vtables, and the heap drop-fn patch). Factored into
        // CajetaClass::initInstanceLayout so synthesized construction sites
        // (throwing capture cast, tryAs, ...) share the exact same logic. The
        // S7.2 zero-init rationale + the Polymorphic-MI + Gap-1 drop-dispatch
        // details live there.
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            klass->initInstanceLayout(module, instance, structTy, stackAlloc);
        } else {
            // Non-class target (no vtable) — just zero the block. Unreached in
            // practice (ClassCreatorRest always builds a CajetaClass), kept to
            // preserve the prior unconditional memset behavior exactly.
            builder->CreateMemSet(instance,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvmCtx), 0),
                allocSize, llvm::MaybeAlign(8));
        }

        // Lambda-as-ctor-arg expectedType propagation. Mirror the
        // MethodCallExpression pattern so a bare-identifier lambda
        // passed to `heap T(...)` / `stack T(...)` / `new T(...)`
        // borrows its param + return types from the matching ctor's
        // formal. Without this, `heap Holder(seed, (acc, x) -> ...)`
        // fails type inference (no LHS signal, no propagator),
        // forcing every caller to spell typed params explicitly.
        //
        // Match the ctor by arity. Constructors are non-static, so the
        // formal param list includes `this` as element 0 — subtract 1.
        // Template-instantiation skip from MCE doesn't fire here (ctors
        // aren't method-templated under the current grammar — the doc
        // explicitly excludes constructors from method-level templates,
        // see docs/specification/lang/templates/MethodLevelTemplate.md § Constructors
        // and operators excluded).
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            bool anyLambda = false;
            for (auto& param : parameters) {
                if (std::dynamic_pointer_cast<LambdaExpression>(param.expression)
                        && !param.expression->getResolvedType()) {
                    anyLambda = true;
                    break;
                }
            }
            if (anyLambda) {
                std::string ctorName = targetType->getQName()->getTypeName();
                if (klass->getTemplateOrigin()) {
                    ctorName = klass->getTemplateOrigin()->getQName()->getTypeName();
                }
                MethodPtr candidate;
                int matches = 0;
                for (auto& mEntry : klass->getMethods()) {
                    auto& m = mEntry.second;
                    if (!m->isConstructor()) continue;
                    if (m->getName() != ctorName) continue;
                    int declared = (int) m->getParameterList().size() - 1;
                    if (declared != (int) parameters.size()) continue;
                    candidate = m;
                    ++matches;
                }
                if (candidate && matches == 1) {
                    auto paramList = candidate->getParameterList();
                    // paramList[0] is `this`; user-visible args start at 1.
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        size_t formalIdx = i + 1;
                        if (formalIdx >= paramList.size()) break;
                        if (auto lambda = std::dynamic_pointer_cast<LambdaExpression>(
                                parameters[i].expression)) {
                            if (!lambda->getResolvedType()
                                    && paramList[formalIdx]->getType()) {
                                lambda->setExpectedType(
                                    paramList[formalIdx]->getType());
                            }
                        }
                    }
                }
            }
        }

        // Resolve parameters and call the constructor. Prefer the expression's
        // resolvedType when available — `CajetaType::of(llvm::Value*)` can't
        // recover class-instance types (the LLVM value is just a `ptr` and
        // doesn't carry the user-class identity). Without this fallback,
        // passing a class instance as a constructor arg would null-deref
        // when `Method::buildCanonical` walks parameter types.
        vector<ParameterEntry> entries;
        for (auto& param : parameters) {
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            llvm::Value* value = param.expression->generateCode(module);
            // L-value-to-r-value coercion. Argument expressions can be
            // local allocas (IdentifierExpression), field GEPs
            // (DotExpression — covers `this.handle` etc.), or array
            // slots (ArrayIndexExpression) — all need a load before
            // the value flows into the constructor. Without this,
            // ctor params like `pointer handle` receive the slot
            // address instead of the value, which silently misroutes
            // every pthread-handle-style argument.
            auto astExpr = dynamic_pointer_cast<Expression>(param.expression);
            value = loadIfLValue(module, value, astExpr);
            CajetaTypePtr paramType = param.expression->getResolvedType();
            if (!paramType) paramType = CajetaType::of(value);
            entries.push_back(ParameterEntry(paramType, param.label, value));
        }
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            // Constructor name is the class's simple type name. For a
            // template instantiation (e.g. "Container<int32>") the
            // source-parsed ctor was named after the unparameterized
            // template ("Container") — fall through to the template
            // origin's name so we find the real ctor instead of looking
            // up "Container<int32>" and falling back to the empty
            // auto-default. Pairs with the Method ctor's same fallback
            // (Method.cpp's constructor-detection logic).
            string ctorName = targetType->getQName()->getTypeName();
            if (klass->getTemplateOrigin()) {
                ctorName = klass->getTemplateOrigin()->getQName()->getTypeName();
            }
            // # transfer at ctor-call site (#67 + Phase 1 of #68 — see
            // docs/specification/lang/OwnershipTransfer.md). Two passes:
            //
            //   1. Caller-side: any argument the caller wrote `#x` on
            //      gets its source local's drop deactivated. The caller's
            //      intent at the source line is authoritative — no need
            //      to inspect the callee. Required for synthesized
            //      ctors (e.g. view ctors) where resolveMethod misses.
            //
            //   2. Callee-side: any formal marked `#T` that didn't get
            //      handled by pass 1 (caller wrote plain `x`) gets the
            //      same deactivation. Phase 2 will tighten this with a
            //      (#T-formal, plain-x) compile error; Phase 1 stays
            //      additive.
            //
            // Primitives, literals, and locals without drop entries
            // naturally degrade to no-op (the inner gate fires only on
            // an IdentifierExpression arg whose Field has a drop entry).
            {
                auto deactivateIfClassLocal = [&](size_t argIdx) {
                    if (argIdx >= parameters.size()) return;
                    auto argExprBase = parameters[argIdx].expression;
                    auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                        argExprBase);
                    if (!idExpr) return;
                    auto scope = module->getScopeStack().peek();
                    if (!scope) return;
                    FieldPtr field = scope->getField(idExpr->getTextValue());
                    if (!field) return;
                    if (llvm::Value* entry = field->getDropEntry()) {
                        if (llvm::Function* mark = module->getRuntimeFunction(
                                "__cajeta_drop_mark_inactive")) {
                            builder->CreateCall(mark, {entry});
                        }
                    }
                };
                // Pass 1: caller-side. Phase 3a of #68 — if the `#x`
                // source is a borrowed class parameter, reject.
                for (size_t i = 0; i < parameters.size(); ++i) {
                    if (!parameters[i].callerTransferred) continue;
                    auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                        parameters[i].expression);
                    if (idExpr) {
                        if (auto scope = module->getScopeStack().peek()) {
                            FieldPtr field = scope->getField(idExpr->getTextValue());
                            auto pf = std::dynamic_pointer_cast<ParameterField>(field);
                            if (pf) {
                                auto formal = pf->getFormalParameter();
                                // 5.2.4 — RETIRED for class-typed formals: a
                                // plain formal is a RUNTIME owner (5.2.2), so
                                // `#formal` as a ctor arg forwards its actual
                                // flag (the ctor word, composed below). Entry-
                                // less formals of a WORDED method count too
                                // (String/primitive get no entry; their word
                                // bit is still the truth) — same rule as MCE.
                                auto crCm = module->getCurrentMethod();
                                bool runtimeOwner = field
                                    && (field->getDropEntry()
                                        || (crCm && crCm->getTransferWordArg()));
                                if (formal && !formal->isTransferred()
                                        && !runtimeOwner) {
                                    auto klassParam = std::dynamic_pointer_cast<CajetaClass>(
                                        field->getType());
                                    if (klassParam && !klassParam->isInterface()) {
                                        throw Exception(
                                            "cannot transfer borrowed parameter `"
                                                + idExpr->getTextValue() + "` via "
                                                "`#` — its formal is declared plain `T` "
                                                "(borrow), so this scope doesn't own the "
                                                "value. Fix: mark the formal `#"
                                                + idExpr->getTextValue() + "` to receive "
                                                "ownership at the outer call site, or "
                                                "restructure to not retain the borrow.",
                                            "CAJETA_ERROR_BORROW_PARAM_ESCAPES");
                                    }
                                }
                            } else if (field) {
                                // title-tracking §3.1.2-3 (plan 2.2.1-2.2.2) —
                                // ctor-arg linearity, mirroring the
                                // MethodCallExpression call-arg checks: a
                                // local ctor-arg `#x` needs a statically-
                                // active owner and marks the source moved.
                                const string& nm = idExpr->getTextValue();
                                auto argKlass = std::dynamic_pointer_cast<CajetaClass>(
                                    field->getType());
                                if (argKlass && !argKlass->isValueType()
                                        && !argKlass->isSharedCapableValue()
                                        && !argKlass->isInterface()) {
                                    if (scope->isMoved(nm)) {
                                        string note = scope->movedNoteOf(nm);
                                        throw Exception(
                                            "use of moved value: `#" + nm + "` — "
                                                "the value was already transferred"
                                                + (note.empty() ? "" : " (" + note + ")")
                                                + ". Fix: reassign a fresh value "
                                                  "before transferring again.",
                                            "CAJETA_ERROR_USE_AFTER_MOVE");
                                    }
                                    if (!field->getDropEntry()) {
                                        string owner = scope->borrowSourceOf(nm);
                                        if (!owner.empty()) {
                                            throw Exception(
                                                "cannot move out of a borrow: `"
                                                    + nm + "` does not own its "
                                                      "value; ownership belongs to `"
                                                    + owner + "`. Fix: move from the "
                                                      "owner, or store an owned value "
                                                      "first.",
                                                "CAJETA_ERROR_MOVE_OF_BORROW");
                                        }
                                    }
                                    scope->markMoved(nm,
                                        "transferred to a constructor at line "
                                            + std::to_string(getSourceLine()));
                                }
                            }
                        }
                    }
                    deactivateIfClassLocal(i);
                }
                // Pass 2: callee-side `#T` formal — Phase 2 of #68
                // contract check. If formal is `#T` and the caller
                // didn't acknowledge transfer (no `#x`, no MoveExpression,
                // no fresh ctor), throw CAJETA_ERROR_TRANSFER_REQUIRED.
                // Only fires on a genuine double-free hazard: an
                // IdentifierExpression of a class-typed local whose
                // Field carries an active drop entry. See
                // docs/specification/lang/OwnershipTransfer.md.
                MethodPtr xferTarget = klass->resolveMethod(
                    ctorName, entries, /*isConstructor=*/true,
                    /*floatingParams=*/false);
                if (xferTarget) {
                    auto formalParams = xferTarget->getParameterList();
                    int xferParamOffset = !formalParams.empty()
                        && formalParams.front()->getName() == "this" ? 1 : 0;
                    size_t fIdx = 0;
                    for (auto& fp : formalParams) {
                        if ((int) fIdx < xferParamOffset) { ++fIdx; continue; }
                        size_t argIdx = fIdx - xferParamOffset;
                        if (argIdx >= parameters.size()) break;
                        ++fIdx;
                        if (!fp->isTransferred()) continue;
                        // optional-borrow-ownership 2.2.3.a — a `#T` formal in a
                        // BORROW-mode instantiation (`Optional<T>`, not
                        // `Optional<#T>`) is not a transfer: the callee's drop body
                        // skips that field (see CajetaClass drop walk's
                        // originTypeParamIndex gate), so there is no double-free to
                        // guard against. Relax exactly when that walk will skip.
                        if (klass->isInstantiation() && fp->getType()) {
                            bool borrowModeFormal = false;
                            const auto& targs = klass->getTypeArguments();
                            auto fpQName = fp->getType()->getQName();
                            for (size_t k = 0; k < targs.size() && fpQName; ++k) {
                                if (klass->isTypeArgumentOwning(k)) continue;
                                if (!targs[k] || !targs[k]->getQName()) continue;
                                if (targs[k]->getQName()->toCanonical()
                                        == fpQName->toCanonical()) {
                                    borrowModeFormal = true;
                                    break;
                                }
                            }
                            if (borrowModeFormal) continue;
                        }
                        if (parameters[argIdx].callerTransferred) continue;
                        auto argExpr = parameters[argIdx].expression;
                        if (dynamic_pointer_cast<MoveExpression>(argExpr)) continue;
                        if (dynamic_pointer_cast<NewExpression>(argExpr)) continue;
                        auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                            argExpr);
                        if (!idExpr) continue;
                        auto scope = module->getScopeStack().peek();
                        if (!scope) continue;
                        FieldPtr field = scope->getField(idExpr->getTextValue());
                        if (!field || !field->getDropEntry()) continue;
                        throw Exception(
                            "constructor `" + ctorName + "` declares parameter `"
                                + fp->getName() + "` as `#T` (ownership transfer required); "
                                "write `#" + idExpr->getTextValue() + "` at the call site "
                                "to surrender ownership of the source local, or pass a "
                                "fresh `heap T(...)` / `stack T(...)` construction. "
                                "See docs/specification/lang/OwnershipTransfer.md.",
                            "CAJETA_ERROR_TRANSFER_REQUIRED");
                    }
                }
            }
            // title-tracking Unit 5: constructors ride the same trailing
            // transfer word as ordinary calls (ctors never emitted the
            // moveMask TLS, so the ABI word is their first per-call channel).
            // 5.2.4 — runtime-composed, same rule as MethodCallExpression: a
            // `#x` sourced from a runtime owner forwards the flag it held.
            auto* twBuilder = module->getBuilder();
            int64_t ctorTransferWord = 0;
            llvm::Value* ctorWordVal = nullptr;
            for (size_t twi = 0; twi < parameters.size(); ++twi) {
                if (!parameters[twi].callerTransferred) continue;
                llvm::Value* rf = nullptr;
                if (auto mv = std::dynamic_pointer_cast<MoveExpression>(
                        parameters[twi].expression)) {
                    rf = mv->getRuntimeTitleFlag();
                }
                if (!rf) {
                    ctorTransferWord |= ((int64_t) 1) << twi;
                    continue;
                }
                llvm::Value* bit = twBuilder->CreateShl(
                    twBuilder->CreateAnd(rf, twBuilder->getInt64(1)),
                    twBuilder->getInt64((uint64_t) twi));
                ctorWordVal = ctorWordVal
                    ? twBuilder->CreateOr(ctorWordVal, bit) : bit;
            }
            if (ctorWordVal) {
                if (ctorTransferWord != 0) {
                    ctorWordVal = twBuilder->CreateOr(ctorWordVal,
                        twBuilder->getInt64((uint64_t) ctorTransferWord));
                }
            } else {
                ctorWordVal = twBuilder->getInt64((uint64_t) ctorTransferWord);
            }
            klass->invokeMethod(ctorName, entries, /*isConstructor=*/true, instance,
                                /*callerModule=*/module,
                                /*forceDirectCall=*/false,
                                /*explicitMethodTypeArgs=*/{},
                                /*sretTarget=*/nullptr,
                                /*transferWord=*/ctorWordVal);
            // slices 9.4.1 — fresh temps consumed as CTOR arguments have no
            // drop entry; reclaim them here, at the only site that sees the
            // temp (mirrors the post-call block in MethodCallExpression::
            // generateCode). A fresh owned-String temp gets the guarded
            // string drop; a shared-capable VALUE call-result gets its
            // stakes released (the ctor's field store retained its own).
            // `#T` formals and `#x` call-site transfers took ownership —
            // skipped, exactly as at method call sites.
            {
                MethodPtr ctorTarget = klass->resolveMethod(
                    ctorName, entries, /*isConstructor=*/true,
                    /*floatingParams=*/false);
                llvm::Function* strDropFn = module->getRuntimeFunction(
                    "__cajeta_string_drop");
                if (ctorTarget) {
                    auto fpl = ctorTarget->getParameterList();
                    int off = !fpl.empty()
                        && fpl.front()->getName() == "this" ? 1 : 0;
                    for (size_t ai = 0; ai < parameters.size(); ++ai) {
                        size_t fi = ai + (size_t) off;
                        if (fi >= fpl.size()) break;
                        if (!fpl[fi] || fpl[fi]->isTransferred()) continue;
                        if (parameters[ai].callerTransferred) continue;
                        llvm::Value* tempV = entries[ai].value;
                        if (!tempV) continue;
                        if (MethodCallExpression::freshOwnedStringTemp(
                                parameters[ai].expression)) {
                            if (strDropFn && tempV->getType()->isPointerTy()) {
                                builder->CreateCall(strDropFn, {tempV});
                            }
                            continue;
                        }
                        if (auto vCls = MethodCallExpression::
                                freshSharedValueTempClass(
                                    parameters[ai].expression)) {
                            llvm::Value* slot = tempV;
                            if (!tempV->getType()->isPointerTy()) {
                                slot = builder->CreateAlloca(tempV->getType(),
                                    nullptr, "temp.value.rel");
                                builder->CreateStore(tempV, slot);
                            }
                            llvm::Function* relFn =
                                CajetaModule::ensureFunctionInModule(
                                    module->getLlvmModule(),
                                    vCls->getOrCreateValueReleaseFunction());
                            if (relFn) builder->CreateCall(relFn, {slot});
                        }
                    }
                }
            }
        }
        return instance;
    }

    // Java-style array allocation: one heap call per dimension level. For `new T[a][b]`
    // we allocate the outer header of length `a` whose element-type is a pointer to
    // an inner array, then loop and allocate an inner header of length `b` for each
    // outer slot. For `new T[a][]` we only allocate the outer; inner slots stay null
    // (the runtime helper zero-fills via calloc). Returns the outermost header pointer.
    llvm::Value* ArrayCreatorRest::generateCode(CajetaModulePtr module) {
        if (!targetType || totalBracketPairs <= 0) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();

        // Build the type chain: typeChain[0] = T (innermost element),
        // typeChain[k] = T[][...] wrapped k times. typeChain[N] is the outermost type.
        vector<CajetaTypePtr> typeChain;
        typeChain.push_back(targetType);
        for (int i = 0; i < totalBracketPairs; i++) {
            CajetaTypePtr wrapped = make_shared<CajetaArray>(module, typeChain.back());
            module->getStructures()[wrapped->toCanonical()] =
                static_pointer_cast<CajetaClass>(wrapped);
            typeChain.push_back(wrapped);
        }

        // U3: a non-escaping single-dimension primitive-element array routes its
        // header through the frame arena (no malloc, no live-set, no drop entry);
        // the scope-exit reset reclaims it. The escape pre-pass only sets
        // arenaEligible when totalBracketPairs==1 and the element is primitive, so
        // there are no inner sub-allocations to worry about. Multi-dim / class-
        // element creators keep the heap allocator.
        bool useArena = arenaEligible && totalBracketPairs == 1;
        llvm::Function* allocFn = module->getRuntimeFunction(
            useArena ? "__cajeta_new_array_header_arena" : "__cajeta_new_array_header");
        if (!allocFn) {
            return nullptr;
        }
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        // Recursive emitter: level 0 = outermost. Allocates that level's header and,
        // when an inner size was specified, loops over the data slots populating them
        // with recursive sub-allocations.
        std::function<llvm::Value*(int)> emit = [&](int level) -> llvm::Value* {
            if (level >= (int) children.size()) {
                return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            }
            auto arr = dynamic_pointer_cast<CajetaArray>(typeChain[totalBracketPairs - level]);
            if (!arr) {
                return nullptr;
            }

            // Resolve this level's user-supplied size, coerced l-value →
            // r-value. The same fix ArrayIndexExpression needed (see
            // Expression.cpp): a field-read dimension like
            // `new int8[this.chunkSize]` or a static-final
            // `new int8[AsyncWriter.DEFAULT_BUFFER]` returns a GEP /
            // GlobalVariable slot pointer, not an AllocaInst — the old
            // alloca-only check left it a `ptr`, and the CreateIntCast
            // below sext'd a pointer (LLVM verify error: "SExt only
            // operates on integer"). loadIfLValue loads through GEP and
            // GlobalVariable slots alike.
            llvm::Value* count = children[level]->generateCode(module);
            auto countAst = dynamic_pointer_cast<Expression>(children[level]);
            count = loadIfLValue(module, count, countAst);
            if (count->getType() != i64Ty) {
                count = builder->CreateIntCast(count, i64Ty, /*isSigned=*/true);
            }

            llvm::Type* headerTy = arr->getLlvmType();
            llvm::Type* elemTy = arr->getElementLlvmType(&ctx);
            llvm::Value* headerSize = llvm::ConstantInt::get(i64Ty,
                dl.getTypeAllocSize(headerTy));
            llvm::Value* elemSize = llvm::ConstantInt::get(i64Ty,
                dl.getTypeAllocSize(elemTy));
            llvm::Value* hdrPtr = builder->CreateCall(allocFn,
                {headerSize, elemSize, count});

            // If there's a deeper level to populate, loop over `count` slots and assign.
            if (level + 1 < (int) children.size()) {
                // Counter alloca at function entry to keep the loop clean of repeated allocas.
                llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                    parentFn->getEntryBlock().begin());
                llvm::Value* counterAlloca = entryBuilder.CreateAlloca(i64Ty);
                builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), counterAlloca);

                llvm::BasicBlock* loopHead = llvm::BasicBlock::Create(ctx, "arr_init_head", parentFn);
                llvm::BasicBlock* loopBody = llvm::BasicBlock::Create(ctx, "arr_init_body", parentFn);
                llvm::BasicBlock* loopExit = llvm::BasicBlock::Create(ctx, "arr_init_exit", parentFn);

                builder->CreateBr(loopHead);

                builder->SetInsertPoint(loopHead);
                llvm::Value* idx = builder->CreateLoad(i64Ty, counterAlloca);
                llvm::Value* cmp = builder->CreateICmpSLT(idx, count);
                builder->CreateCondBr(cmp, loopBody, loopExit);

                builder->SetInsertPoint(loopBody);
                llvm::Value* inner = emit(level + 1);
                // Slot = &hdrPtr->data[idx]. GEP indices walk: pointer -> struct -> data array -> element.
                vector<llvm::Value*> gepIndices = {
                    llvm::ConstantInt::get(i64Ty, 0),
                    llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
                    idx,
                };
                llvm::Value* slot = builder->CreateGEP(headerTy, hdrPtr, gepIndices);
                builder->CreateStore(inner, slot);
                llvm::Value* nextIdx = builder->CreateAdd(idx,
                    llvm::ConstantInt::get(i64Ty, 1));
                builder->CreateStore(nextIdx, counterAlloca);
                builder->CreateBr(loopHead);

                builder->SetInsertPoint(loopExit);
            }

            return hdrPtr;
        };

        return emit(0);
    }

} // code
