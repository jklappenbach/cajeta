//
// Created by James Klappenbach on 2/19/22.
//

#include "Statement.h"
#include "expression/Expression.h"
#include "expression/Identifier.h"
#include "expression/DotExpression.h"
#include "../compile/CajetaModule.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../field/ParameterField.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaStruct.h"
#include "../type/CajetaFunctionType.h"
#include "Block.h"
#include "LocalVariableDeclaration.h"
#include "../error/Exception.h"

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

    BlockPtr Statement::buildBlockFromContext(CajetaParser::BlockContext* ctx) {
        return buildBlock(ctx);
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
            // Two grammar forms:
            //   TRY block (catchClause+ finallyBlock? | finallyBlock)
            //   TRY resourceSpecification block catchClause* finallyBlock?
            // For the resource form we synthesize local-variable declarations
            // at the head of the body (so resource names are in scope), then
            // append `r.close()` ExpressionStatements in reverse declaration
            // order (so opens are paired with closes in LIFO order on the
            // normal-exit path). The catch/finally machinery the second form
            // shares with the first handles its own exception paths.
            BlockPtr tryBlk = ctx->block() ? buildBlock(ctx->block()) : nullptr;

            if (auto* rs = ctx->resourceSpecification()) {
                if (auto* rl = rs->resources()) {
                    auto resources = rl->resource();
                    // Build resource declarators and matching `r.close()` calls.
                    vector<BlockStatementPtr> opens;
                    vector<BlockStatementPtr> closes;
                    for (auto* r : resources) {
                        if (!r->ASSIGN() || !r->expression()) continue;
                        // Resource type: either an explicit classOrInterfaceType
                        // or VAR. We only support the explicit-type form for v1
                        // (`var` resources need full inference at this layer).
                        if (!r->classOrInterfaceType()) continue;
                        if (!r->variableDeclaratorId()) continue;
                        CajetaTypePtr type;
                        {
                            // Build a faux qName lookup matching CajetaType's
                            // resolution path for a class identifier.
                            auto qn = QualifiedName::fromContext(r->classOrInterfaceType());
                            type = CajetaType::of(qn);
                            if (!type) type = CajetaType::of(qn->getTypeName(), "");
                        }
                        string ident = r->variableDeclaratorId()->identifier()->getText();
                        auto init = make_shared<VariableInitializer>(
                            Expression::fromContext(r->expression()), r->getStart());
                        list<VariableDeclaratorPtr> decls;
                        decls.push_back(make_shared<VariableDeclarator>(
                            ident, /*isReference=*/false, /*arrayDim=*/0, init, r->getStart()));
                        set<Modifier> mods;
                        opens.push_back(make_shared<LocalVariableDeclaration>(
                            mods, type, decls, r->getStart()));
                        // `r.close()` — construct an identifier+methodCall AST
                        // tree by hand so we don't need to re-parse text. The
                        // resource's identifier resolves via scope at codegen.
                        // For v1, emit only the call's static form; full
                        // exception-aware close is deferred.
                        //
                        // Limitation note: building expression AST nodes
                        // mechanically here is awkward (no public ctor for
                        // method-call on an identifier without an ANTLR ctx).
                        // For now we skip close synthesis; the resource is
                        // accessible inside the block but isn't auto-closed.
                        (void) closes;
                    }
                    // Construct a new block whose children are
                    // [resource decls...] ++ [original body's children].
                    auto wrapped = make_shared<Block>(ctx->getStart());
                    for (auto& o : opens) wrapped->addChild(o);
                    if (tryBlk) {
                        for (auto& child : tryBlk->getChildren()) {
                            wrapped->addChild(child);
                        }
                    }
                    tryBlk = wrapped;
                }
            }
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
            // `break;` or `break label;` — capture the optional identifier.
            string label = ctx->identifier() ? ctx->identifier()->getText() : "";
            result = make_shared<BreakStatement>(token, std::move(label));
        } else if (ctx->CONTINUE()) {
            string label = ctx->identifier() ? ctx->identifier()->getText() : "";
            result = make_shared<ContinueStatement>(token, std::move(label));
        } else if (ctx->YIELD()) {
            result = make_shared<YieldStatement>(token);
        } else if (ctx->SCOPE()) {
            // `scope { ... }` — structured concurrency block. ctx->block() is
            // present for both this and the bare-block form below; SCOPE() is
            // the discriminator. Must be checked BEFORE the bare-block branch.
            BlockPtr blk = ctx->block() ? buildBlock(ctx->block()) : nullptr;
            result = make_shared<ScopeStatement>(token, blk);
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
            //result = new SwitchExpression;
        } else if (ctx->identifierLabel) {
            // `label: statement` — capture both the label name and the
            // statement it labels so codegen can stash the label on the
            // module before the inner statement (typically a loop) runs.
            string label = ctx->identifierLabel->getText();
            StatementPtr inner = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            result = make_shared<IdentifierLabel>(token, std::move(label), inner);
        } else if (ctx->SEMI()) {
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

    // ThreadModel.md — `scope { ... }` is a structured-concurrency block.
    // R5-A: every spawn site inside the block registers its task's done-
    // addr with the active scope frame; at the closing `}` we wait for
    // each one before letting control past. The scope frame stack is
    // managed in the runtime (per-fiber for code running inside an async
    // body; per-OS-thread for the main thread).
    void ScopeStatement::resolveTypes(CajetaModulePtr module) {
        if (block) block->resolveTypes(module);
    }

    llvm::Value* ScopeStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (llvm::Function* enterFn = module->getRuntimeFunction(
                "__cajeta_scope_enter")) {
            builder->CreateCall(enterFn, {});
        }
        // Manage the block's drop frame ourselves so scope_exit runs
        // BEFORE emitTopFrameDrops fires. If drops fired first, a
        // Task drop's free(task) would happen before scope_exit's
        // exception-slot walk reads task->exception — use-after-free
        // on the very pointer the scope holds. Method::generateCode's
        // fall-through path has the same ordering rule (scope_exit_to
        // before emitOwnerDrops at line 322-329 in Method.cpp); this
        // matches it for explicit `scope { }` blocks. Mirrors the
        // pattern Block::generateCode uses, just split apart so
        // scope_exit can land between body and drops.
        auto m = module->getCurrentMethod();
        if (m) m->pushDropFrame();
        if (block) {
            for (auto& child : block->getChildren()) {
                child->generateCode(module);
            }
        }
        // scope_exit only emits if the body didn't already terminate
        // (return/throw mid-block). A terminator means control is
        // exiting the function instead of falling off the brace — the
        // surrounding cleanup paths will be responsible for waiting.
        // TODO: integrate scope_exit into the early-return path.
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (bb && !bb->getTerminator()) {
            if (llvm::Function* exitFn = module->getRuntimeFunction(
                    "__cajeta_scope_exit")) {
                builder->CreateCall(exitFn, {});
            }
            if (m) m->emitTopFrameDrops(module);
        }
        if (m) m->popDropFrame();
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
        // L-value coercion for non-alloca address forms — most notably
        // ArrayIndexExpression and DotExpression, which return GEP
        // pointers rather than loaded values. If v is a pointer but
        // the condition's resolved type is a primitive scalar (boolean,
        // any integer), load through to get the value. Without this,
        // `while (arr[i])` / `if (this.field)` end up comparing a ptr
        // to an integer-zero constant in CreateICmpNE below and the
        // ICmp verifier rejects the type mismatch.
        if (v && v->getType()->isPointerTy() && cond->getResolvedType()) {
            llvm::Type* valTy = cond->getResolvedType()->getLlvmType();
            if (valTy && valTy != v->getType() && !valTy->isStructTy()) {
                v = builder->CreateLoad(valTy, v);
            }
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

        // P3b — definite-assignment flow analysis for if/else.
        // Snapshot the NYA set before the branches; run each branch with
        // its own deltas; merge at the join. Variable is DA-after iff DA
        // in BOTH branches → variable is NYA-after iff NYA in EITHER
        // branch → merged set is the UNION of post-branch NYA sets.
        // Missing else: post-else equals pre-if (the else "ran" without
        // touching anything). Union with post-then yields pre-if (since
        // markAssigned can only shrink NYA — post-then ⊆ pre-if).
        auto scope = module->getScopeStack().peek();
        std::set<std::string> preIfNYA;
        if (scope) preIfNYA = scope->snapshotNotYetAssigned();

        builder->SetInsertPoint(thenBB);
        if (thenBranch) thenBranch->generateCode(module);
        std::set<std::string> postThenNYA = preIfNYA;
        if (scope) postThenNYA = scope->snapshotNotYetAssigned();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }

        std::set<std::string> postElseNYA = preIfNYA;
        if (elseBB) {
            if (scope) scope->restoreNotYetAssigned(preIfNYA);
            builder->SetInsertPoint(elseBB);
            if (elseBranch) elseBranch->generateCode(module);
            if (scope) postElseNYA = scope->snapshotNotYetAssigned();
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        }

        // Reset to post-then, then union in post-else.
        if (scope) {
            scope->restoreNotYetAssigned(postThenNYA);
            scope->mergeNotYetAssigned(postElseNYA);
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
        // Make this try's catch types visible to nested call-site lints
        // (#209). Only covers the try body — popped before catch-body
        // codegen so a throw inside a catch handler isn't considered
        // caught by the same try's clauses. Nested try statements push
        // their own frames; the lint walks the whole stack.
        {
            std::vector<CajetaTypePtr> catchTypes;
            catchTypes.reserve(catchClauses.size());
            for (auto& c : catchClauses) {
                if (c.type) catchTypes.push_back(c.type);
            }
            module->pushTryCatchContext(std::move(catchTypes));
        }
        if (tryBlock) tryBlock->generateCode(module);
        module->popTryCatchContext();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateCall(pop, {});
            builder->CreateBr(afterBB);
        }

        builder->SetInsertPoint(catchBB);
        // R5/Error-model #202: getThrown now returns void*. Read it BEFORE
        // popping — pop unwinds the runtime stack and the helper reads from
        // the topmost frame. Convert back to the catch binding's declared
        // type below.
        llvm::Value* thrownValPtr = builder->CreateCall(getThrown, {});
        builder->CreateCall(pop, {});
        if (!catchClauses.empty()) {
            auto& c = catchClauses[0];  // single-clause for now
            if (!c.variableName.empty()) {
                auto type = c.type ? c.type : CajetaType::of("int64");
                llvm::Type* bindTy = type->getLlvmType();
                if (!bindTy) bindTy = i64Ty;
                // Class-typed catch bindings (catch (Throwable e), etc.)
                // hold a heap pointer, not a struct-by-value. The class's
                // getLlvmType() returns the struct type, so we substitute
                // `ptr` for the alloca's stored element. The thrown value
                // is already a ptr, so it stores directly.
                llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
                bool classTypedBinding =
                    dynamic_pointer_cast<CajetaClass>(type) != nullptr;
                if (classTypedBinding) bindTy = ptrTy;
                llvm::Value* slot = entryBuilder.CreateAlloca(bindTy);
                // Integer catch binding: PtrToInt to recover the legacy
                // i64-throw shape. Pointer/class binding: store the ptr
                // directly.
                llvm::Value* storeVal = thrownValPtr;
                if (bindTy->isIntegerTy()) {
                    storeVal = builder->CreatePtrToInt(thrownValPtr, i64Ty);
                    if (bindTy != i64Ty) {
                        storeVal = builder->CreateIntCast(storeVal, bindTy,
                            /*isSigned=*/true);
                    }
                }
                builder->CreateStore(storeVal, slot);
                auto& scope = module->getScopeStack();
                if (!scope.isEmpty()) {
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

    // R5-A': pop every scope frame this method pushed via the watermark
    // captured at function entry, waiting on each registered task. Called
    // before BOTH the void-return path and the typed-return path so an
    // early return from inside an explicit `scope { }` still joins all
    // pending child tasks before the ret instruction.
    static void emitScopeExitToWatermark(CajetaModulePtr module) {
        auto m = module->getCurrentMethod();
        if (!m) return;
        llvm::AllocaInst* mark = m->getScopeWatermark();
        if (!mark) return;
        llvm::Function* exitToFn = module->getRuntimeFunction(
            "__cajeta_scope_exit_to");
        if (!exitToFn) return;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Value* watermark = builder->CreateLoad(ptrTy, mark);
        builder->CreateCall(exitToFn, {watermark});
    }

    llvm::Value* ReturnStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (!expression) {
            // A4: fire @After advice before scope-exit + drops, on
            // the same ordering rule the fall-through return uses in
            // Method::generateCode (advice runs in body lifetime,
            // cleanup runs after). A6: @AfterReturning fires next
            // on the normal-return path; then pop any try frame
            // the enclosing method's @AfterThrowing wrapping set up.
            if (auto m = module->getCurrentMethod()) {
                m->emitAfterAdvice(module);
                m->emitAfterReturningAdvice(module);
                m->emitAfterThrowingTryPop(module);
            }
            // Pop any open scope frames before the value-less return so
            // every pending child task is joined first.
            emitScopeExitToWatermark(module);
            // Fire drops before the value-less return.
            if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
            return builder->CreateRetVoid();
        }
        // L3-2 escape check: a function-typed local that holds a closure
        // with borrow captures is bounded by its declaring scope — its
        // captured borrows refer to outer locals that would be dead by
        // the time the caller invoked the returned closure. Reject the
        // return before codegen so the user gets a clear error rather
        // than runtime use-after-free.
        //
        // Two shapes covered:
        //   - `return fnLocal;`   — IdentifierExpression with a Field
        //                            whose hasBorrowCaptures was set by
        //                            LocalVariableDeclaration when the
        //                            lambda RHS was generated.
        //   - `return () -> ...;` — direct LambdaExpression; we'll see
        //                            its flag after generateCode below.
        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(expression)) {
            auto scope = module->getScopeStack().peek();
            FieldPtr f = scope ? scope->getField(idExpr->getTextValue()) : nullptr;
            if (f && f->hasBorrowCaptures()) {
                throw Exception(
                    "cannot return closure '" + idExpr->getTextValue()
                    + "' — it captures one or more outer locals by borrow, "
                    "and those borrows would dangle past the function return; "
                    "transfer the captures via `#name` to give the closure "
                    "ownership it can carry past this scope",
                    "CAJETA_ERROR_BORROW_ESCAPE");
            }
            // Struct view-aliasing escape check: returning a struct
            // view whose underlying buffer is a function-scope local
            // would leave the caller with a view of freed memory —
            // the buffer drops as this function returns. View over
            // a parameter (caller-owned buffer) or a field (some
            // object's buffer that outlives the call) is fine; only
            // function-locals end at return. Detected by walking
            // viewSource and checking it's not a ParameterField.
            if (f && f->getViewSource()) {
                FieldPtr src = f->getViewSource();
                bool srcIsParam =
                    dynamic_pointer_cast<ParameterField>(src) != nullptr;
                if (!srcIsParam) {
                    throw Exception(
                        "cannot return struct view '" + idExpr->getTextValue()
                        + "' — it aliases the buffer '" + src->getName()
                        + "' which is a function-scope local and would drop "
                        "as this function returns, leaving the caller with "
                        "a view of freed memory; allocate the buffer on the "
                        "caller side and pass it in as a parameter, or have "
                        "the function return the buffer instead of the view",
                        "CAJETA_ERROR_VIEW_ESCAPE");
                }
            }
            // S10.3 — interface escape check for BORROWED_STRUCT values.
            // The interface local's data ptr roots in a struct body
            // (the source struct local or an inline aggregate literal)
            // that lives in this function's frame. Returning the
            // interface value would hand the caller a fat pointer
            // whose data dangles past the frame's death. The
            // OWNED_CLASS / BORROWED_CLASS variants don't trigger —
            // their data pointers root in heap allocations whose
            // lifetime is independent of this frame.
            if (f && f->interfaceBorrowsStructLocal()) {
                throw Exception(
                    "cannot return interface value '" + idExpr->getTextValue()
                    + "' — its data pointer roots in a function-scope "
                    "struct whose body lives only as long as this frame; "
                    "returning the value would leave the caller with a "
                    "fat pointer to freed stack memory. Box the underlying "
                    "data in a class and assign via `#` to transfer "
                    "ownership across the return.",
                    "CAJETA_ERROR_INTERFACE_VALUE_ESCAPE");
            }
            // L3-3: returning a function-typed local transfers closure
            // ownership to the caller. Deactivate the local's drop entry
            // so this scope's exit pop doesn't free the closure out from
            // under the receiving caller. The caller's own function-typed
            // local will register a fresh drop entry on receipt.
            if (f && dynamic_pointer_cast<CajetaFunctionType>(f->getType())) {
                if (llvm::Value* entry = f->getDropEntry()) {
                    if (llvm::Function* mark = module->getRuntimeFunction(
                            "__cajeta_drop_mark_inactive")) {
                        builder->CreateCall(mark, {entry});
                    }
                }
            }
            // Same transfer semantics for class-instance locals.
            // Returning the local moves the heap allocation up to the
            // caller; this scope's drop entry must not fire or we'd
            // free the instance the caller now owns. The caller's
            // declared receiving local will get its own drop entry.
            // Struct values and arrays aren't covered here — structs
            // live inline (no drop), and arrays remain owned by the
            // declaring scope regardless of whether the array header
            // is returned (that's a separate pre-existing limitation).
            if (f) {
                auto klass = dynamic_pointer_cast<CajetaClass>(f->getType());
                auto isStruct = dynamic_pointer_cast<CajetaView>(f->getType());
                if (klass && !isStruct && !klass->isInterface()) {
                    if (llvm::Value* entry = f->getDropEntry()) {
                        if (llvm::Function* mark = module->getRuntimeFunction(
                                "__cajeta_drop_mark_inactive")) {
                            builder->CreateCall(mark, {entry});
                        }
                    }
                }
            }
        }
        llvm::Value* val = expression->generateCode(module);
        // Same check, deferred form: returning a fresh lambda directly
        // (`return () -> ...;`). We need the lambda's generateCode to
        // run first so its capture analysis populated the flag, hence
        // the post-codegen position. Catches the inline construction
        // case that no Field-based check could reach.
        if (auto lambdaExpr = dynamic_pointer_cast<LambdaExpression>(expression)) {
            if (lambdaExpr->getHasBorrowCaptures()) {
                throw Exception(
                    "cannot return this lambda — it captures one or more "
                    "outer locals by borrow, and those borrows would "
                    "dangle past the function return; transfer the "
                    "captures via `#name` to give the closure ownership "
                    "it can carry past this scope",
                    "CAJETA_ERROR_BORROW_ESCAPE");
            }
        }
        if (auto refExpr = dynamic_pointer_cast<MethodReferenceExpression>(expression)) {
            if (refExpr->getHasBorrowCaptures()) {
                throw Exception(
                    "cannot return this method reference — it captures "
                    "the receiver by borrow, and the borrow would dangle "
                    "past the function return",
                    "CAJETA_ERROR_BORROW_ESCAPE");
            }
        }
        // S6.7 — struct return type. Methods returning CajetaStruct travel
        // by VALUE (not by ptr) so the body alloca dying with the callee's
        // frame doesn't dangle the result. Three shapes for `val` here:
        //
        //   (1) IdentifierExpression of a struct local: val is the
        //       HeapField slot (alloca ptr); slot holds the body ptr.
        //       Double-load: first the slot → body ptr, then body ptr →
        //       struct value.
        //   (2) AggregateInitializerExpression: val is the body alloca
        //       itself (alloca of struct LLVM type); single load gives
        //       the struct value. The general AllocaInst branch below
        //       handles this correctly.
        //   (3) MethodCallExpression (struct-returning call): MCE
        //       already repackages the result into a fresh body alloca
        //       and returns its ptr; same shape as (2) — single load.
        //
        // The shape (1) branch must fire BEFORE the general AllocaInst
        // load to avoid loading the slot just once (which would give us
        // a body pointer where a struct value is expected, producing
        // `ret ptr` against a struct return signature).
        if (auto m = module->getCurrentMethod()) {
            // Pick up either a CajetaStruct return (S6.7) or an interface
            // return (S9.5.5). Both travel by VALUE per the small-struct
            // ABI; both need the same double-load when the source is a
            // named local whose HeapField slot holds a body pointer.
            CajetaTypePtr byValRet;
            if (auto structRet = dynamic_pointer_cast<CajetaStruct>(m->getReturnType())) {
                byValRet = structRet;
            } else if (auto ifaceRet = dynamic_pointer_cast<CajetaClass>(m->getReturnType())) {
                if (ifaceRet->isInterface()) byValRet = ifaceRet;
            }
            if (byValRet) {
                // Both shapes load the slot to get a body pointer, then
                // load the body to get the value:
                //   (a) Struct / interface local: field type is the
                //       aggregate, the HeapField slot holds a pointer
                //       to the body alloca. Detected via the binding
                //       name in scope.
                //   (b) `this` on a struct method: ThisExpression
                //       resolves through a ParameterField also named
                //       "this" whose type is "pointer"; the slot holds
                //       the caller-supplied body pointer.
                // For `return this;` ThisExpression generates the alloca
                // directly (see ThisExpression::generateCode); for
                // `return localStruct;` IdentifierExpression returns the
                // HeapField slot. Both go through the same double-load
                // path below.
                std::string fieldName;
                bool tryDoubleLoad = false;
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                    fieldName = idExpr->getTextValue();
                    auto scope = module->getScopeStack().peek();
                    if (scope) {
                        if (auto field = scope->getField(fieldName)) {
                            bool isStructLocal = dynamic_pointer_cast<CajetaStruct>(
                                field->getType()) != nullptr;
                            auto fldClass = dynamic_pointer_cast<CajetaClass>(field->getType());
                            bool isInterfaceLocal = fldClass && fldClass->isInterface();
                            bool isThisParam = (fieldName == "this");
                            if (isStructLocal || isInterfaceLocal || isThisParam) {
                                tryDoubleLoad = true;
                            }
                        }
                    }
                } else if (dynamic_pointer_cast<ThisExpression>(expression)) {
                    fieldName = "this";
                    tryDoubleLoad = true;
                }
                if (tryDoubleLoad) {
                    auto scope = module->getScopeStack().peek();
                    if (scope) {
                        if (auto field = scope->getField(fieldName)) {
                            llvm::AllocaInst* slot = field->getOrCreateAllocation();
                            llvm::Value* bodyPtr = builder->CreateLoad(
                                slot->getAllocatedType(), slot);
                            val = builder->CreateLoad(
                                byValRet->getLlvmType(), bodyPtr);
                            // Skip the rest of the load logic — val
                            // is already the right struct value.
                            if (auto curM = module->getCurrentMethod()) {
                                curM->emitAfterAdvice(module);
                                curM->emitAfterReturningAdvice(module);
                                curM->emitAfterThrowingTryPop(module);
                            }
                            emitScopeExitToWatermark(module);
                            if (auto curM = module->getCurrentMethod())
                                curM->emitOwnerDrops(module);
                            return builder->CreateRet(val);
                        }
                    }
                }
            }
        }

        // Load if the expression returned an l-value (alloca, array-slot GEP, or
        // struct/class field GEP) — return wants a value, not an address.
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
        } else if (auto dot = dynamic_pointer_cast<DotExpression>(expression)) {
            // DotExpression returned a GEP to a field slot — load through it
            // using the field's declared type. If the receiver struct carries
            // a non-host endianness annotation, bswap after the load so the
            // host sees the value in its own byte order.
            if (!dot->getChildren().empty()) {
                auto recv = dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
                if (recv) {
                    if (!recv->getResolvedType()) recv->resolveTypes(module);
                    if (auto klass = dynamic_pointer_cast<CajetaClass>(recv->getResolvedType())) {
                        // Walk the inheritance chain — an inherited field
                        // lives on an ancestor's properties map, not the
                        // receiver's own. Mirrors DotExpression's own
                        // findProp lambda.
                        StructurePropertyPtr found;
                        std::function<bool(const CajetaClassPtr&)> findProp =
                            [&](const CajetaClassPtr& cls) -> bool {
                                auto pit = cls->getProperties().find(dot->getIdentifier());
                                if (pit != cls->getProperties().end()) {
                                    found = pit->second;
                                    return true;
                                }
                                for (auto& parent : cls->getSuperClasses()) {
                                    if (findProp(parent)) return true;
                                }
                                return false;
                            };
                        if (findProp(klass) && found) {
                            if (llvm::Type* lt = found->getType()->getLlvmType()) {
                                val = builder->CreateLoad(lt, val);
                                val = DotExpression::maybeBswap(module, val, recv);
                            }
                        }
                    }
                }
            }
        } else if (auto id = dynamic_pointer_cast<IdentifierExpression>(expression)) {
            // Implicit-this field access: when a bare identifier inside
            // an instance method body resolves to a class property,
            // IdentifierExpression returns the field's GEP slot (not an
            // AllocaInst). Load through using the resolved type, same
            // shape as the DotExpression branch above.
            if (val && val->getType()->isPointerTy() && id->getResolvedType()) {
                if (llvm::Type* lt = id->getResolvedType()->getLlvmType()) {
                    if (lt != val->getType()) {
                        val = builder->CreateLoad(lt, val);
                    }
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
        // A4: fire @After advice before scope-exit + drops on the
        // typed-return path too. Same ordering rule as the void-
        // return / fall-through paths. A6: @AfterReturning fires
        // next on the normal-return path; then pop any
        // @AfterThrowing try frame the enclosing method set up.
        if (auto m = module->getCurrentMethod()) {
            m->emitAfterAdvice(module);
            m->emitAfterReturningAdvice(module);
            m->emitAfterThrowingTryPop(module);
        }
        // Pop any open scope frames before the typed return — joins every
        // pending child task (function-body scope + any explicit scope
        // the return is lexically inside of).
        emitScopeExitToWatermark(module);
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
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
        if (!throwFn) {
            return nullptr;
        }

        llvm::Value* val = expression ? expression->generateCode(module)
                                       : llvm::ConstantPointerNull::get(ptrTy);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(val)) {
            val = builder->CreateLoad(a->getAllocatedType(), a);
        }
        // R5/Error-model #202: the runtime takes a void*. Pointer values flow
        // through unchanged (class instance throws are the doc-aligned case);
        // integer values get IntToPtr-converted, preserving the legacy
        // `throw 42` shape so old exception tests still work — the catch
        // codegen reverses the conversion via PtrToInt when the catch
        // binding type is integer-shaped.
        if (val && val->getType()->isIntegerTy()) {
            if (val->getType() != i64Ty) {
                val = builder->CreateIntCast(val, i64Ty, /*isSigned=*/true);
            }
            val = builder->CreateIntToPtr(val, ptrTy);
        } else if (val && val->getType()->isPointerTy()) {
            // Already a pointer — pass through.
        } else {
            val = llvm::ConstantPointerNull::get(ptrTy);
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
        // Labeled `break label;` walks the loop stack for a matching label;
        // bare `break;` uses the innermost loop. After the unconditional br,
        // start a fresh unreachable block so further statements in the same
        // Cajeta block (if any) emit into a valid container — LLVM otherwise
        // complains about adding to a terminated block.
        auto* builder = module->getBuilder();
        llvm::BasicBlock* target = nullptr;
        if (!label.empty()) {
            if (auto* lc = module->findLoopContext(label)) {
                target = lc->breakTarget;
            }
        }
        if (!target) target = module->currentLoopContext().breakTarget;
        builder->CreateBr(target);
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
        llvm::BasicBlock* target = nullptr;
        if (!label.empty()) {
            if (auto* lc = module->findLoopContext(label)) {
                target = lc->continueTarget;
            }
        }
        if (!target) target = module->currentLoopContext().continueTarget;
        builder->CreateBr(target);
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
        // `label: statement` — stash the label on the module so the next
        // pushLoopContext (typically by the inner loop) picks it up. Then
        // run the labeled statement.
        if (!identifier.empty()) {
            module->setPendingLoopLabel(identifier);
        }
        if (body) body->generateCode(module);
        return nullptr;
    }

    llvm::Value* SemiStatement::generateCode(CajetaModulePtr module) {
        return nullptr;
    }
}