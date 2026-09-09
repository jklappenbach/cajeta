//
// Created by James Klappenbach on 4/19/23.
//

#include "CreatorRest.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/xref/XrefIndex.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/asn/expression/DotExpression.h"
#include "cajeta/asn/expression/Expression.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/NewExpression.h"
#include "cajeta/error/Exception.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/method/Method.h"
#include "cajeta/ownership/TitleClassifier.h"

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
    void ClassCreatorRest::resolveTypes(CajetaModulePtr module) {
        // LINT ONLY — see MethodCallExpression::resolveTypes. Resolving ctor
        // args this early pins their types before template substitution and
        // breaks real builds; the build path records constructor edges from
        // generateCode as it always has.
        if (!module || !module->isResolutionOnly()) {
            AbstractSyntaxNode::resolveTypes(module);
            return;
        }
        // Constructor args live in `parameters`, not `children` — the same
        // split MethodCallExpression has — so the default walk never reaches
        // them and a field read inside `heap Foo(b.v)` goes unrecorded.
        // Best-effort per argument: one that cannot resolve must not cost the
        // others theirs (xref-lint-emission-gap 4.2.3).
        for (auto& child : children) {
            if (!child) continue;
            try { child->resolveTypes(module); } catch (...) { }
        }
        for (auto& p : parameters) {
            if (!p.expression) continue;
            try { p.expression->resolveTypes(module); } catch (...) { }
        }
        // The constructor EDGE is recorded by NewExpression::resolveTypes,
        // which is where the created type is known — `targetType` here is set
        // by generateCode and is still null on the lint path.
    }

    llvm::Value* ClassCreatorRest::generateCode(CajetaModulePtr module) {
        // xref (ide-symbol-index §2): `heap Foo(args)` / `stack Foo(args)` resolves a
        // CONSTRUCTOR through CajetaClass::resolveMethod, so open this call site or
        // the constructor call is recorded against whatever site happens to be open
        // (or, with none open, dropped). Ctrl-click on `heap Derived(7)` should land
        // on Derived's constructor.
        // The file comes from THIS NODE, not from `module` — see
        // MethodCallExpression::generateCode.
        xref::CallSiteScope xrefSite(getSourceFile(),
                                     getSourceLine(), getSourceColumn());

        if (!targetType) {
            return nullptr;
        }
        // placeholder-owned-field defect (table-fit §2's discipline extended
        // to codegen): CONSTRUCTION must never build against an unfilled
        // placeholder — its getLlvmType() is a bare `ptr`, so the alloc
        // size, instance layout, ctor set, and drop symbol all come out
        // wrong (malformed IR; the AOT flavor called a garbage pointer).
        // Materialize the declaring user module on demand — the placeholder
        // fills in place (same shared_ptr) — and fail LOUDLY if it cannot.
        if (auto phk = dynamic_pointer_cast<CajetaClass>(targetType)) {
            if (phk->isPlaceholder() && phk->getQName()
                    && CajetaModule::userMaterializeHook) {
                CajetaModule::userMaterializeHook(
                    phk->getQName()->toCanonical());
            }
            if (phk->isPlaceholder()) {
                throw Exception(
                    "cannot construct '"
                        + (phk->getQName() ? phk->getQName()->toCanonical()
                                           : std::string("<unknown>"))
                        + "': its declaration has not been compiled "
                          "(unresolved placeholder)",
                    "CAJETA_ERROR_UNRESOLVED_PLACEHOLDER");
            }
        }
        // U2 (plan 2.2.3) — `heap T(#x)` is the shape the cajeta-llama bug
        // actually took (`heap String(#kb, kl)`). Constructor arguments carry
        // the same `callerTransferred` flag as call arguments and likewise
        // build no MoveExpression, so they need the same rejection.
        rejectTransferOfBorrowArgs(module, parameters);

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
        // 6.2.5 — flags for PLAIN ctor args that are class-pointer call
        // results (anonymous rvalues): read each one's return-flag TLS
        // immediately after its generation (the next arg's call clobbers
        // it) and forward it into the ctor word below, exactly as
        // MethodCallExpression's arg loop does.
        // Unit 7 — each constructor argument's classified title (shape +
        // flag), filled right after its codegen; the two flag vectors keep
        // the bits that ride the constructor's transfer word (a plain
        // argument's, a `#x` argument's).
        std::vector<ownership::ArgTitle> ctorArgTitles(parameters.size());
        std::vector<llvm::Value*> plainArgTempFlags(parameters.size(), nullptr);
        std::vector<llvm::Value*> ctorArgTitleFlags(parameters.size(), nullptr);
        size_t ctorArgIndex = (size_t) -1;
        for (auto& param : parameters) {
            ++ctorArgIndex;
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            llvm::Value* value = param.expression->generateCode(module);
            // Unit 7 — the argument's title, on the classifier, AFTER its
            // codegen (see MethodCallExpression's argument loop): a call's
            // TLS is read now, a `#x` name's entry is read now, a static
            // owner and a fresh value are constants, an intrinsic lowering
            // stores no flag. Only a class or array title rides the word; a
            // `#x` always composes.
            if (ctorArgIndex < ctorArgTitles.size()) {
                ctorArgTitles[ctorArgIndex] = ownership::classifyArgument(
                    param.expression, param.callerTransferred, module, "a `#` argument");
                CajetaTypePtr argTy = param.expression->getResolvedType();
                bool wordCarrier = param.callerTransferred
                    || MethodCallExpression::droppableTempClass(argTy) != nullptr
                    || dynamic_pointer_cast<CajetaArray>(argTy) != nullptr
                    // Unit 9 (spec 5.13) — a closure rides the word too (a lambda
                    // literal is a fresh owner, a name lends).
                    || dynamic_pointer_cast<CajetaFunctionType>(argTy) != nullptr;
                if (wordCarrier && ctorArgTitles[ctorArgIndex].flag) {
                    if (param.callerTransferred) {
                        ctorArgTitleFlags[ctorArgIndex] = ctorArgTitles[ctorArgIndex].flag;
                    } else {
                        plainArgTempFlags[ctorArgIndex] = ctorArgTitles[ctorArgIndex].flag;
                    }
                }
            }
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
            //   2. Callee-side: a formal marked `#T` whose argument the
            //      caller did NOT mark `#` is a CONTRACT VIOLATION, not an
            //      implicit transfer — pass 2 throws
            //      CAJETA_ERROR_TRANSFER_REQUIRED. A `#T` formal never
            //      deactivates a caller drop entry on its own; only the
            //      caller's `#` does.
            //
            // Primitives, literals, and locals without drop entries
            // naturally degrade to no-op (the inner gate fires only on
            // an IdentifierExpression arg whose Field has a drop entry).
            // 5.2.4 / 6.2.4 — capture each `#`-arg's TITLE FLAG before Pass 1
            // deactivates it (MethodCallExpression's rule; a ctor arg's `#`
            // is the parse-level token, NOT a MoveExpression, so the old
            // MoveExpression-stash-only composition read null and degraded
            // EVERY `#formal` ctor arg to a STATIC 1 — a borrow put through
            // `heap RedBlackNode(#key, #value)` stamped the node's bits
            // owned and the teardown freed the caller's value). A runtime
            // owner forwards the flag its entry holds; an entry-less formal
            // (String/primitive) forwards its own incoming word bit.
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
                for (size_t i = 0; i < parameters.size(); ++i) {
                    if (!parameters[i].callerTransferred) continue;
                    auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                        parameters[i].expression);
                    if (idExpr) {
                        if (auto scope = module->getScopeStack().peek()) {
                            const string& nm = idExpr->getTextValue();
                            FieldPtr field = scope->getField(nm);
                            // Unit 7 — the scope's move bookkeeping, as
                            // MoveExpression does it (see the call site).
                            if (field && !std::dynamic_pointer_cast<ParameterField>(field)) {
                                scope->rejectTransferOfBorrow(nm, /*modeCarrying=*/false);
                                auto kls = std::dynamic_pointer_cast<CajetaClass>(
                                    field->getType());
                                if (kls && !kls->isValueType()
                                        && !kls->isSharedCapableValue()
                                        && !kls->isInterface()) {
                                    scope->demoteToBorrow(nm,
                                        "transferred to `" + ctorName
                                            + "` at line "
                                            + std::to_string(getSourceLine()));
                                }
                            }
                        }
                    }
                    deactivateIfClassLocal(i);
                }
                // Unit 7 — the `#T`-formal contract on the classifier (spec
                // 5.8, 5.11, 5.10), the constructor twin of the call site.
                MethodPtr xferTarget = klass->resolveMethod(
                    ctorName, entries, /*isConstructor=*/true,
                    /*floatingParams=*/false);
                if (xferTarget) {
                    auto formalParams = xferTarget->getParameterList();
                    bool hasThisP = !formalParams.empty()
                        && formalParams.front()->getName() == "this";
                    int xferParamOffset = hasThisP ? 1 : 0;
                    size_t fIdx = 0;
                    for (auto& fp : formalParams) {
                        if ((int) fIdx < xferParamOffset) { ++fIdx; continue; }
                        size_t argIdx = fIdx - xferParamOffset;
                        if (argIdx >= parameters.size()) break;
                        ++fIdx;
                        if (!fp->isTransferred()) continue;
                        auto escArg = parameters[argIdx].expression;
                        if (!escArg) continue;
                        if (escArg->kind() == ExprKind::Move
                                || parameters[argIdx].callerTransferred) {
                            ownership::rejectEscape(escArg,
                                ownership::ConsumerRole::ArgOwned, module,
                                "a `#T` argument");
                        }
                        ownership::rejectOwnedFormalArgument(escArg,
                            parameters[argIdx].callerTransferred, module,
                            ctorName, fp->getName(), (int) getSourceLine());
                    }
                }
            }
            // Unit 7 — the constructor's transfer word, one answer per
            // argument (ownership::classifyArgument): a constant title is a
            // static bit, a runtime one is OR'd in; no title, no bit.
            auto* twBuilder = module->getBuilder();
            int64_t ctorTransferWord = 0;
            llvm::Value* ctorWordVal = nullptr;
            for (size_t twi = 0; twi < parameters.size(); ++twi) {
                llvm::Value* rf = parameters[twi].callerTransferred
                    ? ctorArgTitleFlags[twi] : plainArgTempFlags[twi];
                if (!rf) continue;
                if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(rf)) {
                    if (!k->isZero()) ctorTransferWord |= ((int64_t) 1) << twi;
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
                        // Unit 7 — a String temp the constructor only
                        // borrowed is the caller's to release: a static title
                        // drops now, a runtime one drops on its flag (see the
                        // call site's reclaim).
                        if (ai < ctorArgTitles.size()
                                && ctorArgTitles[ai].shape.has(ownership::TitleShape::kString)
                                && ctorArgTitles[ai].shape.family != ownership::TitleFamily::Literal
                                && strDropFn && tempV->getType()->isPointerTy()) {
                            const auto& at = ctorArgTitles[ai];
                            if (at.shape.answer == ownership::TitleAnswer::Owned) {
                                builder->CreateCall(strDropFn, {tempV});
                            } else if (at.shape.answer == ownership::TitleAnswer::Runtime
                                    && at.flag) {
                                if (auto* cf = llvm::dyn_cast<llvm::ConstantInt>(at.flag)) {
                                    if (!cf->isZero()) builder->CreateCall(strDropFn, {tempV});
                                } else {
                                    auto& tctx = *module->getLlvmContext();
                                    llvm::Function* tfn = builder->GetInsertBlock()->getParent();
                                    auto* dropBB = llvm::BasicBlock::Create(tctx, "ctor_arg_temp_drop", tfn);
                                    auto* contBB = llvm::BasicBlock::Create(tctx, "ctor_arg_temp_cont", tfn);
                                    builder->CreateCondBr(
                                        builder->CreateICmpNE(at.flag,
                                            llvm::ConstantInt::get(at.flag->getType(), 0),
                                            "ctor_arg_temp_owned"),
                                        dropBB, contBB);
                                    builder->SetInsertPoint(dropBB);
                                    builder->CreateCall(strDropFn, {tempV});
                                    builder->CreateBr(contBB);
                                    builder->SetInsertPoint(contBB);
                                }
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
        // title-stores §3.1 — bit-capable elements (the archived Unit-4
        // predicate) get the tail-bitmap allocator. Only the innermost
        // level can qualify: outer levels' elements are arrays, which the
        // predicate rejects. Arena arrays are primitive-only, so the two
        // never collide.
        // title-stores §3.4 — OUTER levels qualify too now: their elements
        // are arrays, and a jagged slot owns its inner buffer, so the outer
        // header needs the tail bitmap the arrelem family reads. Fetch the
        // allocator whenever any level could need it (per-level pick below).
        // The condition MUST match the per-level pick below, or the pick
        // short-circuits on a null and silently allocates a header with no
        // tail bitmap while the stores and the drop walk both use one.
        //
        // `totalBracketPairs > 1` covers a LITERAL nested array. It does not
        // cover an element type that arrives through a TYPE PARAMETER —
        // `heap T[cap]` with `T = int8[]` inside a generic container has ONE
        // bracket pair, and `arrayElementCarriesSlotBits` rejects arrays by
        // its first line. So `ArrayList<int8[]>` allocated a bitmap-less
        // header, `data[i] #= v` wrote slot bits past the payload, and the
        // process died in `free` at teardown (plan 8.13). Asking
        // `arrayElementCarriesArraySlotBits` here is what the level pick
        // already asks.
        llvm::Function* bitsAllocFn = nullptr;
        if (!useArena && (CajetaClass::arrayElementCarriesSlotBits(targetType)
                || CajetaClass::arrayElementCarriesArraySlotBits(targetType)
                || totalBracketPairs > 1)) {
            bitsAllocFn = module->getRuntimeFunction("__cajeta_new_array_header_bits");
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
            // A dimension that did not resolve to a VALUE used to reach
            // `count->getType()` and segfault on null — the compiler dying
            // with a raw SIGSEGV and no source location at all. A crash is
            // never an acceptable diagnostic: it says nothing about which
            // `heap T[n]` was at fault, and the null comes from an ordinary
            // authoring or resolution failure in `n`, not from a corrupt
            // state.
            if (count == nullptr) {
                throw Exception(
                    "array dimension did not resolve to a value at level "
                        + std::to_string(level)
                        + " of this `heap` array creation (a sub-expression "
                          "produced no value — e.g. a static field or member "
                          "that did not resolve)",
                    "CAJETA_ERROR_NULL_ARRAY_DIMENSION");
            }
            if (count->getType() != i64Ty) {
                count = builder->CreateIntCast(count, i64Ty, /*isSigned=*/true);
            }

            llvm::Type* headerTy = arr->getLlvmType();
            llvm::Type* elemTy = arr->getElementLlvmType(&ctx);
            llvm::Value* headerSize = llvm::ConstantInt::get(i64Ty,
                dl.getTypeAllocSize(headerTy));
            llvm::Value* elemSize = llvm::ConstantInt::get(i64Ty,
                arr->elementStrideBytes(dl, &ctx));
            bool levelHasBits = bitsAllocFn
                && (CajetaClass::arrayElementCarriesSlotBits(arr->getElementType())
                    || CajetaClass::arrayElementCarriesArraySlotBits(
                           arr->getElementType()));
            llvm::Value* hdrPtr = builder->CreateCall(
                levelHasBits ? bitsAllocFn : allocFn,
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
