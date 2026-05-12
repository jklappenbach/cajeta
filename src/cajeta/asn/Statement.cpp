//
// Created by James Klappenbach on 2/19/22.
//

#include "Statement.h"
#include "expression/Expression.h"
#include "expression/Identifier.h"
#include "../compile/CajetaModule.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../type/CajetaArray.h"
#include "Block.h"
#include "LocalVariableDeclaration.h"

/**
 * statement
    : blockLabel=block
    | ASSERT expression (':' expression)? ';'
    | IF parExpression statement (ELSE statement)?
    | FOR '(' forControl ')' statement
    | WHILE parExpression statement
    | DO statement WHILE parExpression ';'
    | TRY block (catchClause+ finallyBlock? | finallyBlock)
    | TRY resourceSpecification block catchClause* finallyBlock?
    | SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
    | SYNCHRONIZED parExpression block
    | RETURN expression? ';'
    | THROW expression ';'
    | BREAK identifier? ';'
    | CONTINUE identifier? ';'
    | YIELD expression ';' // Java17
    | SEMI
    | statementExpression=expression ';'
    | switchExpression ';'? // Java17
    | identifierLabel=identifier ':' statement
    ;

 * @param ctx
 */
namespace cajeta {

    // Forward decl for the helper used by both `block()` and `IF/WHILE/FOR/DO` paths.
    static BlockStatementPtr buildBlockStatement(CajetaParser::BlockStatementContext* ctx);

    // Construct a LocalVariableDeclaration from a parse context. Used by nested-block
    // building and by FOR's variable-decl init form. Returns nullptr on malformed
    // input.
    static shared_ptr<LocalVariableDeclaration>
    buildLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* lvdCtx) {
        if (!lvdCtx) return nullptr;
        set<Modifier> modifiers;
        for (auto* mod : lvdCtx->variableModifier()) {
            modifiers.insert(Modifiable::toModifier(mod->getText()));
        }
        // Type resolution without a module — works for primitives and class references;
        // arrays in nested decls need module-aware registration we don't have here.
        CajetaTypePtr type = CajetaType::fromContext(lvdCtx->typeType(), nullptr);
        list<VariableDeclaratorPtr> declarators;
        if (auto* vdsCtx = lvdCtx->variableDeclarators()) {
            for (auto* vdCtx : vdsCtx->variableDeclarator()) {
                InitializerPtr initializer;
                if (auto* viCtx = vdCtx->variableInitializer()) {
                    if (viCtx->expression()) {
                        initializer = make_shared<VariableInitializer>(
                            Expression::fromContext(viCtx->expression()),
                            viCtx->getStart());
                    }
                }
                string identName = vdCtx->variableDeclaratorId()->identifier()->getText();
                int arrayDim = static_cast<int>(vdCtx->variableDeclaratorId()->LBRACK().size());
                // The legacy `REFERENCE? variableInitializer` form was removed
                // from the grammar; transfers now flow through MoveExpression in
                // the initializer expression itself.
                declarators.push_back(make_shared<VariableDeclarator>(
                    identName, /*isReference=*/false, arrayDim, initializer, vdCtx->getStart()));
            }
        }
        return make_shared<LocalVariableDeclaration>(
            modifiers, type, declarators, lvdCtx->getStart());
    }

    // Build a Block AST from a BlockContext. Each child is either:
    //   - a Statement (recursive Statement::fromContext)
    //   - a LocalVariableDeclaration (primitive / class types only — array types in
    //     nested blocks need module-aware codegen and aren't supported here yet;
    //     declare them at function top-level for now).
    // Local type declarations are skipped (out of scope).
    static BlockPtr buildBlock(CajetaParser::BlockContext* ctx) {
        auto block = make_shared<Block>(ctx->getStart());
        for (auto* bsCtx : ctx->blockStatement()) {
            if (auto child = buildBlockStatement(bsCtx)) {
                block->addChild(child);
            }
        }
        return block;
    }

    static BlockStatementPtr buildBlockStatement(CajetaParser::BlockStatementContext* ctx) {
        if (auto* lvdCtx = ctx->localVariableDeclaration()) {
            return buildLocalVariableDeclaration(lvdCtx);
        }
        if (auto* stmtCtx = ctx->statement()) {
            return Statement::fromContext(stmtCtx);
        }
        // localTypeDeclaration (nested class) — out of scope.
        return nullptr;
    }

    StatementPtr Statement::fromContext(CajetaParser::StatementContext* ctx) {
        StatementPtr result;

        antlr4::Token* token = ctx->getStart();

        if (ctx->statementExpression) {
            cout << "Hit statementExpression in a Statement";
        }

        // Order matters: TRY / SWITCH / FOR / WHILE / DO / IF can each contain a block
        // as a sub-rule, so `ctx->block()` is non-null for those too. Check the
        // discriminating keywords first; the bare-block form falls through to the
        // tail of this chain.
        if (ctx->IF()) {
            // IF parExpression statement (ELSE statement)?
            ExpressionPtr cond = ctx->parExpression()
                ? Expression::fromContext(ctx->parExpression()->expression())
                : nullptr;
            StatementPtr thenStmt = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            StatementPtr elseStmt = ctx->statement().size() > 1
                ? Statement::fromContext(ctx->statement(1))
                : nullptr;
            result = make_shared<IfStatement>(token, cond, thenStmt, elseStmt);
        } else if (ctx->FOR()) {
            // FOR '(' forControl ')' statement.
            //   - enhancedForControl: for-each over an iterable
            //   - C-style: optional init, condition, and update list
            auto forCtl = ctx->forControl();
            StatementPtr body = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            if (forCtl && forCtl->enhancedForControl()) {
                auto ec = forCtl->enhancedForControl();
                CajetaTypePtr iterType;
                string iterName;
                if (auto it = ec->loopIterator()) {
                    if (it->typeType()) {
                        iterType = CajetaType::fromContext(it->typeType(), nullptr);
                    }
                    if (it->variableDeclaratorId()) {
                        iterName = it->variableDeclaratorId()->identifier()->getText();
                    }
                }
                CajetaTypePtr elemType;
                string elemName;
                ExpressionPtr iterableExpr;
                if (auto lv = ec->loopVariable()) {
                    if (lv->typeType()) {
                        elemType = CajetaType::fromContext(lv->typeType(), nullptr);
                    }
                    if (lv->variableDeclaratorId()) {
                        elemName = lv->variableDeclaratorId()->identifier()->getText();
                    }
                    if (lv->expression()) {
                        iterableExpr = Expression::fromContext(lv->expression());
                    }
                }
                result = make_shared<EnhancedForStatement>(token,
                    iterType, iterName, elemType, elemName, iterableExpr, body);
            } else {
                BlockStatementPtr init;
                ExpressionPtr cond;
                list<ExpressionPtr> updates;
                if (forCtl) {
                    if (auto fi = forCtl->forInit()) {
                        if (fi->localVariableDeclaration()) {
                            init = buildLocalVariableDeclaration(fi->localVariableDeclaration());
                        } else if (fi->expressionList() && !fi->expressionList()->expression().empty()) {
                            init = make_shared<ExpressionStatement>(
                                Expression::fromContext(fi->expressionList()->expression(0)),
                                token);
                        }
                    }
                    if (forCtl->expression()) {
                        cond = Expression::fromContext(forCtl->expression());
                    }
                    if (forCtl->expressionList()) {
                        for (auto* e : forCtl->expressionList()->expression()) {
                            updates.push_back(Expression::fromContext(e));
                        }
                    }
                }
                result = make_shared<ForStatement>(token, init, cond, updates, body);
            }
        } else if (ctx->WHILE() && !ctx->DO()) {
            // WHILE parExpression statement
            ExpressionPtr cond = ctx->parExpression()
                ? Expression::fromContext(ctx->parExpression()->expression())
                : nullptr;
            StatementPtr body = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            result = make_shared<WhileStatement>(token, cond, body);
        } else if (ctx->DO()) {
            // DO statement WHILE parExpression ';'
            StatementPtr body = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            ExpressionPtr cond = ctx->parExpression()
                ? Expression::fromContext(ctx->parExpression()->expression())
                : nullptr;
            result = make_shared<DoStatement>(token, body, cond);
        } else if (ctx->SWITCH()) {
            // SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
            ExpressionPtr subj = ctx->parExpression()
                ? Expression::fromContext(ctx->parExpression()->expression())
                : nullptr;
            std::vector<SwitchGroup> groups;
            for (auto* groupCtx : ctx->switchBlockStatementGroup()) {
                SwitchGroup g;
                for (auto* lblCtx : groupCtx->switchLabel()) {
                    if (lblCtx->DEFAULT()) {
                        g.isDefault = true;
                    } else if (auto* exprCtx = lblCtx->expression()) {
                        g.caseValues.push_back(Expression::fromContext(exprCtx));
                    } else if (auto* idCtx = lblCtx->identifier()) {
                        // `case SOME_NAME:` — bare identifier (enum constant).
                        // Treat as an IdentifierExpression so it resolves like any
                        // other constant reference.
                        g.caseValues.push_back(
                            make_shared<IdentifierExpression>(idCtx, true));
                    }
                }
                for (auto* bsCtx : groupCtx->blockStatement()) {
                    if (auto child = buildBlockStatement(bsCtx)) {
                        g.statements.push_back(child);
                    }
                }
                groups.push_back(std::move(g));
            }
            result = make_shared<SwitchStatement>(token, subj, std::move(groups));
        } else if (ctx->TRY()) {
            // TRY block (catchClause+ finallyBlock? | finallyBlock)
            BlockPtr tryBlk = ctx->block() ? buildBlock(ctx->block()) : nullptr;
            std::vector<CatchClause> catches;
            for (auto* ccCtx : ctx->catchClause()) {
                CatchClause c;
                // catchType is `qualifiedName ('|' qualifiedName)*`. We take just the
                // first qualifiedName for now (no multi-catch). Look it up as a
                // primitive or class type.
                if (auto* catchType = ccCtx->catchType()) {
                    if (!catchType->qualifiedName().empty()) {
                        string typeName = catchType->qualifiedName(0)->getText();
                        c.type = CajetaType::of(typeName);
                        if (!c.type) {
                            c.type = CajetaType::of(typeName, "");
                        }
                    }
                }
                if (auto* idCtx = ccCtx->identifier()) {
                    c.variableName = idCtx->getText();
                }
                if (ccCtx->block()) {
                    c.body = buildBlock(ccCtx->block());
                }
                catches.push_back(std::move(c));
            }
            BlockPtr finallyBlk;
            if (auto* fbCtx = ctx->finallyBlock()) {
                if (fbCtx->block()) {
                    finallyBlk = buildBlock(fbCtx->block());
                }
            }
            result = make_shared<TryStatement>(token, tryBlk, std::move(catches), finallyBlk);
        } else if (ctx->RETURN()) {
            ExpressionPtr returnExpr = ctx->expression()
                ? Expression::fromContext(ctx->expression())
                : nullptr;
            result = make_shared<ReturnStatement>(token, returnExpr);
        } else if (ctx->THROW()) {
            ExpressionPtr throwExpr = ctx->expression()
                ? Expression::fromContext(ctx->expression())
                : nullptr;
            result = make_shared<ThrowStatement>(token, throwExpr);
        } else if (ctx->BREAK()) {
            result = make_shared<BreakStatement>(token);
        } else if (ctx->CONTINUE()) {
            result = make_shared<ContinueStatement>(token);
        } else if (ctx->YIELD()) {
            result = make_shared<YieldStatement>(token);
        } else if (ctx->block()) {
            // Bare block-as-statement (no keyword discriminator), e.g. `{ stmt1; stmt2; }`
            // in statement position. Reached only after all keyword forms (which can
            // contain a block as a sub-rule) have been checked.
            result = make_shared<LabelStatement>(token, buildBlock(ctx->block()));
        } else if (ctx->expression()) {
            // Expressions and Statements are siblings; wrap an expression appearing in
            // statement position so it conforms to the Statement contract.
            result = make_shared<ExpressionStatement>(Expression::fromContext(ctx->expression()), token);
        } else if (ctx->statementExpression) {
        } else if (ctx->switchExpression()) {
            cout << "Hit switch expression";
            //result = new SwitchExpression;
        } else if (ctx->identifierLabel) {
            result = make_shared<IdentifierLabel>(ctx->getStart());
        } else if (ctx->SEMI()) {
            cout << "Hit SEMI statement";
        }

        return result;
    }

    llvm::Value* ExpressionStatement::generateCode(CajetaModulePtr module) {
        return expression ? expression->generateCode(module) : nullptr;
    }

    void ExpressionStatement::resolveTypes(CajetaModulePtr module) {
        if (expression) expression->resolveTypes(module);
    }

    void ReturnStatement::resolveTypes(CajetaModulePtr module) {
        if (expression) expression->resolveTypes(module);
    }

    void LabelStatement::resolveTypes(CajetaModulePtr module) {
        if (block) block->resolveTypes(module);
    }

    llvm::Value* LabelStatement::generateCode(CajetaModulePtr module) {
        if (block) block->generateCode(module);
        return nullptr;
    }

    // Helper used by every control-flow statement: evaluate `cond`, load if it's an
    // l-value, and coerce to i1. Treats nullptr `cond` as "always true" so callers
    // can pass it through directly for unconditional forms.
    static llvm::Value* evalCondition(CajetaModulePtr module, ExpressionPtr cond) {
        auto* builder = module->getBuilder();
        llvm::Type* i1Ty = llvm::Type::getInt1Ty(*module->getLlvmContext());
        if (!cond) {
            return llvm::ConstantInt::getTrue(*module->getLlvmContext());
        }
        llvm::Value* v = cond->generateCode(module);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
            v = builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (!v) return llvm::ConstantInt::getFalse(*module->getLlvmContext());
        if (v->getType() != i1Ty) {
            llvm::Value* zero = llvm::ConstantInt::get(v->getType(), 0);
            v = builder->CreateICmpNE(v, zero);
        }
        return v;
    }

    void IfStatement::resolveTypes(CajetaModulePtr module) {
        if (condition) condition->resolveTypes(module);
        if (thenBranch) thenBranch->resolveTypes(module);
        if (elseBranch) elseBranch->resolveTypes(module);
    }

    llvm::Value* IfStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Value* condVal = evalCondition(module, condition);
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(ctx, "if_then", parentFn);
        llvm::BasicBlock* elseBB = elseBranch
            ? llvm::BasicBlock::Create(ctx, "if_else", parentFn)
            : nullptr;
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx, "if_merge", parentFn);

        builder->CreateCondBr(condVal, thenBB, elseBB ? elseBB : mergeBB);

        builder->SetInsertPoint(thenBB);
        if (thenBranch) thenBranch->generateCode(module);
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }

        if (elseBB) {
            builder->SetInsertPoint(elseBB);
            if (elseBranch) elseBranch->generateCode(module);
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        }

        builder->SetInsertPoint(mergeBB);
        return nullptr;
    }

    void WhileStatement::resolveTypes(CajetaModulePtr module) {
        if (condition) condition->resolveTypes(module);
        if (body) body->resolveTypes(module);
    }

    llvm::Value* WhileStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        llvm::BasicBlock* headBB = llvm::BasicBlock::Create(ctx, "while_head", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx, "while_body", parentFn);
        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx, "while_exit", parentFn);

        builder->CreateBr(headBB);

        builder->SetInsertPoint(headBB);
        llvm::Value* condVal = evalCondition(module, condition);
        builder->CreateCondBr(condVal, bodyBB, exitBB);

        builder->SetInsertPoint(bodyBB);
        module->pushLoopContext(headBB, exitBB);
        if (body) body->generateCode(module);
        module->popLoopContext();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(headBB);
        }

        builder->SetInsertPoint(exitBB);
        return nullptr;
    }

    void ForStatement::resolveTypes(CajetaModulePtr module) {
        if (init) init->resolveTypes(module);
        if (condition) condition->resolveTypes(module);
        for (auto& u : update) {
            if (u) u->resolveTypes(module);
        }
        if (body) body->resolveTypes(module);
    }

    llvm::Value* ForStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        // Init runs once, in the entry block. (Variable-decl inits are stitched in
        // by fromContext; expression inits become an ExpressionStatement.)
        if (init) init->generateCode(module);

        llvm::BasicBlock* headBB = llvm::BasicBlock::Create(ctx, "for_head", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx, "for_body", parentFn);
        llvm::BasicBlock* updateBB = llvm::BasicBlock::Create(ctx, "for_update", parentFn);
        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx, "for_exit", parentFn);

        builder->CreateBr(headBB);

        builder->SetInsertPoint(headBB);
        llvm::Value* condVal = evalCondition(module, condition);
        builder->CreateCondBr(condVal, bodyBB, exitBB);

        builder->SetInsertPoint(bodyBB);
        // continue jumps to the update block, not the head — so the update fires
        // before the next condition test. Matches Java semantics.
        module->pushLoopContext(updateBB, exitBB);
        if (body) body->generateCode(module);
        module->popLoopContext();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(updateBB);
        }

        builder->SetInsertPoint(updateBB);
        for (auto& u : update) {
            if (u) u->generateCode(module);
        }
        builder->CreateBr(headBB);

        builder->SetInsertPoint(exitBB);
        return nullptr;
    }

    void DoStatement::resolveTypes(CajetaModulePtr module) {
        if (body) body->resolveTypes(module);
        if (condition) condition->resolveTypes(module);
    }

    llvm::Value* DoStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx, "do_body", parentFn);
        llvm::BasicBlock* tailBB = llvm::BasicBlock::Create(ctx, "do_tail", parentFn);
        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx, "do_exit", parentFn);

        builder->CreateBr(bodyBB);

        builder->SetInsertPoint(bodyBB);
        // continue jumps to the condition test (the tail), which then branches back
        // to body or out. break exits.
        module->pushLoopContext(tailBB, exitBB);
        if (body) body->generateCode(module);
        module->popLoopContext();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(tailBB);
        }

        builder->SetInsertPoint(tailBB);
        llvm::Value* condVal = evalCondition(module, condition);
        builder->CreateCondBr(condVal, bodyBB, exitBB);

        builder->SetInsertPoint(exitBB);
        return nullptr;
    }

    void EnhancedForStatement::resolveTypes(CajetaModulePtr module) {
        if (iterableExpr) iterableExpr->resolveTypes(module);
        if (body) body->resolveTypes(module);
    }

    llvm::Value* EnhancedForStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (!iterableExpr || !elementType || elementName.empty()) {
            return nullptr;
        }

        // We only support array iterables today. Resolve the iterable's array type so
        // we know its element-LLVM shape (primitive vs pointer-slot).
        iterableExpr->resolveTypes(module);
        auto arrType = dynamic_pointer_cast<CajetaArray>(iterableExpr->getResolvedType());
        if (!arrType) {
            return nullptr;
        }
        llvm::Value* arrayVal = iterableExpr->generateCode(module);
        if (!arrayVal) return nullptr;
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(arrayVal)) {
            arrayVal = builder->CreateLoad(a->getAllocatedType(), a);
        }

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* hdrTy = arrType->getLlvmType();

        // Index counter — always int64 to match the array-header size field.
        llvm::AllocaInst* idxSlot = builder->CreateAlloca(i64Ty, nullptr, "fe_idx");
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxSlot);

        // Element binding alloca. For primitive element types we store the value
        // directly; for reference element types we store a `ptr` slot (matching the
        // shape LocalVariableDeclaration would have used).
        bool elemIsPrimitive = (elementType->getTypeFlags() & PRIMITIVE_FLAG) != 0;
        bool elemIsArray = dynamic_pointer_cast<CajetaArray>(elementType) != nullptr;
        llvm::Type* elemSlotTy = (elemIsPrimitive && !elemIsArray)
            ? elementType->getLlvmType()
            : llvm::PointerType::get(ctx, 0);
        llvm::AllocaInst* elemSlot = builder->CreateAlloca(elemSlotTy, nullptr, elementName);

        // Optional iterator alloca for the Cajeta-extended `for (int i, T x : arr)` form.
        llvm::AllocaInst* iterSlot = nullptr;
        if (iteratorType && !iteratorName.empty()) {
            iterSlot = builder->CreateAlloca(iteratorType->getLlvmType(), nullptr, iteratorName);
        }

        // Register both bindings in the current scope so the body's IdentifierExpression
        // lookups find them. StackField pre-seeded with the alloca we just created.
        auto scope = module->getScopeStack().peek();
        auto elemField = make_shared<StackField>(module, elementName, elementType);
        elemField->setAllocation(elemSlot);
        scope->putField(elemField);
        if (iterSlot) {
            auto iterField = make_shared<StackField>(module, iteratorName, iteratorType);
            iterField->setAllocation(iterSlot);
            scope->putField(iterField);
        }

        // Load size once at loop entry (Java semantics: iterating an array sees its
        // length at the start; explicit grows are not in scope today anyway).
        llvm::Value* sizePtr = builder->CreateStructGEP(hdrTy, arrayVal,
            CajetaArray::SIZE_FIELD_INDEX, "size");
        llvm::Value* sizeVal = builder->CreateLoad(i64Ty, sizePtr, "size_v");

        llvm::BasicBlock* headBB = llvm::BasicBlock::Create(ctx, "fe_head", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx, "fe_body", parentFn);
        llvm::BasicBlock* updateBB = llvm::BasicBlock::Create(ctx, "fe_update", parentFn);
        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx, "fe_exit", parentFn);
        builder->CreateBr(headBB);

        builder->SetInsertPoint(headBB);
        llvm::Value* idxVal = builder->CreateLoad(i64Ty, idxSlot, "i");
        llvm::Value* cond = builder->CreateICmpSLT(idxVal, sizeVal, "fe_cmp");
        builder->CreateCondBr(cond, bodyBB, exitBB);

        builder->SetInsertPoint(bodyBB);
        // GEP to data[idx]: header is `{ i64 size, [0 x T] data }`. Match the form
        // ArrayIndexExpression uses — single 3-index GEP on the header type so the
        // result matches the byte-offset that the runtime's heap layout assumes.
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Value* elemPtr = builder->CreateGEP(hdrTy, arrayVal,
            {llvm::ConstantInt::get(i64Ty, 0),
             llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
             idxVal}, "elem_ptr");
        // The element slot in the data region matches elemSlotTy (primitive value
        // for primitive arrays, pointer for reference arrays). Load and store.
        llvm::Value* elemVal = builder->CreateLoad(elemSlotTy, elemPtr, "elem");
        builder->CreateStore(elemVal, elemSlot);
        if (iterSlot) {
            // Store the current index into the iterator binding, narrowing if needed.
            llvm::Value* idxCast = idxVal;
            llvm::Type* itTy = iteratorType->getLlvmType();
            if (itTy != i64Ty && itTy->isIntegerTy()) {
                idxCast = builder->CreateIntCast(idxVal, itTy, /*isSigned=*/true);
            }
            builder->CreateStore(idxCast, iterSlot);
        }

        module->pushLoopContext(updateBB, exitBB);
        if (body) body->generateCode(module);
        module->popLoopContext();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(updateBB);
        }

        builder->SetInsertPoint(updateBB);
        llvm::Value* nextIdx = builder->CreateAdd(idxVal,
            llvm::ConstantInt::get(i64Ty, 1), "fe_next");
        builder->CreateStore(nextIdx, idxSlot);
        builder->CreateBr(headBB);

        builder->SetInsertPoint(exitBB);
        return nullptr;
    }

    void TryStatement::resolveTypes(CajetaModulePtr module) {
        if (tryBlock) tryBlock->resolveTypes(module);
        for (auto& c : catchClauses) {
            if (c.body) c.body->resolveTypes(module);
        }
        if (finallyBlock) finallyBlock->resolveTypes(module);
    }

    // setjmp/longjmp-based exception handling. The compiler emits a stack-allocated
    // frame blob, pushes it on the runtime's frame stack, then `setjmp`s to set a
    // recovery point. If body runs cleanly, we pop and proceed. If `throw` fires
    // inside the body, the runtime `longjmp`s back; setjmp returns non-zero and we
    // dispatch to the matching catch clause (currently first match wins; no real
    // type-based selection beyond a single clause).
    llvm::Value* TryStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

        // Resolve runtime helpers. If any is missing the runtime wasn't linked;
        // skip codegen and let the body run unprotected (loud failure on throw).
        llvm::Function* push = module->getRuntimeFunction("__cajeta_exc_push");
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        llvm::Function* getThrown = module->getRuntimeFunction("__cajeta_get_thrown");
        if (!push || !pop || !getThrown) {
            if (tryBlock) tryBlock->generateCode(module);
            return nullptr;
        }

        // Declare setjmp(ptr) -> i32 as an external. libc's setjmp matches this
        // signature on every supported target. Mark `returns_twice` so the optimizer
        // doesn't reorder around it.
        llvm::Module* lmod = module->getLlvmModule();
        llvm::Function* setjmpFn = lmod->getFunction("setjmp");
        if (!setjmpFn) {
            llvm::FunctionType* sjt = llvm::FunctionType::get(
                i32Ty, {llvm::PointerType::get(ctx, 0)}, false);
            setjmpFn = llvm::Function::Create(sjt, llvm::Function::ExternalLinkage,
                "setjmp", lmod);
            setjmpFn->addFnAttr(llvm::Attribute::ReturnsTwice);
        }

        // Allocate the frame at function entry to avoid setjmp/alloca interaction
        // issues (setjmp captures the SP; reusing the slot across nested try-blocks
        // could overwrite an outer frame, but distinct try-blocks each get their
        // own alloca which is enough for correctness).
        // 512 bytes covers jmp_buf + prev + thrown_value on common targets; the
        // exact size lives in __cajeta_exc_frame_size() in the runtime if we need
        // to tighten this later.
        constexpr unsigned frameBytes = 512;
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* framePtr = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, frameBytes));

        llvm::BasicBlock* tryBB = llvm::BasicBlock::Create(ctx, "try_body", parentFn);
        llvm::BasicBlock* catchBB = llvm::BasicBlock::Create(ctx, "try_catch", parentFn);
        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(ctx, "try_after", parentFn);

        // push + setjmp at the current block.
        builder->CreateCall(push, {framePtr});
        llvm::Value* sjResult = builder->CreateCall(setjmpFn, {framePtr});
        llvm::Value* threw = builder->CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        builder->CreateCondBr(threw, catchBB, tryBB);

        builder->SetInsertPoint(tryBB);
        if (tryBlock) tryBlock->generateCode(module);
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateCall(pop, {});
            builder->CreateBr(afterBB);
        }

        builder->SetInsertPoint(catchBB);
        // Read the thrown value BEFORE popping — pop unwinds the runtime stack and
        // the helper reads from the topmost frame.
        llvm::Value* thrownValPreserved = builder->CreateCall(getThrown, {});
        builder->CreateCall(pop, {});
        if (!catchClauses.empty()) {
            auto& c = catchClauses[0];  // single-clause for now
            llvm::Value* thrownVal = thrownValPreserved;
            // Bind the catch variable: alloca i64 (the thrown value), store, and
            // register a Field so the body can reference it by name.
            if (!c.variableName.empty()) {
                llvm::Value* slot = entryBuilder.CreateAlloca(i64Ty);
                builder->CreateStore(thrownVal, slot);
                auto& scope = module->getScopeStack();
                if (!scope.isEmpty()) {
                    auto type = c.type ? c.type : CajetaType::of("int64");
                    auto field = make_shared<HeapField>(module, c.variableName, type);
                    field->setAllocation(llvm::cast<llvm::AllocaInst>(slot));
                    scope.peek()->putField(field);
                }
            }
            if (c.body) c.body->generateCode(module);
        }
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(afterBB);
        }

        builder->SetInsertPoint(afterBB);
        // Run finally body unconditionally (after either path). Finally that throws
        // out of itself isn't supported here.
        if (finallyBlock) finallyBlock->generateCode(module);

        return nullptr;
    }

    llvm::Value* ResourceTryStatement::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    void SwitchStatement::resolveTypes(CajetaModulePtr module) {
        if (subject) subject->resolveTypes(module);
        for (auto& g : groups) {
            for (auto& v : g.caseValues) {
                if (v) v->resolveTypes(module);
            }
            for (auto& s : g.statements) {
                if (s) s->resolveTypes(module);
            }
        }
    }

    llvm::Value* SwitchStatement::generateCode(CajetaModulePtr module) {
        if (!subject) return nullptr;
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        // Evaluate the subject and coerce to a signed integer.
        llvm::Value* subjVal = subject->generateCode(module);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(subjVal)) {
            subjVal = builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (!subjVal || !subjVal->getType()->isIntegerTy()) {
            // Switch on non-integer subjects isn't supported yet.
            return nullptr;
        }
        unsigned subjBits = subjVal->getType()->getIntegerBitWidth();

        // Allocate a block per group and an after block. Default block defaults to
        // the after block if no `default:` was supplied.
        std::vector<llvm::BasicBlock*> groupBBs;
        groupBBs.reserve(groups.size());
        for (size_t i = 0; i < groups.size(); i++) {
            groupBBs.push_back(llvm::BasicBlock::Create(ctx, "switch_group", parentFn));
        }
        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(ctx, "switch_after", parentFn);
        llvm::BasicBlock* defaultBB = afterBB;
        for (size_t i = 0; i < groups.size(); i++) {
            if (groups[i].isDefault) {
                defaultBB = groupBBs[i];
                break;
            }
        }

        // Estimate the case count for SwitchInst's `numCases` hint.
        unsigned caseCount = 0;
        for (auto& g : groups) caseCount += static_cast<unsigned>(g.caseValues.size());

        llvm::SwitchInst* sw = builder->CreateSwitch(subjVal, defaultBB, caseCount);

        // Populate cases and emit each group's statements. Fall-through is implicit
        // — if a group's terminator hasn't been set after emitting its body, we
        // branch to the next group's block (or after, if last).
        module->pushLoopContext(afterBB, afterBB);  // `break` jumps out
        for (size_t i = 0; i < groups.size(); i++) {
            auto& g = groups[i];
            for (auto& valExpr : g.caseValues) {
                llvm::Value* v = valExpr->generateCode(module);
                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                    v = builder->CreateLoad(a->getAllocatedType(), a);
                }
                auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v);
                if (!ci) continue;  // case must be a constant integer
                // Match the subject's bit width so SwitchInst is well-typed.
                if (ci->getBitWidth() != subjBits) {
                    ci = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(
                        llvm::cast<llvm::IntegerType>(subjVal->getType()),
                        ci->getValue().sextOrTrunc(subjBits)));
                }
                sw->addCase(ci, groupBBs[i]);
            }
            builder->SetInsertPoint(groupBBs[i]);
            for (auto& s : g.statements) {
                if (s) s->generateCode(module);
            }
            if (!builder->GetInsertBlock()->getTerminator()) {
                llvm::BasicBlock* fallTo = (i + 1 < groups.size()) ? groupBBs[i + 1] : afterBB;
                builder->CreateBr(fallTo);
            }
        }
        module->popLoopContext();

        builder->SetInsertPoint(afterBB);
        return nullptr;
    }

    llvm::Value* SynchronizedStatement::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* ReturnStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (!expression) {
            // Fire drops before the value-less return.
            if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
            return builder->CreateRetVoid();
        }
        llvm::Value* val = expression->generateCode(module);
        // Load if the expression returned an l-value (alloca or array-slot GEP) — return
        // wants a value, not an address.
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(val)) {
            val = builder->CreateLoad(a->getAllocatedType(), a);
        } else if (auto idx = dynamic_pointer_cast<ArrayIndexExpression>(expression)) {
            // ArrayIndex returned a slot address. Element type determines the load size:
            // primitive elements load their own LLVM type; reference elements load `ptr`.
            CajetaTypePtr elemType = idx->getResolvedType();
            if (elemType) {
                llvm::Type* loadTy;
                if (dynamic_pointer_cast<CajetaArray>(elemType) ||
                    (elemType->getTypeFlags() & STRUCT_FLAG)) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else {
                    loadTy = elemType->getLlvmType();
                }
                if (loadTy) {
                    val = builder->CreateLoad(loadTy, val);
                }
            }
        }
        // Coerce to the enclosing function's return type. IntegerLiteralExpression picks
        // the smallest-fitting width and Cajeta otherwise lacks an upfront-promotion pass,
        // so cast/extend at the boundary so the return inst matches the function type.
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        llvm::Type* retTy = fn->getReturnType();
        llvm::Type* valTy = val->getType();
        if (valTy != retTy) {
            if (retTy->isIntegerTy() && valTy->isIntegerTy()) {
                val = builder->CreateIntCast(val, retTy, /*isSigned=*/true);
            } else if (retTy->isFloatingPointTy() && valTy->isFloatingPointTy()) {
                val = builder->CreateFPCast(val, retTy);
            } else if (retTy->isFloatingPointTy() && valTy->isIntegerTy()) {
                val = builder->CreateSIToFP(val, retTy);
            } else if (retTy->isIntegerTy() && valTy->isFloatingPointTy()) {
                val = builder->CreateFPToSI(val, retTy);
            }
            // Pointer / aggregate mismatches fall through; LLVM verifier will flag.
        }
        // Fire drops before the typed return so all owned locals are released.
        if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
        return builder->CreateRet(val);
    }

    void ThrowStatement::resolveTypes(CajetaModulePtr module) {
        if (expression) expression->resolveTypes(module);
    }

    llvm::Value* ThrowStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
        if (!throwFn) {
            return nullptr;
        }

        llvm::Value* val = expression ? expression->generateCode(module)
                                       : llvm::ConstantInt::get(i64Ty, 0);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(val)) {
            val = builder->CreateLoad(a->getAllocatedType(), a);
        }
        // The runtime takes i64; extend/narrow numeric throws to fit. References
        // (when we get them) would need a different path; punt for now.
        if (val && val->getType()->isIntegerTy()) {
            if (val->getType() != i64Ty) {
                val = builder->CreateIntCast(val, i64Ty, /*isSigned=*/true);
            }
        } else if (val && val->getType()->isPointerTy()) {
            val = builder->CreatePtrToInt(val, i64Ty);
        } else {
            val = llvm::ConstantInt::get(i64Ty, 0);
        }

        builder->CreateCall(throwFn, {val});
        // throw is noreturn from the user's POV; the runtime longjmps. Mark the
        // current block terminated with `unreachable` so following statements emit
        // into a fresh block (the harness will skip them).
        builder->CreateUnreachable();
        llvm::BasicBlock* dead = llvm::BasicBlock::Create(ctx, "after_throw",
            builder->GetInsertBlock()->getParent());
        builder->SetInsertPoint(dead);
        return nullptr;
    }

    llvm::Value* BreakStatement::generateCode(CajetaModulePtr module) {
        if (!module->hasLoopContext()) {
            return nullptr;
        }
        // After the unconditional br, start a fresh unreachable block so further
        // statements in the same Cajeta block (if any) emit into a valid container —
        // LLVM otherwise complains about adding to a terminated block.
        auto* builder = module->getBuilder();
        builder->CreateBr(module->currentLoopContext().breakTarget);
        llvm::BasicBlock* deadBB = llvm::BasicBlock::Create(
            *module->getLlvmContext(), "after_break",
            builder->GetInsertBlock()->getParent());
        builder->SetInsertPoint(deadBB);
        return nullptr;
    }

    llvm::Value* ContinueStatement::generateCode(CajetaModulePtr module) {
        if (!module->hasLoopContext()) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        builder->CreateBr(module->currentLoopContext().continueTarget);
        llvm::BasicBlock* deadBB = llvm::BasicBlock::Create(
            *module->getLlvmContext(), "after_continue",
            builder->GetInsertBlock()->getParent());
        builder->SetInsertPoint(deadBB);
        return nullptr;
    }

    llvm::Value* YieldStatement::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* IdentifierLabel::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* SemiStatement::generateCode(CajetaModulePtr module) {
        return nullptr;
    }
}