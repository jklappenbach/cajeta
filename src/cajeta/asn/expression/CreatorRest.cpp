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

    void ClassCreatorRest::resolveTypes(CajetaModulePtr module) {
        // Lint only: on the build path, resolving ctor args this early pins
        // their types before template substitution and breaks real builds.
        if (!module || !module->isResolutionOnly()) {
            AbstractSyntaxNode::resolveTypes(module);
            return;
        }
        // Best effort: an argument that cannot resolve must not cost the others theirs.
        for (auto& child : children) {
            if (!child) continue;
            try { child->resolveTypes(module); } catch (...) { }
        }
        for (auto& p : parameters) {
            if (!p.expression) continue;
            try { p.expression->resolveTypes(module); } catch (...) { }
        }
    }

    // Allocates the instance (caller sret slot under NRVO, entry-block alloca for
    // `stack`, malloc otherwise), installs the vtable, and invokes the matching
    // constructor with the transfer word built from the arguments' titles.
    llvm::Value* ClassCreatorRest::generateCode(CajetaModulePtr module) {
        // Open the xref call site first, or the constructor edge is recorded
        // against whatever site happens to be open. The file comes from this
        // node, not from `module`.
        xref::CallSiteScope xrefSite(getSourceFile(),
                                     getSourceLine(), getSourceColumn());

        if (!targetType) {
            return nullptr;
        }
        // An unfilled placeholder's llvm type is a bare `ptr`, so alloc size,
        // layout, ctor set and drop symbol all come out wrong: materialize the
        // declaring module on demand (fills in place) and fail loudly if it cannot.
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
            // NRVO: init and ctor run against the caller-owned sret slot, no copy.
            instance = nrvoTarget;
        } else if (stackAlloc) {
            // Hoisted to the entry block so the alloca lives for the whole frame:
            // an alloca in a loop body would grow the frame per iteration.
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                parentFn->getEntryBlock().begin());
            instance = entryBuilder.CreateAlloca(structTy);
        } else {
            instance = MemoryManager::createMallocInstruction(
                module, allocSize, builder->GetInsertBlock());
        }

        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            klass->initInstanceLayout(module, instance, structTy, stackAlloc);
        } else {
            builder->CreateMemSet(instance,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvmCtx), 0),
                allocSize, llvm::MaybeAlign(8));
        }

        // An untyped lambda argument takes its param and return types from the
        // matching ctor formal; there is no other inference signal. The ctor is
        // matched by arity, and its formal 0 is `this`, so the count subtracts 1.
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

        // The expression's resolvedType is preferred over `CajetaType::of(value)`,
        // which cannot recover a class-instance type from a bare `ptr`.
        vector<ParameterEntry> entries;
        // Each argument's title flag must be read immediately after that argument's
        // codegen: the next argument's call clobbers the return-flag TLS.
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
            if (ctorArgIndex < ctorArgTitles.size()) {
                ctorArgTitles[ctorArgIndex] = ownership::classifyArgument(
                    param.expression, param.callerTransferred, module, "a `#` argument");
                CajetaTypePtr argTy = param.expression->getResolvedType();
                bool wordCarrier = param.callerTransferred
                    || MethodCallExpression::droppableTempClass(argTy) != nullptr
                    || dynamic_pointer_cast<CajetaArray>(argTy) != nullptr
                    || dynamic_pointer_cast<CajetaFunctionType>(argTy) != nullptr;
                if (wordCarrier && ctorArgTitles[ctorArgIndex].flag) {
                    if (param.callerTransferred) {
                        ctorArgTitleFlags[ctorArgIndex] = ctorArgTitles[ctorArgIndex].flag;
                    } else {
                        plainArgTempFlags[ctorArgIndex] = ctorArgTitles[ctorArgIndex].flag;
                    }
                }
            }
            auto astExpr = dynamic_pointer_cast<Expression>(param.expression);
            value = loadIfLValue(module, value, astExpr);
            CajetaTypePtr paramType = param.expression->getResolvedType();
            if (!paramType) paramType = CajetaType::of(value);
            entries.push_back(ParameterEntry(paramType, param.label, value));
        }
        if (auto klass = dynamic_pointer_cast<CajetaClass>(targetType)) {
            // A template instantiation's ctor was parsed under the unparameterized
            // name, so look it up there or resolution finds only the auto-default.
            string ctorName = targetType->getQName()->getTypeName();
            if (klass->getTemplateOrigin()) {
                ctorName = klass->getTemplateOrigin()->getQName()->getTypeName();
            }
            // Two passes: the caller's `#x` deactivates its source local's drop entry
            // (caller intent is authoritative, and synthesized ctors never resolve),
            // then a `#T` formal whose argument carries no `#` is rejected outright.
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
            // Transfer word: bit i is set when argument i tenders title. A constant
            // title folds into the literal; a runtime flag is shifted and OR'd in.
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
            // Fresh temps consumed as ctor arguments have no drop entry, and this is
            // the only site that sees them: reclaim owned String temps and release
            // shared-value stakes. Arguments taken by `#T` or `#x` were transferred.
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

    // Allocates one array header per sized dimension and returns the outermost:
    // `new T[a][b]` fills each of the outer header's `a` slots with an inner header
    // of length `b`, while `new T[a][]` leaves the inner slots null (calloc'd).
    llvm::Value* ArrayCreatorRest::generateCode(CajetaModulePtr module) {
        if (!targetType || totalBracketPairs <= 0) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();

        // typeChain[0] = T, typeChain[k] = T wrapped k times, so [N] is outermost.
        vector<CajetaTypePtr> typeChain;
        typeChain.push_back(targetType);
        for (int i = 0; i < totalBracketPairs; i++) {
            CajetaTypePtr wrapped = make_shared<CajetaArray>(module, typeChain.back());
            module->getStructures()[wrapped->toCanonical()] =
                static_pointer_cast<CajetaClass>(wrapped);
            typeChain.push_back(wrapped);
        }

        // An arena header carries no drop entry; the scope-exit reset reclaims it.
        // arenaEligible is only set for a single-dimension primitive-element array,
        // so there are never inner sub-allocations to reclaim separately.
        bool useArena = arenaEligible && totalBracketPairs == 1;
        llvm::Function* allocFn = module->getRuntimeFunction(
            useArena ? "__cajeta_new_array_header_arena" : "__cajeta_new_array_header");
        if (!allocFn) {
            return nullptr;
        }
        // Fetch the tail-bitmap allocator whenever any level could need one. This
        // condition must match the per-level pick below, or a level allocates a
        // bitmap-less header while its slot stores and drop walk both use one.
        llvm::Function* bitsAllocFn = nullptr;
        if (!useArena && (CajetaClass::arrayElementCarriesSlotBits(targetType)
                || CajetaClass::arrayElementCarriesArraySlotBits(targetType)
                || totalBracketPairs > 1)) {
            bitsAllocFn = module->getRuntimeFunction("__cajeta_new_array_header_bits");
        }
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        // Recursive emitter, level 0 = outermost: allocates that level's header and,
        // when a deeper size was given, fills each slot with a sub-allocation.
        std::function<llvm::Value*(int)> emit = [&](int level) -> llvm::Value* {
            if (level >= (int) children.size()) {
                return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx, 0));
            }
            auto arr = dynamic_pointer_cast<CajetaArray>(typeChain[totalBracketPairs - level]);
            if (!arr) {
                return nullptr;
            }

            // A field or static-final dimension arrives as a slot pointer, so it must
            // be loaded before the int cast below, which cannot sext a pointer.
            llvm::Value* count = children[level]->generateCode(module);
            auto countAst = dynamic_pointer_cast<Expression>(children[level]);
            count = loadIfLValue(module, count, countAst);
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

            if (level + 1 < (int) children.size()) {
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
                // &hdrPtr->data[idx]: the indices walk pointer, struct, data, element.
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
