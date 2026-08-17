//
// Created by James Klappenbach on 2/19/22.
//

#include "Statement.h"
#include "expression/Expression.h"
#include "expression/MethodCallExpression.h"
#include "expression/CallExpression.h"
#include "expression/Identifier.h"
#include "expression/DotExpression.h"
#include "expression/NewExpression.h"
#include "expression/AggregateInitializerExpression.h"
#include "../compile/CajetaModule.h"
#include "../compile/ScriptUnitSynthesis.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "cajeta/dbg/LineInfoCodegen.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../field/ParameterField.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaFunctionType.h"
#include "Block.h"
#include "LocalVariableDeclaration.h"
#include "../error/Exception.h"
#include "../error/Diagnostics.h"
#include "cajeta/ownership/ReturnTitleAudit.h"

/**
 * statement
    : blockLabel=block
    | ASSERT expression (':' expression)? ';'
    | IF parExpression statement (ELSE statement)?
    | FOR '(' forControl ')' statement
    | WHILE parExpression statement
    | DO statement WHILE parExpression ';'
    | TRY block (catchClause+ finallyBlock? | finallyBlock)
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
    // Build an Initializer from a `variableInitializer` context, handling both
    // alternatives of `variableInitializer : arrayInitializer | expression`.
    // Recursive so nested array literals (`{{...}, {...}}`) and array literals
    // of method references (`{ A::f, B::g }` — a device dispatch table) build
    // correctly. Mirrors CajetaLlvmVisitor::visitVariableInitializer; the
    // Statement path is module-free (kernel bodies, FOR-decls) so it can't use
    // the visitor. Returns nullptr for a malformed/empty initializer.
    static InitializerPtr buildVariableInitializer(
            CajetaParser::VariableInitializerContext* viCtx) {
        if (!viCtx) return nullptr;
        if (auto* aiCtx = viCtx->arrayInitializer()) {
            list<InitializerPtr> elements;
            for (auto* childVi : aiCtx->variableInitializer()) {
                elements.push_back(buildVariableInitializer(childVi));
            }
            return make_shared<ArrayInitializer>(elements, aiCtx->getStart());
        }
        if (viCtx->expression()) {
            return make_shared<VariableInitializer>(
                Expression::fromContext(viCtx->expression()), viCtx->getStart());
        }
        return nullptr;
    }

    static shared_ptr<LocalVariableDeclaration>
    buildLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* lvdCtx) {
        if (!lvdCtx) return nullptr;
        // title-tracking §8.1 (plan 7.1.3) — `#Type` local declarations are
        // retired; mirrors visitLocalVariableDeclaration.
        if (lvdCtx->REFERENCE() != nullptr) {
            throw Exception(
                "`#` on a local declaration's type is retired: a local's role "
                "comes from its initializer under title-tracking "
                "(specs/title-tracking-spec.md §8.1) — drop the `#` from the "
                "declaration",
                "CAJETA_ERROR_TYPE_TRANSFER_RETIRED");
        }
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
                InitializerPtr initializer =
                    buildVariableInitializer(vdCtx->variableInitializer());
                // title-stores §2.2.3 — `T x #= v`: wrap the initializer
                // expression (mirrors visitVariableDeclarator).
                if (vdCtx->SHARP_ASSIGN() != nullptr && initializer != nullptr) {
                    // `T x #= #v` — the transfer spelled twice. Same rule as the
                    // assignment form in Expression::fromContext: `#=` IS the
                    // transfer, so a second `#` on the initializer adds nothing
                    // and reads as a claim that does not exist. Checked
                    // syntactically on the initializer's expression context.
                    if (auto* viCtx = vdCtx->variableInitializer()) {
                        if (auto* eCtx = viCtx->expression()) {
                            // `T x #= #v` — the transfer spelled twice.
                            // Technically valid (`#=` already carries the
                            // source's mode), so it warns rather than rejects.
                            // Flagged here; reported from
                            // MoveExpression::generateCode.
                            if (cajeta::cajetaRhsCarriesRedundantSharp(eCtx)) {
                                if (auto vi0 = dynamic_pointer_cast<
                                        VariableInitializer>(initializer)) {
                                    if (!vi0->getChildren().empty()) {
                                        if (auto redMv = dynamic_pointer_cast<
                                                MoveExpression>(
                                                    vi0->getChildren()[0])) {
                                            redMv->setRedundantSharp(true);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (auto vi = dynamic_pointer_cast<VariableInitializer>(initializer)) {
                        auto& kids = vi->getChildren();
                        if (!kids.empty()) {
                            if (auto inner = dynamic_pointer_cast<Expression>(kids[0])) {
                                auto mv = make_shared<MoveExpression>(
                                    vdCtx->variableInitializer()->getStart());
                                mv->setSharpStore(true);
                                mv->addChild(inner);
                                initializer = make_shared<VariableInitializer>(
                                    mv, vdCtx->variableInitializer()->getStart());
                            }
                        }
                    }
                } else if (initializer != nullptr) {
                    // title-stores §2.3 Phase 2 (plan 7.2.2) — legacy `T x = #v`.
                    markLegacyTransferAssign(initializer);
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
            // Single grammar form:
            //   TRY block (catchClause+ finallyBlock? | finallyBlock)
            //
            // Try-with-resources was removed 2026-05-20 — destructors
            // fire deterministically at the closing `}` of the
            // resource's declaring block, so the wrapped-declaration
            // dance was strictly redundant with `{ R r = …; … }`.
            // See docs/MemoryModel.md § Destructors and
            // docs/specification/io/file/Readme.md § Design tenets.
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
                        // Parse-time pre-seed only — a bare registry hit can
                        // miss (sibling-file/archive class) or land a
                        // same-named shadow. resolveTypes re-resolves the
                        // retained NAME through the scoped tier discipline.
                        c.typeNameText = typeName;
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
            // `return #= x` — the mode-carrying form. SHARP_ASSIGN is its own
            // token, so this never reaches the `REFERENCE expression` path.
            result = make_shared<ReturnStatement>(
                token, returnExpr, ctx->SHARP_ASSIGN() != nullptr);
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
        // jupyter-kernel U3 — take the unit-result mark BEFORE generating
        // anything. Our own expression may contain nested expression
        // statements (a block-form lambda body), and the mark names exactly
        // one statement; reading it later would let the innermost one claim
        // it. Cleared by the take, so nothing downstream sees a stale mark.
        bool unitResult = module->takeScriptResultPending();
        // A statement-position `spawn f(...);` never binds its Task —
        // mark it so the lowering hands ownership to the runtime scope
        // frame instead of a per-site drop entry (see SpawnExpression::
        // discardedMode). Detach arrives via DetachExpression and never
        // takes this path.
        if (auto sp = dynamic_pointer_cast<SpawnExpression>(expression)) {
            sp->setDiscardedMode(true);
        }
        // discarded-wildcard-next (docs/LintRules.md). When an
        // element-producing call on a wildcard receiver appears in
        // statement position — the returned `#Optional<?>` is allocated
        // and immediately discarded — both the heap allocation and the
        // wildcard-dispatch indirect call are wasted. Same enclosing-
        // context exclusions as the loop-site wildcard lints (proxy
        // class and method-template bodies) avoid the false-positive
        // window described in MethodCallExpression.cpp.
        if (auto mce = dynamic_pointer_cast<MethodCallExpression>(expression)) {
            const string& name = mce->getMethodCallName();
            bool isElementProducing = (name == "next" || name == "get");
            auto currentMethod = module->getCurrentMethod();
            bool enclosingIsWildcardProxy = currentMethod
                && currentMethod->getParent()
                && currentMethod->getParent()->isWildcardInstantiation();
            bool enclosingIsMethodTemplate = currentMethod
                && (currentMethod->isMethodTemplate()
                    || currentMethod->isMethodTemplateInstantiation());
            if (isElementProducing && currentMethod
                    && !enclosingIsWildcardProxy
                    && !enclosingIsMethodTemplate
                    && !currentMethod->isLintSuppressed(
                        "discarded-wildcard-next")) {
                auto& children = mce->getChildren();
                if (!children.empty()) {
                    auto recv = dynamic_pointer_cast<Expression>(children[0]);
                    if (recv) {
                        if (!recv->getResolvedType()) recv->resolveTypes(module);
                        auto recvClass = dynamic_pointer_cast<CajetaClass>(
                            recv->getResolvedType());
                        if (recvClass && recvClass->isWildcardInstantiation()) {
                            std::cerr << "warning: [discarded-wildcard-next] "
                                << "call to '" << name
                                << "' on wildcard-typed receiver "
                                << recvClass->toCanonical()
                                << " in statement position in "
                                << currentMethod->getName()
                                << " — the result (a heap Optional<?>) is "
                                << "discarded; remove the call if you don't "
                                << "need its value, or bind the result and "
                                << "act on it. Suppress with "
                                << "@SuppressLint(\"discarded-wildcard-next\")."
                                << std::endl;
                        }
                    }
                }
            }
        }
        llvm::Value* stmtVal = expression
            ? expression->generateCode(module) : nullptr;
        // Render the cell's result before the discard handling below: a
        // flagged class-pointer return is about to be dropped, and reading it
        // afterwards would render freed memory.
        if (unitResult) emitScriptUnitResult(module, expression, stmtVal);
        // title-tracking 6.2.2 — DISCARDED flagged return. A class-pointer
        // call result in statement position is a temp whose title (if the
        // callee surrendered one — a flagged `remove`, a `#T` return) has
        // nowhere to land: before this it silently LEAKED, and container
        // discard sites (`cache.entries.remove(k);`) relied on the callee
        // dropping internally. Read the paired flag and conditionally drop.
        // Legacy plain returns store flag 0 — those discards stay no-ops —
        // and `return this`-style chaining APIs are flag-0 by construction.
        if (stmtVal && stmtVal->getType()->isPointerTy()) {
            if (auto mceD = dynamic_pointer_cast<MethodCallExpression>(
                    expression)) {
                MethodPtr rm = mceD->getResolvedMethod();
                // Only when the callee actually STORES the flag: a raw-IR
                // synthesized class-pointer body (getter `return this.f`,
                // builder-setter `return this`, factory provider, ...) has
                // emitsReturnFlag()==false and never touches the TLS, so the
                // flag here is a STALE value from a prior flag-emitting call —
                // reading it drops a value the callee never surrendered
                // (double-free / UAF). Mirrors the ReturnStatement guard below.
                if (rm && rm->returnsClassPointer() && rm->emitsReturnFlag()) {
                    auto* b = module->getBuilder();
                    llvm::Function* getFlag = module->getRuntimeFunction(
                        "__cajeta_return_flag_get");
                    // Drop fn by the STATIC return type (dropValue's
                    // dispatch): virtual_drop on an ARRAY header reads its
                    // first word as a vtable — a discarded '#T[]' return
                    // was dropped through garbage. Shared-capable values
                    // (String) take the mode-aware string drop.
                    CajetaTypePtr drt = rm->getReturnType();
                    const char* dropName = "__cajeta_class_virtual_drop";
                    if (dynamic_pointer_cast<CajetaArray>(drt)) {
                        dropName = "__cajeta_free_array";
                    } else if (auto drc =
                                   dynamic_pointer_cast<CajetaClass>(drt)) {
                        if (drc->isSharedCapableValue()) {
                            dropName = "__cajeta_string_drop";
                        }
                    }
                    llvm::Function* dropFn = module->getRuntimeFunction(
                        dropName);
                    if (getFlag && dropFn) {
                        llvm::Value* fl = b->CreateCall(getFlag, {},
                            "discard_flag");
                        llvm::Value* owned = b->CreateICmpNE(fl,
                            b->getInt64(0), "discard_owned");
                        auto& dctx = *module->getLlvmContext();
                        llvm::Function* fn =
                            b->GetInsertBlock()->getParent();
                        llvm::BasicBlock* dropBB =
                            llvm::BasicBlock::Create(dctx,
                                "discard_drop", fn);
                        llvm::BasicBlock* contBB =
                            llvm::BasicBlock::Create(dctx,
                                "discard_cont", fn);
                        b->CreateCondBr(owned, dropBB, contBB);
                        b->SetInsertPoint(dropBB);
                        b->CreateCall(dropFn, {stmtVal});
                        b->CreateBr(contBB);
                        b->SetInsertPoint(contBB);
                    }
                }
            }
        }
        return stmtVal;
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

    // docs/specification/concurrent/Concurrency.md — `scope { ... }` is a structured-concurrency block.
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
        if (bb && !bb->hasTerminator()) {
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
            // A genuinely absent condition — `for (;;)` — is an infinite loop.
            return llvm::ConstantInt::getTrue(*module->getLlvmContext());
        }
        llvm::Value* v = cond->generateCode(module);
        if (!v) {
            // Written but unresolvable. Falling through would branch on a null
            // and crash the compiler, or (worse) fold to a constant-false test.
            throw locatedException(
                cond->getSourceLine(), cond->getSourceColumn() + 1,
                "condition did not resolve to a value (a sub-expression produced"
                " nothing — e.g. a method or member that does not exist on the"
                " receiver's type)",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        // A `void` call is PRESENT but valueless, so the null guard above misses
        // it and the i1 coercion below asserts inside LLVM (1.2.3).
        if (cond->getResolvedType()
                && cond->getResolvedType()->toCanonical() == "void") {
            throw locatedException(
                cond->getSourceLine(), cond->getSourceColumn() + 1,
                "condition is a 'void' expression, which has no value to test",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
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
        // title-tracking §3.1.5 — arm-isolated move state. Codegen is
        // sequential but the arms are exclusive: the else arm must see the
        // PRE-IF moved state, and the join takes the union (a Block-level
        // return/throw retraction already emptied a terminating arm's
        // slice, so terminated arms contribute nothing).
        size_t preIfMoves = scope ? scope->moveLogSize() : 0;

        builder->SetInsertPoint(thenBB);
        if (thenBranch) thenBranch->generateCode(module);
        std::set<std::string> postThenNYA = preIfNYA;
        if (scope) postThenNYA = scope->snapshotNotYetAssigned();
        if (!builder->GetInsertBlock()->hasTerminator()) {
            builder->CreateBr(mergeBB);
        }

        std::set<std::string> postElseNYA = preIfNYA;
        if (elseBB) {
            std::vector<Scope::MoveMark> thenMoves;
            if (scope) {
                thenMoves = scope->snapshotMovesSince(preIfMoves);
                scope->retractMovesSince(preIfMoves);
            }
            if (scope) scope->restoreNotYetAssigned(preIfNYA);
            builder->SetInsertPoint(elseBB);
            if (elseBranch) elseBranch->generateCode(module);
            if (scope) postElseNYA = scope->snapshotNotYetAssigned();
            if (!builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateBr(mergeBB);
            }
            if (scope) scope->reapplyMoves(thenMoves);
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

        // P6.3 — definite-assignment flow for while. Body may execute zero
        // times, so any markAssigned the body emits is conditional. Snapshot
        // the NYA set before the body and restore after so the body's
        // assignments don't leak out as DA-after.
        auto scope = module->getScopeStack().peek();
        std::set<std::string> preBodyNYA;
        if (scope) preBodyNYA = scope->snapshotNotYetAssigned();

        builder->SetInsertPoint(bodyBB);
        module->pushLoopContext(headBB, exitBB);
        if (body) body->generateCode(module);
        module->popLoopContext();
        if (!builder->GetInsertBlock()->hasTerminator()) {
            builder->CreateBr(headBB);
        }

        if (scope) scope->restoreNotYetAssigned(preBodyNYA);

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

        // P6.3 — same DA-flow rule as while: for-body may execute zero
        // times. Snapshot pre, restore post so conditional assignments
        // inside the body don't leak.
        auto scope = module->getScopeStack().peek();
        std::set<std::string> preBodyNYA;
        if (scope) preBodyNYA = scope->snapshotNotYetAssigned();

        builder->SetInsertPoint(bodyBB);
        // continue jumps to the update block, not the head — so the update fires
        // before the next condition test. Matches Java semantics.
        module->pushLoopContext(updateBB, exitBB);
        if (body) body->generateCode(module);
        module->popLoopContext();
        if (!builder->GetInsertBlock()->hasTerminator()) {
            builder->CreateBr(updateBB);
        }

        builder->SetInsertPoint(updateBB);
        for (auto& u : update) {
            if (u) u->generateCode(module);
        }
        builder->CreateBr(headBB);

        if (scope) scope->restoreNotYetAssigned(preBodyNYA);

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
        if (!builder->GetInsertBlock()->hasTerminator()) {
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
        // Mask the shared-state sign bit (slice-spec §3.3): the signed loop
        // compare would see a shared buffer's count as negative (0 iterations).
        sizeVal = builder->CreateAnd(sizeVal,
            llvm::ConstantInt::get(i64Ty, 0x7FFFFFFFFFFFFFFFULL), "size_m");

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
        if (!builder->GetInsertBlock()->hasTerminator()) {
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
            // Re-resolve the catch type AS WRITTEN through the scoped tier
            // discipline (own package -> imports -> global -> archive
            // placeholders) — the parse-time pre-seed used the bare global
            // registry, which misses sibling-file/archive classes and can
            // land a same-named shadow. Without this, a missed type silently
            // degraded the clause to an int64-bound CATCH-ALL: the wrong
            // clause could catch, and the bound variable held pointer bits.
            if (!c.typeNameText.empty()) {
                if (auto scoped = CajetaType::resolveNamed(
                        QualifiedName::getOrCreate(c.typeNameText), module)) {
                    c.type = scoped;
                }
            }
            if (!c.type && !c.typeNameText.empty()) {
                throw locatedException(
                    getSourceLine(), getSourceColumn() + 1,
                    "catch type '" + c.typeNameText + "' does not resolve to "
                    "a type in scope (declare it, import it, or qualify it) — "
                    "an unresolved catch type is not a catch-all",
                    "CAJETA_ERROR_CATCH_TYPE_UNRESOLVED");
            }
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
        // H4 (bugfix-plan): capture the scope-chain top at try-entry, so a throw
        // caught here first unwinds/joins any `scope { spawn ... }` children
        // spawned in the try body (otherwise the scope frame leaks and the child
        // is orphaned). Stored in an alloca so it survives the setjmp/longjmp.
        llvm::Function* scopeSaveTop =
            module->getRuntimeFunction("__cajeta_scope_save_top");
        llvm::Function* scopeExitTo =
            module->getRuntimeFunction("__cajeta_scope_exit_to");
        llvm::Value* scopeWmSlot = nullptr;
        if (scopeSaveTop && scopeExitTo) {
            scopeWmSlot =
                entryBuilder.CreateAlloca(llvm::PointerType::get(ctx, 0));
            builder->CreateStore(builder->CreateCall(scopeSaveTop, {}), scopeWmSlot);
        }
        llvm::Value* sjResult = builder->CreateCall(setjmpFn, {framePtr});
        llvm::Value* threw = builder->CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        builder->CreateCondBr(threw, catchBB, tryBB);

        // P6.3 — DA flow for try/catch. Try body may throw at any point,
        // so its assignments are conditional. Snapshot pre-try, run try
        // (snapshot post-try NYA), restore pre-try, run catch (snapshot
        // post-catch NYA), then post-after NYA = post-try ∪ post-catch
        // (variable is DA-after iff DA in both arms).
        auto daScope = module->getScopeStack().peek();
        std::set<std::string> preTryNYA;
        if (daScope) preTryNYA = daScope->snapshotNotYetAssigned();

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
        // C1: a return inside the try body must pop this frame + run finally.
        // The push records this try's finally (null for catch-only) so a
        // `return`/break/continue escaping the body unwinds it (emitTryFinallyUnwind).
        module->pushTryFinally(finallyBlock);
        if (tryBlock) tryBlock->generateCode(module);
        module->popTryFinally();
        module->popTryCatchContext();
        std::set<std::string> postTryNYA = preTryNYA;
        if (daScope) postTryNYA = daScope->snapshotNotYetAssigned();
        if (!builder->GetInsertBlock()->hasTerminator()) {
            builder->CreateCall(pop, {});
            builder->CreateBr(afterBB);
        }

        // Reset to pre-try so catch starts from the same baseline (its
        // assignments are independent of the try's).
        if (daScope) daScope->restoreNotYetAssigned(preTryNYA);
        builder->SetInsertPoint(catchBB);
        // R5/Error-model #202: getThrown now returns void*. Read it BEFORE
        // popping — pop unwinds the runtime stack and the helper reads from
        // the topmost frame. Convert back to the catch binding's declared
        // type below.
        llvm::Value* thrownValPtr = builder->CreateCall(getThrown, {});
        builder->CreateCall(pop, {});
        // H4: this try's frame is now popped, so unwinding the try body's open
        // scopes here joins their children before the catch runs — and a re-raise
        // (a child also threw) goes to the OUTER frame, not back to this one.
        if (scopeWmSlot && scopeExitTo) {
            llvm::Value* wm = builder->CreateLoad(
                llvm::PointerType::get(ctx, 0), scopeWmSlot);
            builder->CreateCall(scopeExitTo, {wm});
        }

        // Multi-clause type dispatch (Error-model RTTI catch-matching) composed
        // with try/finally semantics. For each catch clause IN SOURCE ORDER, test
        // whether the thrown object's runtime type is-a the clause's declared type
        // — a vtable parent-chain walk via __cajeta_exc_matches, the same chain
        // __cajeta_is_unrecoverable uses. The FIRST matching clause binds + runs
        // its body, then branches to afterBB (where the finally runs on the normal
        // edge). If a `finally` is present, each clause's body is additionally
        // wrapped in its own exception frame whose landing pad runs the finally +
        // re-raises (H3), so a throw OUT of a catch body still runs the finally
        // before propagating. If NO clause matches, the exception is re-thrown so
        // it propagates to the enclosing frame (we already popped this try's
        // frame, so the re-throw targets the outer handler).
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Function* excMatches = module->getRuntimeFunction("__cajeta_exc_matches");
        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");

        // Bind the thrown value to a clause's catch variable in the current
        // scope (int-shaped bindings PtrToInt back to the legacy throw shape;
        // class/pointer bindings store the ptr directly).
        auto bindCatchVar = [&](const CatchClause& c) {
            if (c.variableName.empty()) return;
            auto type = c.type ? c.type : CajetaType::of("int64");
            llvm::Type* bindTy = type->getLlvmType();
            if (!bindTy) bindTy = i64Ty;
            bool classTypedBinding =
                dynamic_pointer_cast<CajetaClass>(type) != nullptr;
            if (classTypedBinding) bindTy = ptrTy;
            llvm::Value* slot = entryBuilder.CreateAlloca(bindTy);
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
        };

        // Emit one matched clause's body, returning its normal-completion NYA
        // snapshot for the DA merge. Without a finally, the body runs directly at
        // the current (bind) block. With a finally, the body is wrapped in its own
        // exception frame (H3): normal completion pops the frame and branches to
        // afterBB (where the finally runs); a throw out of the body lands on
        // catchLandBB, which pops the frame, runs the finally, and re-raises. C1:
        // a `return` inside the catch body unwinds through the finally too, via
        // the pushed tryFinally entry (emitTryFinallyUnwind at the return site).
        auto emitClauseBody = [&](const CatchClause& c) -> std::set<std::string> {
            if (!finallyBlock) {
                if (c.body) c.body->generateCode(module);
                return daScope ? daScope->snapshotNotYetAssigned()
                               : std::set<std::string>();
            }
            llvm::Value* catchFrame = entryBuilder.CreateAlloca(
                llvm::ArrayType::get(i8Ty, frameBytes));
            llvm::BasicBlock* catchBodyBB =
                llvm::BasicBlock::Create(ctx, "catch_body", parentFn);
            llvm::BasicBlock* catchLandBB =
                llvm::BasicBlock::Create(ctx, "catch_finally", parentFn);
            builder->CreateCall(push, {catchFrame});
            llvm::Value* csj = builder->CreateCall(setjmpFn, {catchFrame});
            llvm::Value* cthrew = builder->CreateICmpNE(
                csj, llvm::ConstantInt::get(i32Ty, 0));
            builder->CreateCondBr(cthrew, catchLandBB, catchBodyBB);

            // Normal: run the catch body, pop its frame, join at afterBB.
            builder->SetInsertPoint(catchBodyBB);
            module->pushTryFinally(finallyBlock);
            if (c.body) c.body->generateCode(module);
            module->popTryFinally();
            std::set<std::string> afterBodyNYA;
            if (daScope) afterBodyNYA = daScope->snapshotNotYetAssigned();
            if (!builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateCall(pop, {});
                builder->CreateBr(afterBB);
            }

            // Landing: the catch body threw — pop its frame, run finally, re-raise.
            builder->SetInsertPoint(catchLandBB);
            llvm::Value* thrown2 = builder->CreateCall(getThrown, {});
            builder->CreateCall(pop, {});
            finallyBlock->generateCode(module);
            if (throwFn && !builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateCall(throwFn, {thrown2});
                builder->CreateUnreachable();
            }
            // Restore the body's DA state so the merge below is unaffected by the
            // throw-path finally regen.
            if (daScope) daScope->restoreNotYetAssigned(afterBodyNYA);
            return afterBodyNYA;
        };

        std::vector<std::set<std::string>> armNYAs;  // per-arm post-NYA for DA merge

        if (!catchClauses.empty()) {
            // Legacy `throw 42` idiom: the value is an integer IntToPtr'd into
            // the throw slot (below the zero-page boundary), with no vtable to
            // walk. To preserve the historical "first clause catches an untyped
            // throw" behavior — and the int-binding tests — such a value matches
            // the first clause unconditionally. Real Throwable instances
            // (>= 4096) dispatch by the vtable walk.
            llvm::Value* thrownInt = builder->CreatePtrToInt(thrownValPtr, i64Ty);
            llvm::Value* isLegacyInt = builder->CreateICmpULT(thrownInt,
                llvm::ConstantInt::get(i64Ty, 4096));
            for (size_t ci = 0; ci < catchClauses.size(); ++ci) {
                auto& c = catchClauses[ci];
                bool lastClause = (ci + 1 == catchClauses.size());
                llvm::BasicBlock* bindBB =
                    llvm::BasicBlock::Create(ctx, "catch_bind", parentFn);
                llvm::BasicBlock* nextBB = llvm::BasicBlock::Create(ctx,
                    lastClause ? "catch_nomatch" : "catch_test", parentFn);

                // Resolve this clause's #VTable global (the identity we match
                // the thrown object's chain against). A non-class clause (e.g.
                // `catch (int64 e)`) or a class with no vtable is treated as a
                // catch-all — it matches unconditionally (last-resort clause).
                //
                // The universal roots `cajeta.error.Throwable` /
                // `cajeta.error.Exception` are ALSO catch-all: they catch every
                // exception by definition, and treating them so avoids
                // dereferencing a legacy `throw 12345` integer-as-pointer (whose
                // value can exceed the 4096 low-address guard, so it isn't
                // covered by isLegacyInt) for a vtable read that would SIGSEGV.
                llvm::Constant* catchVt = nullptr;
                auto catchClass = dynamic_pointer_cast<CajetaClass>(c.type);
                bool universalCatch = false;
                if (catchClass && catchClass->getQName()) {
                    auto canon = catchClass->getQName()->toCanonical();
                    universalCatch = (canon == "cajeta.error.Throwable"
                        || canon == "cajeta.error.Exception");
                }
                if (catchClass && !universalCatch) {
                    if (auto* vt = catchClass->getVirtualTableGlobal()) {
                        catchVt = CajetaModule::ensureGlobalInModule(
                            module->emitTargetLlvmModule(), vt);
                    }
                }
                if (catchVt && excMatches) {
                    llvm::Value* m = builder->CreateCall(excMatches,
                        {thrownValPtr, catchVt}, "catch.match");
                    llvm::Value* isM = builder->CreateICmpNE(m,
                        llvm::ConstantInt::get(i32Ty, 0));
                    // A legacy int throw matches the first clause it reaches.
                    llvm::Value* cond = builder->CreateOr(isLegacyInt, isM);
                    builder->CreateCondBr(cond, bindBB, nextBB);
                } else {
                    builder->CreateBr(bindBB);  // catch-all clause
                }

                // --- matched: bind + run the body from the pre-try baseline ---
                builder->SetInsertPoint(bindBB);
                if (daScope) daScope->restoreNotYetAssigned(preTryNYA);
                bindCatchVar(c);
                std::set<std::string> armNYA = emitClauseBody(c);
                if (daScope) armNYAs.push_back(armNYA);
                // The no-finally path falls through here un-terminated; the
                // finally path already branched/terminated inside emitClauseBody.
                if (!builder->GetInsertBlock()->hasTerminator()) {
                    builder->CreateBr(afterBB);
                }

                // Continue testing the next clause (or land on the nomatch
                // block after the loop).
                builder->SetInsertPoint(nextBB);
            }
            // No clause matched: re-throw to the enclosing frame.
            if (throwFn) {
                builder->CreateCall(throwFn, {thrownValPtr});
            }
            builder->CreateUnreachable();
        } else {
            // H2 (bugfix-plan): try/finally with NO catch clause — the throw must
            // propagate, not be swallowed. Run the finally on this unwinding edge
            // (the grammar guarantees a finally here: `try` requires catch+ OR
            // finally), then re-raise. afterBB runs the finally for the normal,
            // non-throwing edge; this is the matching emission for the throw edge.
            if (finallyBlock) finallyBlock->generateCode(module);
            if (throwFn && !builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateCall(throwFn, {thrownValPtr});
                builder->CreateUnreachable();
            } else if (!builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateBr(afterBB);
            }
        }

        // Merge post-try and post-catch NYA: a name is NYA-after iff NYA in the
        // try arm OR any catch arm (union). The nomatch re-throw doesn't reach
        // afterBB, so it doesn't contribute. With no catch clauses (try {…}
        // alone), any throw propagates out, so only the post-try state reaches
        // afterBB.
        if (daScope) {
            if (!armNYAs.empty()) {
                std::set<std::string> postCatchNYA = armNYAs[0];
                for (size_t k = 1; k < armNYAs.size(); ++k) {
                    for (auto& n : armNYAs[k]) postCatchNYA.insert(n);
                }
                daScope->restoreNotYetAssigned(postTryNYA);
                daScope->mergeNotYetAssigned(postCatchNYA);
            } else {
                daScope->restoreNotYetAssigned(postTryNYA);
            }
        }

        builder->SetInsertPoint(afterBB);
        // Run finally body unconditionally (after either path). Finally that throws
        // out of itself isn't supported here.
        if (finallyBlock) finallyBlock->generateCode(module);

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

        // P6.3 — DA flow for switch. Pre-switch NYA snapshot is the
        // starting point for each group (modulo fall-through, which we
        // approximate conservatively below). After each group runs, snap
        // its post-group NYA; the post-switch NYA is the UNION across
        // groups (variable is NYA-after iff NYA in some arm). When no
        // `default:` is present, the "no case matched" path also
        // contributes — union in pre-switch NYA too.
        auto scope = module->getScopeStack().peek();
        std::set<std::string> preSwitchNYA;
        if (scope) preSwitchNYA = scope->snapshotNotYetAssigned();
        std::set<std::string> mergedPostNYA = preSwitchNYA;
        bool hasDefault = false;
        for (auto& g : groups) {
            if (g.isDefault) { hasDefault = true; break; }
        }

        // Populate cases and emit each group's statements. Fall-through is implicit
        // — if a group's terminator hasn't been set after emitting its body, we
        // branch to the next group's block (or after, if last).
        // `break` jumps out of the switch, but `continue` targets the
        // ENCLOSING loop (a switch is not a loop) — preserve that loop's
        // continue target instead of hijacking it to afterBB. When the switch
        // isn't inside a loop, afterBB is a harmless fallback (a bare
        // `continue` there is already a semantic error).
        llvm::BasicBlock* enclosingCont =
            module->hasLoopContext()
                ? module->currentLoopContext().continueTarget
                : afterBB;
        module->pushLoopContext(enclosingCont, afterBB);
        bool anyArmMerged = false;
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
            // Reset to pre-switch NYA so each group starts from the same
            // baseline (conservative wrt fall-through, which would
            // actually inherit prior arm's deltas).
            if (scope) scope->restoreNotYetAssigned(preSwitchNYA);
            builder->SetInsertPoint(groupBBs[i]);
            for (auto& s : g.statements) {
                if (s) s->generateCode(module);
            }
            std::set<std::string> postGroupNYA;
            if (scope) postGroupNYA = scope->snapshotNotYetAssigned();
            if (!builder->GetInsertBlock()->hasTerminator()) {
                llvm::BasicBlock* fallTo = (i + 1 < groups.size()) ? groupBBs[i + 1] : afterBB;
                builder->CreateBr(fallTo);
            }
            // Merge: a name is NYA-after iff NYA in SOME arm.
            if (anyArmMerged) {
                for (auto& n : postGroupNYA) mergedPostNYA.insert(n);
            } else {
                mergedPostNYA = postGroupNYA;
                anyArmMerged = true;
            }
        }
        module->popLoopContext();

        // No default → "no case matched" path didn't assign anything;
        // union pre-switch NYA back in. (anyArmMerged guards the empty-
        // switch case where there are no groups at all.)
        if (!hasDefault && anyArmMerged) {
            for (auto& n : preSwitchNYA) mergedPostNYA.insert(n);
        } else if (!anyArmMerged) {
            mergedPostNYA = preSwitchNYA;
        }
        if (scope) scope->restoreNotYetAssigned(mergedPostNYA);

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
    // C1 (bugfix-plan): a `return` inside one or more enclosing try/catch bodies
    // must pop each active exception frame (innermost first) and run its finally
    // before the method's own scope-exit/drops/ret — otherwise the frame dangles
    // (a later throw longjmps into the returned, dead stack) and the finally never
    // runs. Emitted at every return site, after the return value is evaluated.
    // Unwinds the active try frames from the innermost down to `stopDepth` (the
    // number of frames to KEEP): pop each + run its finally. `stopDepth == 0` is a
    // return (unwind all enclosing tries); break/continue pass the loop's
    // tryFinallyDepth so only the tries entered inside the loop are unwound.
    static void emitTryFinallyUnwind(CajetaModulePtr module, size_t stopDepth = 0) {
        auto& stack = module->getTryFinallyStack();
        if (stack.size() <= stopDepth) return;
        auto* builder = module->getBuilder();
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        // The finally regen below mutates the (single, evolving) definite-
        // assignment set; the code after a break/continue/return is unreachable,
        // so snapshot and restore so the method's DA tracking is unaffected.
        auto daScope = module->getScopeStack().peek();
        std::set<std::string> savedNYA;
        if (daScope) savedNYA = daScope->snapshotNotYetAssigned();
        for (size_t i = stack.size(); i > stopDepth; --i) {
            if (builder->GetInsertBlock()->hasTerminator()) break;
            if (pop) builder->CreateCall(pop, {});
            if (stack[i - 1]) {
                auto fin = std::static_pointer_cast<Statement>(stack[i - 1]);
                fin->generateCode(module);
            }
        }
        if (daScope) daScope->restoreNotYetAssigned(savedNYA);
    }

    static void emitScopeExitToWatermark(CajetaModulePtr module) {
        // Debugger CP5: pop this method's debug frame on the return path. Done
        // first (independent of the watermark below) so it fires on every
        // explicit-return chokepoint that funnels through here. No-op unless
        // --debug-info.
        {
            auto m = module->getCurrentMethod();
            dbg::emitDbgFrameLeave(module, m ? m->getDbgFrameSlot() : nullptr);
        }
        // diagnostic-exceptions U3: pop the line-info shadow frame on this return
        // path (no-op unless --line-info). Same every-return coverage as above.
        dbg::emitLineLeave(module);
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

    // title-tracking Unit 5 (spec §4.2): a class-pointer-returning method
    // stores its paired return flag immediately before `ret` — AFTER owner
    // drops, so no drop_fn (which may itself return class pointers) can
    // clobber the thread-local between this store and the caller's read.
    // Slice 1 stores the method's static mode (`#`-return → 1, plain → 0);
    // 5.2.3 upgrades `return #x` of a runtime owner to forward its bit.
    static void emitReturnFlag(CajetaModulePtr& module,
                               llvm::Value* flagOverride = nullptr) {
        auto m = module->getCurrentMethod();
        // 7.2.5 — lambda bodies have no Method context; the module flag says
        // the synthesized function returns a class pointer. Static mode for
        // a lambda return is BORROW (the caller-visible ownership cases —
        // fresh construction, `#x`, tail call — arrive via flagOverride or
        // ride the inner call's own flag).
        bool lambdaMode = !m && module->isLambdaClassPtrReturn();
        if (!lambdaMode && (!m || !m->returnsClassPointer())) {
            return;
        }
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_return_flag_set");
        if (!fn) {
            return;
        }
        // 5.2.2 — a `return #x` / returned formal threads its title's runtime
        // flag; otherwise the method's static mode stands (5.2.3 arms callers).
        llvm::Value* flag = flagOverride ? flagOverride
            : (llvm::Value*) module->getBuilder()->getInt64(
                  (!lambdaMode && m->isReturnsOwnership()) ? 1 : 0);
        module->getBuilder()->CreateCall(fn, {flag});
    }

    // stdlib-ownership-convention 8.1.1 — enumerate the ride-through sites.
    //
    // Unit 8 asks whether the return still carries the ownership decision, and
    // the answer turns on how many plain-return methods hand out a title
    // anyway. This reads the flag the compiler ITSELF is about to store, at the
    // one point where it is final — so the enumeration is a measurement, not a
    // second classifier that could be wrong in its own way (3.3.3). Off unless
    // CAJETA_AUDIT_RETURN_TITLES is set; see ReturnTitleAudit.h.
    static void auditReturnTitle(CajetaModulePtr& module, llvm::Value* flag,
                                 const ExpressionPtr& expression,
                                 int sourceLine) {
        namespace own = cajeta::ownership;
        auto m = module->getCurrentMethod();
        // Only a PLAIN class-pointer return can misreport: a `#` return
        // declares the transfer, and a primitive carries no title at all.
        if (!m || !m->returnsClassPointer() || m->isReturnsOwnership()) return;
        const std::string cls =
            m->getParent() ? m->getParent()->toCanonical() : "";
        const std::string ret =
            m->getReturnType() ? m->getReturnType()->toCanonical() : "";
        // The denominator: this site is a plain return the audit examined.
        own::ReturnTitleAudit::consider(cls, m->getName(), ret);
        // No override means the static borrow default stands — the conforming
        // view return, and the overwhelming majority. A constant 0 is the same
        // answer reached explicitly (`return #= x` from a frame holding none).
        if (!flag) return;
        if (auto* konst = llvm::dyn_cast<llvm::ConstantInt>(flag)) {
            if (konst->isZero()) return;
        }

        own::ReturnTitleRecord rec;
        rec.className = cls;
        rec.methodName = m->getName();
        rec.returnType = ret;
        // 8.1.4 — a line is reported ONLY when it points at real source. A
        // monomorphization's AST comes from a synthesized instantiation
        // buffer, so `getSourceLine()` counts lines in that buffer, not in the
        // file: `Stream.reduce` reported :309/:342/:370 for a body that lives
        // at :331-335, and a different number per instantiation. Class,
        // method, mechanism and counts are all still correct — only the line
        // is meaningless, so it is dropped rather than printed. A line that
        // points somewhere real and wrong is worse than no line at all; this
        // is the same convention `AbstractSyntaxNode::getSourceFile` follows.
        const bool synthesizedBuffer =
            m->isMethodTemplateInstantiation()
            || (m->getParent() && m->getParent()->isInstantiation());
        rec.line = synthesizedBuffer ? 0 : sourceLine;
        rec.carry = llvm::isa<llvm::ConstantInt>(flag)
            ? own::TitleCarry::StaticTitle : own::TitleCarry::RuntimeFlag;

        // Which mechanism produced the flag. The runtime helper's name is the
        // ground truth — each corresponds to exactly one assignment site above.
        llvm::Value* src = flag;
        if (auto* zext = llvm::dyn_cast<llvm::ZExtInst>(src)) {
            src = zext->getOperand(0);
        }
        std::string helper;
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(src)) {
            if (llvm::Function* callee = call->getCalledFunction()) {
                helper = callee->getName().str();
            }
        }
        // The SPELLING is checked before the helper, because `return #x` of a
        // local forwards that local's own `__cajeta_drop_entry_flag` — the
        // same helper a returned formal uses. Measured, not assumed
        // (ReturnTitleAuditTests.moveReturnUnderPlainTypeIsEnumerated): `#`
        // does not assert a title in the return position any more than it does
        // in the argument position, it forwards the mode the frame holds
        // (CLAUDE.md §2.2). Classifying by helper alone would file every
        // `return #x` as a pass-through and lose the shape the audit is for.
        if (dynamic_pointer_cast<MoveExpression>(expression)) {
            rec.via = own::TitleVia::Move;
        } else if (helper == "__cajeta_return_flag_get") {
            rec.via = own::TitleVia::CallRide;
            // WHAT is tail-called decides how much the ride means: a callee
            // that declares `#` hands a title out through a plain signature
            // (the shape 8.1.1 counts), while a plain callee only defers the
            // decision one frame — a fluent builder chain, not a finding.
            if (auto mce =
                    dynamic_pointer_cast<MethodCallExpression>(expression)) {
                if (MethodPtr callee = mce->getResolvedMethod()) {
                    rec.calleeKey =
                        (callee->getParent()
                            ? callee->getParent()->toCanonical() + "."
                            : std::string())
                        + callee->getName();
                    rec.calleeOwned = callee->isReturnsOwnership();
                }
            } else if (auto ce =
                    dynamic_pointer_cast<CallExpression>(expression)) {
                // A closure call — the `Stream.fold<R>` shape. Nothing static
                // decides: the callback is a parameter, so its declared
                // function type is all there is to read.
                if (ExpressionPtr target = ce->getCallee()) {
                    if (auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                            target->getResolvedType())) {
                        rec.calleeOwned = fnType->isReturnsOwnership();
                    }
                }
            }
        } else if (helper == "__cajeta_drop_entry_flag") {
            rec.via = own::TitleVia::FormalPassThrough;
        } else if (helper == "__cajeta_drop_take_active") {
            rec.via = own::TitleVia::ModeCarry;
        } else {
            auto mce = dynamic_pointer_cast<MethodCallExpression>(expression);
            rec.via = (mce && mce->getFlaggedTitleValue() == flag)
                ? own::TitleVia::Flagged : own::TitleVia::Other;
        }
        own::ReturnTitleAudit::record(std::move(rec));
    }

    llvm::Value* ReturnStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        // 5.2.2 — runtime title flag riding out with this return (formal
        // pass-through or `#x`); null -> emitReturnFlag uses the static mode.
        llvm::Value* returnTitleFlag = nullptr;
        // argument-title-carry — `return #= x`: release WHATEVER title this
        // frame holds. `__cajeta_drop_take_active` reads the local's drop
        // entry and disarms it in one step, so the prior value becomes the
        // flagged return's runtime bit: 1 when we owned it (the caller now
        // does), 0 when we only ever held a borrow (so does the caller).
        //
        // Distinct from `return #x`, which declares the transfer contract but
        // itself forwards the mode the frame holds (MOVE_OF_BORROW rejects it
        // where the frame provably holds none), and from `return x`, which is
        // transparent carry — a formal forwards the caller's mode and hands
        // its entry over, while a named local with a drop entry is rejected
        // by FRESH_RETURN_NEEDS_TRANSFER. Needed because a collection slot
        // may now hold either an
        // owned value or a borrow (collections no longer own by default), so a
        // remove-shaped return cannot decide statically.
        if (modeCarrying && expression) {
            if (auto mcId = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                if (auto scope = module->getScopeStack().peek()) {
                    if (FieldPtr mcFld = scope->getField(mcId->getTextValue())) {
                        if (llvm::Value* mcEntry = mcFld->getDropEntry()) {
                            if (llvm::Function* takeFn = module->getRuntimeFunction(
                                    "__cajeta_drop_take_active")) {
                                llvm::Value* was = builder->CreateCall(
                                    takeFn, {mcEntry}, "title.release");
                                returnTitleFlag = builder->CreateZExt(
                                    was, llvm::Type::getInt64Ty(
                                        *module->getLlvmContext()),
                                    "title.release.i64");
                            }
                        }
                    }
                }
            }
            // No drop entry (arena local, borrowed formal, non-droppable) means
            // this frame holds no title to release: flag 0, a lend.
            if (!returnTitleFlag) {
                returnTitleFlag = builder->getInt64(0);
            }
        }
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
            // C1: run enclosing try finallys + pop their frames first (this also
            // emits the __cajeta_exc_pop for each escaped try frame).
            emitTryFinallyUnwind(module);
            // Pop any open scope frames before the value-less return so
            // every pending child task is joined first.
            emitScopeExitToWatermark(module);
            // Fire drops before the value-less return.
            if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
            return builder->CreateRetVoid();
        }
        // array-literals §3.2 — a returned `[...]` literal is target-typed by
        // the method's declared array return type (before any generateCode
        // below consumes it), so `int64[] f() { return [1,2,3]; }` widens.
        // collection-literals §2 — a class (collection) return type instead
        // rewrites the literal to a from-array ctor call `heap Target([...])`.
        if (auto arrLit = dynamic_pointer_cast<ArrayLiteralExpression>(expression)) {
            if (auto m = module->getCurrentMethod()) {
                CajetaTypePtr rt = m->getReturnType();
                if (auto at = dynamic_pointer_cast<CajetaArray>(rt)) {
                    arrLit->setElementType(at->getElementType());
                } else if (auto ctor = collectionLiteralFromArray(rt, expression)) {
                    expression = ctor;
                }
            }
        } else if (auto agg = dynamic_pointer_cast<
                       AggregateInitializerExpression>(expression)) {
            // collection-literals §4 — a returned prefixless `{…}` infers the
            // method's declared return type (only when it's a class; a
            // primitive return leaves it uninferred → clean NO_TYPE error).
            if (auto m = module->getCurrentMethod()) {
                CajetaTypePtr rt = m->getReturnType();
                if (dynamic_pointer_cast<CajetaClass>(rt)
                        && !dynamic_pointer_cast<CajetaArray>(rt)) {
                    agg->setExpectedType(rt);
                }
            }
        } else if (auto mapLit = dynamic_pointer_cast<
                       MapLiteralExpression>(expression)) {
            // collection-literals §3 — a returned `[k: v]` infers the declared
            // map return type.
            if (auto m = module->getCurrentMethod()) {
                CajetaTypePtr rt = m->getReturnType();
                if (dynamic_pointer_cast<CajetaClass>(rt)
                        && !dynamic_pointer_cast<CajetaArray>(rt)) {
                    mapLit->setExpectedType(rt);
                }
            }
        }
        // Value-return (sret + NRVO): the enclosing method/lambda hands back
        // a `stack`-constructed value by copy. Its LLVM signature returns
        // `void` and takes the result slot as hidden arg 0. Build the
        // returned value directly into that caller-owned slot (NRVO for a
        // `stack X(...)` construction; memcpy/store fallback otherwise),
        // then `ret void`. No pointer escapes the dying frame — the bytes
        // already live in the caller. This bypasses the fresh-return
        // rejection + by-pointer load machinery below, which both assume a
        // pointer/value return. Triggered for methods via
        // Method::returnsStackValue() (the body-scan that drives the sret
        // ABI in Method.cpp) and for lambdas via the LLVM signature itself
        // (M5(b) — lambdas have no Method context but their underlying
        // function carries the sret attribute on arg 0).
        // See docs/specification/lang/ValueReturns.md.
        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        bool sretMethod = false;
        if (auto m = module->getCurrentMethod()) {
            sretMethod = m->returnsStackValue();
        }
        bool sretFnSig = curFn && curFn->getReturnType()->isVoidTy()
            && curFn->arg_size() > 0
            && curFn->getArg(0)->hasAttribute(llvm::Attribute::StructRet);
        if ((sretMethod || sretFnSig) && expression) {
            llvm::Value* sretPtr = curFn->getArg(0);
            if (auto newExpr = dynamic_pointer_cast<NewExpression>(expression)) {
                newExpr->setNrvoTarget(sretPtr);
                expression->generateCode(module);
            } else {
                // Fallback (e.g. `return someStackLocal;` or a stack
                // aggregate): produce the value/pointer and copy it into
                // the sret slot. For a class-typed identifier (the
                // FilterStream `return o;` shape after the #66 stream
                // pipeline sweep), the field's getOrCreateAllocation
                // returns the local's pointer SLOT (an alloca of `ptr`)
                // — to copy the struct bytes we have to load through the
                // slot once to recover the struct's actual address. The
                // class typing comes from the expression's resolvedType;
                // raw `pointer`-typed slots and aggregate-direct results
                // skip the load.
                llvm::Value* v = expression->generateCode(module);
                llvm::Type* structTy = nullptr;
                if (auto m = module->getCurrentMethod()) {
                    if (m->getReturnType()) structTy = m->getReturnType()->getLlvmType();
                }
                if (!structTy && sretFnSig) {
                    // Recover the struct type from the sret param attribute
                    // (set when the function was created).
                    structTy = curFn->getArg(0)->getAttribute(
                        llvm::Attribute::StructRet).getValueAsType();
                }
                if (v && structTy) {
                    if (v->getType()->isPointerTy()) {
                        // A class-typed expression (the `return o;` shape
                        // for a class local; also a class-typed parameter
                        // or member access) produced a SLOT pointer, not
                        // the struct address — local slots are alloca of
                        // ptr that holds the struct address. To memcpy
                        // the struct bytes we have to load through the
                        // slot once. Without the load, the memcpy reads
                        // the slot's pointer bytes (8 bytes of `ptr to
                        // struct` + adjacent stack) instead of the struct
                        // contents — the runtime SIGSEGVs or returns
                        // uninitialized data on the caller's first
                        // method call.
                        llvm::Value* srcPtr = v;
                        // Identifier-of-class-typed-local: the field's
                        // allocation is a SLOT (alloca of ptr) holding
                        // the struct address. Load through to get the
                        // struct address before memcpy. Field lookup
                        // through scope, not expression->getResolvedType
                        // (which IdentifierExpression::resolveTypes leaves
                        // null when the local was declared after the
                        // surrounding block's resolveTypes ran — see
                        // Statement.cpp's resolveTypes ordering).
                        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                            if (auto scope = module->getScopeStack().peek()) {
                                FieldPtr fld = scope->getField(idExpr->getTextValue());
                                if (fld) {
                                    auto klass = dynamic_pointer_cast<CajetaClass>(fld->getType());
                                    if (klass && !klass->isInterface()) {
                                        llvm::Type* ptrTy = llvm::PointerType::get(
                                            *module->getLlvmContext(), 0);
                                        srcPtr = builder->CreateLoad(ptrTy, v);
                                    }
                                }
                            }
                        }
                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        llvm::Value* sz = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(*module->getLlvmContext()),
                            dl.getTypeAllocSize(structTy));
                        builder->CreateMemCpy(sretPtr, llvm::MaybeAlign(8),
                            srcPtr, llvm::MaybeAlign(8), sz);
                    } else {
                        builder->CreateStore(v, sretPtr);
                    }
                }
            }
            if (auto m = module->getCurrentMethod()) {
                m->emitAfterAdvice(module);
                m->emitAfterReturningAdvice(module);
                m->emitAfterThrowingTryPop(module);
            }
            // C1: run enclosing try finallys + pop their frames before the
            // sret value-return (mirrors the void + typed return paths).
            emitTryFinallyUnwind(module);
            emitScopeExitToWatermark(module);
            if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
            return builder->CreateRetVoid();
        }
        // stack-return-transfer-error-spec §2.1: a `#T` return type promises
        // the caller an OWNED value that outlives this frame, but a `stack`
        // instance dies with the frame. The pairing is always wrong — the
        // caller registers a drop entry over reclaimed stack memory. Before
        // this check the direct shape reached codegen and blew up in the IR
        // verifier ("return type does not match operand type"), and the
        // named-local shape (`Cell c = stack Cell(); return #c;`) compiled
        // clean and returned clobbered memory. Both are now one diagnostic;
        // the fix for both is `heap`.
        if (auto m = module->getCurrentMethod()) {
            if (m->isReturnsOwnership() && expression) {
                // Unwrap `return #c` to reach the moved-from identifier.
                ExpressionPtr inner = expression;
                if (auto mv = dynamic_pointer_cast<MoveExpression>(expression)) {
                    auto& mch = mv->getChildren();
                    if (!mch.empty()) {
                        inner = dynamic_pointer_cast<Expression>(mch[0]);
                        if (!inner) inner = expression;
                    }
                }
                // owned-return-of-borrowed-this §4: the receiver is a
                // plain-borrow formal — the method holds NO title to
                // transfer, so `return this` under a `#` return would hand
                // the caller ownership of the borrowed receiver: the
                // caller's temp drop frees the wrapper out from under the
                // receiver local (UAF, detonating when the block is
                // reused). Matches the field-store-title-trap doctrine
                // that plain formals never inherit ownership.
                bool returnsBorrowedThis =
                    dynamic_pointer_cast<ThisExpression>(inner) != nullptr;
                if (!returnsBorrowedThis) {
                    if (auto idExpr =
                            dynamic_pointer_cast<IdentifierExpression>(inner)) {
                        returnsBorrowedThis = idExpr->getTextValue() == "this";
                    }
                }
                if (returnsBorrowedThis) {
                    throw Exception(
                        "method `" + m->toCanonical(false) + "` returns "
                        "`this` through a `#` (ownership-transfer) return "
                        "type, but the receiver is a borrow — the method "
                        "holds no title to transfer. The caller would own "
                        "the receiver's wrapper and free it on drop, "
                        "dangling the caller's own local (use-after-free). "
                        "Fix: return a fresh owned value (e.g. an owned "
                        "copy, or a zero-copy borrow window such as "
                        "`Cajeta.stringSliceBorrow(this, 0, "
                        "this.byteLength())` for String), or drop the `#` "
                        "from the return type if the caller only borrows "
                        "the result. See "
                        "specs/owned-return-of-borrowed-this-spec.md.",
                        "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS");
                }
                // 8.2.6 / spec §4.5 — the same doctrine one step out from
                // `this`: a `#` return is a CONTRACT THE CALLEE MUST KEEP, and
                // a plain return under a `#` declaration takes the static mode
                // (flag 1) unchallenged. The existing TITLE_MISS guard only
                // covers `return #x` with a RUNTIME flag, so a frame that
                // never held a title could assert one and nothing said a word
                // — which is exactly how `reduceParallelChain` handed callers
                // a forged title over their own lent seed (8.4.2), found by a
                // canary test rather than by the compiler.
                //
                // The split with Unit 2 is by FORM, not by provenance, and it
                // took two baselines to get right: `return #x` is Unit 2's
                // (`CAJETA_ERROR_MOVE_OF_BORROW` — the literal `#`-on-a-borrow
                // it was built for, plus the runtime TITLE_MISS behind it),
                // while a PLAIN return under a `#` declaration is diagnosed by
                // nothing at all, whichever provenance the local carries. That
                // silent case is this check's, and it covers both origins.
                //
                // Fires only where provenance PROVES the frame holds a borrow,
                // never on "cannot prove" — spec §7.2's discipline, and the
                // reason it should not repeat 3.3.3's gate breakage.
                //
                // `return #= x` is exempt by design: it declares the return
                // carries the MODE rather than claiming a title, so there is
                // no promise to break. It is also the fix this diagnostic
                // prescribes, and what 8.4.2 landed.
                if (!modeCarrying
                        && !dynamic_pointer_cast<MoveExpression>(expression)) {
                    if (auto borrowId =
                            dynamic_pointer_cast<IdentifierExpression>(inner)) {
                        FieldPtr rf;
                        if (auto scope = module->getScopeStack().peek()) {
                            rf = scope->getField(borrowId->getTextValue());
                        }
                        if (rf) {
                            std::string origin = rf->getCallBorrowOrigin();
                            std::string lender = "the borrow-returning call `"
                                + origin + "`, whose receiver still owns and "
                                "frees the value";
                            if (origin.empty()) {
                                origin = rf->getParamBorrowOrigin();
                                lender = "the plain parameter `" + origin
                                    + "`, whose title stays with the caller";
                            }
                            if (!origin.empty()) {
                                throw Exception(
                                    "method `" + m->toCanonical(false)
                                    + "` promises ownership with a `#` return "
                                    "type, but `"
                                    + borrowId->getTextValue()
                                    + "` holds a BORROW — it came from "
                                    + lender + ". The `#` return asserts a "
                                    "title this frame never held, so the "
                                    "caller arms a drop on a value someone "
                                    "else still owns and frees: a double free "
                                    "on the caller's scope exit. Fix: return "
                                    "`#= " + borrowId->getTextValue()
                                    + "` to carry whatever mode the frame "
                                    "actually holds (the caller then registers "
                                    "a drop only when it really received a "
                                    "title), or produce an owned value, or "
                                    "drop the `#` from the return type. See "
                                    "specs/stdlib-ownership-convention-spec.md "
                                    "§4.5.",
                                    "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
                            }
                        }
                    }
                }
                bool stackReturn = Method::exprIsStackConstruction(inner);
                std::string what = "a `stack` construction";
                if (!stackReturn) {
                    if (auto idExpr =
                            dynamic_pointer_cast<IdentifierExpression>(inner)) {
                        if (auto scope = module->getScopeStack().peek()) {
                            FieldPtr f = scope->getField(idExpr->getTextValue());
                            if (f && f->isStackInstance()) {
                                stackReturn = true;
                                what = "local '" + idExpr->getTextValue()
                                     + "', a `stack` value,";
                            }
                        }
                    }
                }
                if (stackReturn) {
                    throw Exception(
                        "method `" + m->toCanonical(false) + "` returns " + what
                        + " through a `#` (ownership-transfer) return type. The "
                        "stack instance is reclaimed when this frame exits, but "
                        "`#` promises the caller an owned value that outlives "
                        "the call — the caller would register a drop over freed "
                        "stack memory. Fix: allocate with `heap` (the value "
                        "must outlive the frame), or drop the `#` from the "
                        "return type if you meant to return by value. See "
                        "docs/specification/lang/MemoryModel.md § Function "
                        "signatures.",
                        "CAJETA_ERROR_STACK_RETURN_ESCAPES");
                }
            }
        }
        // Memory-model § Function signatures: returning a fresh
        // allocation (`return heap X(...)`, `return new X(...)`,
        // `return heap X { ... }`) requires the enclosing method's
        // return type to be marked `#` for ownership transfer. A plain
        // return carries whatever the RETURN EXPRESSION holds (spec §2.8,
        // §4.8) — a formal, a call result, or `#= local`. A fresh
        // allocation is none of those: its title is held only by this
        // frame's drop chain, so the allocation would either
        // leak (caller doesn't drop) or get freed by the function's
        // scope-exit drop chain and hand back a dangling pointer.
        //
        // Same story for `stack X(...)` returns: the stack memory
        // dies on function return, so any return at all is a use-
        // after-free hazard. Both shapes get the same diagnostic
        // since the fix is the same: mark the return type with `#`
        // for `heap` (the right form), and switch to `heap` for
        // anything you wanted to escape the function.
        if (auto m = module->getCurrentMethod()) {
            // Skip lambdas — the (T) -> R type syntax doesn't carry a
            // `#` for the return; treating lambda returns as borrow-by-
            // default would block any lambda that constructs and
            // returns a fresh value. When the lambda type syntax gains
            // `#R` support, this carve-out can drop.
            bool isLambda = m->getName().rfind("__cajeta_lambda_", 0) == 0;
            // @ValueType returns are by-value (Copy): `return stack Vec2(...)`
            // copies the value into the caller's slot like a primitive — no
            // ownership transfer, no dangling-borrow hazard. Exempt them so static
            // value-type operators (Vec2 operator+(Vec2,Vec2)) need no `#`.
            // See plans/value-type-overloading-plan.md (value types are Copy).
            auto rtype = m->getReturnType();
            bool returnsValueType = rtype && rtype->isValueType();
            // Builtin by-value aggregates — Vector<T,N>, Matrix<T,R,C>,
            // Quaternion<T> — are PRIMITIVE_FLAG: `return stack Matrix(...)`
            // copies the value into the caller's slot exactly like a scalar or
            // an @ValueType return, with no heap pointer and so no dangling-
            // borrow hazard. They lack VALUE_TYPE_FLAG only because they are
            // compiler builtins rather than @ValueType-annotated classes, so
            // isValueType() above misses them. Exempt them here too — otherwise
            // a math builder like cajeta.math.Camera.perspective() returning a
            // freshly-constructed Matrix trips the fresh-return check spuriously.
            bool returnsByValuePrimitive =
                rtype && (rtype->getTypeFlags() & PRIMITIVE_FLAG) != 0;
            // argument-title-carry — `return #= x` is exempt from BOTH shapes
            // below. The guard exists because a non-`#` signature makes the
            // caller register no drop entry, so a released title would leak.
            // The mode-carrying return closes exactly that hole: it ships the
            // title's runtime bit in the return flag, and the caller's `#=`
            // receipt registers a drop entry when the bit is 1. Without this
            // exemption the guard rejects the very form built to satisfy it.
            if (!isLambda && !m->isReturnsOwnership() && !returnsValueType
                    && !returnsByValuePrimitive && !modeCarrying) {
                auto newExpr = dynamic_pointer_cast<NewExpression>(expression);
                auto aggExpr = dynamic_pointer_cast<AggregateInitializerExpression>(expression);
                if (newExpr || aggExpr) {
                    std::string canonical = m->toCanonical(false);
                    throw Exception(
                        "method `" + canonical + "` returns a freshly "
                        "allocated value but its return type isn't marked "
                        "`#` for ownership transfer. Without `#`, the "
                        "caller treats the return as a borrow tied to "
                        "no source — the allocation either leaks or gets "
                        "freed by this function's scope-exit drop chain "
                        "and hands back a dangling pointer. Fix: change "
                        "the return type to `#T` so ownership transfers "
                        "to the caller. See docs/specification/"
                        "MemoryModel.md § Function signatures.",
                        "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER");
                }
                // Same hazard, named-local shape: `Foo c = new Foo();
                // return c;` (or `#Foo p` parameter `return p;`). The
                // local owns the allocation — its registered drop entry
                // would deactivate at the return per the class-instance
                // transfer block below, while the receiver treats the
                // value as a borrow (post task #54 semantics: receive-
                // side only registers a drop if the callee's return is
                // `#`). Net result: the allocation silently leaks. The
                // `return new X()` shape above caught only the inline
                // construction; this catches the symmetric leak when
                // the freshly-owned value flows through a named local
                // first. Same fix: mark the return type `#T`.
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                    if (auto scope = module->getScopeStack().peek()) {
                        FieldPtr f = scope->getField(idExpr->getTextValue());
                        // 5.2.2 — formals are RUNTIME owners: their entry is
                        // armed per call, and a plain return is the legal
                        // pass-through (flag rides the return-flag TLS).
                        // Only statically-owned LOCALS demand the `#` return.
                        if (dynamic_pointer_cast<ParameterField>(f)) {
                            f = nullptr;
                        }
                        if (f && f->getDropEntry()) {
                            auto klass = dynamic_pointer_cast<CajetaClass>(f->getType());
                            auto view = dynamic_pointer_cast<CajetaView>(f->getType());
                            // Value types are exempt: they return by COPY, and
                            // a shared-capable value local's drop entry (slice-
                            // spec §6.1) deactivates at return as a move-out —
                            // no `#` needed for correctness.
                            bool transferShape =
                                (klass && !klass->isInterface()
                                       && !klass->isValueType()) || (bool) view;
                            if (transferShape) {
                                std::string canonical = m->toCanonical(false);
                                throw Exception(
                                    "method `" + canonical + "` returns owned "
                                    "local '" + idExpr->getTextValue() + "' "
                                    "but its return type isn't marked `#` for "
                                    "ownership transfer. The local owns its "
                                    "value (its drop entry is active), so the "
                                    "return deactivates this scope's drop, "
                                    "while the caller — per the non-`#` "
                                    "signature — won't register a fresh drop "
                                    "entry on receipt: the allocation silently "
                                    "leaks. Fix: change the return type to "
                                    "`#T`. See docs/specification/MemoryModel"
                                    ".md § Function signatures.",
                                    "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER");
                            }
                        }
                    }
                }
            }
        }
        // Phase 3a of #68 (body-side borrow-escape check): returning a
        // borrowed class parameter through a `#T`-return signature is
        // a silent escape. The caller will register a fresh drop entry
        // expecting ownership, but the value was the caller-of-caller's
        // borrow — when the original owner drops, the receiver's drop
        // fires on the same allocation and produces a double free. The
        // borrow-param's formal is plain `T` (not `#T`), so the
        // parameter doesn't own its value and can't transfer it.
        if (auto m = module->getCurrentMethod()) {
            if (m->isReturnsOwnership() && expression) {
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                    auto scope = module->getScopeStack().peek();
                    if (scope) {
                        FieldPtr fld = scope->getField(idExpr->getTextValue());
                        auto pf = dynamic_pointer_cast<ParameterField>(fld);
                        if (pf) {
                            auto formal = pf->getFormalParameter();
                            // 5.2.4 — RETIRED for class-typed formals: a plain
                            // formal is a RUNTIME owner (5.2.2), so returning it
                            // through a `#T` signature forwards its actual flag
                            // (captured above) rather than forging ownership.
                            bool runtimeOwner = fld && fld->getDropEntry();
                            if (formal && !formal->isTransferred()
                                    && !runtimeOwner) {
                                auto klass = dynamic_pointer_cast<CajetaClass>(
                                    fld->getType());
                                if (klass && !klass->isInterface()) {
                                    throw Exception(
                                        "method `" + m->toCanonical(false)
                                            + "` declares a `#T` return "
                                            "(ownership transfer) but returns "
                                            "borrowed parameter `"
                                            + idExpr->getTextValue() + "` "
                                            "declared without `#`. The caller "
                                            "would register a fresh drop entry "
                                            "on receipt and free a value the "
                                            "original owner still references. "
                                            "Fix: mark the parameter `#"
                                            + idExpr->getTextValue() + "` so the "
                                            "caller transfers ownership, or "
                                            "change the return type to plain "
                                            "`T` (borrow pass-through). See "
                                            "docs/specification/lang/OwnershipTransfer.md.",
                                        "CAJETA_ERROR_BORROW_PARAM_ESCAPES");
                                }
                            }
                        }
                    }
                }
            }
        }

        // 5.2.7 (spec §7.4, sub-fork B) — single-hop DANGLING LEND. The
        // returned holder escapes this method, but any local it merely LENT
        // (a plain, non-`#` store or arg) dies at this scope's exit — the
        // caller would receive an object pointing at freed memory. Covers
        // both `return h` and `return #h`. Conservative and intra-procedural:
        // the fix is to spell the lend `#s` (surrender the title to the
        // holder), which records no edge.
        {
            ExpressionPtr escapee = expression;
            if (auto mvEsc = dynamic_pointer_cast<MoveExpression>(escapee)) {
                auto& mch = mvEsc->getChildren();
                if (!mch.empty()) {
                    escapee = dynamic_pointer_cast<Expression>(mch[0]);
                }
            }
            if (auto escId = dynamic_pointer_cast<IdentifierExpression>(escapee)) {
                if (auto sc = module->getScopeStack().peek()) {
                    set<string> lends = sc->lendsOf(escId->getTextValue());
                    if (!lends.empty()) {
                        std::string names;
                        for (auto& n : lends) {
                            if (!names.empty()) names += "`, `";
                            names += n;
                        }
                        throw Exception(
                            "cannot return `" + escId->getTextValue()
                                + "` — it holds a LEND of the local `" + names
                                + "`, which drops when this method returns, so "
                                  "the caller would receive an object pointing "
                                  "at freed memory. A plain store/argument lends "
                                  "(the title stays with the local); only `#` "
                                  "surrenders it. Fix: spell the lend `#" + names
                                + "` so the holder takes the title, or don't let "
                                  "the holder escape. See docs/specification/"
                                  "MemoryModel.md § Function signatures.",
                            "CAJETA_ERROR_DANGLING_LEND");
                    }
                }
            }
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
        // 6.2.6c — a reference cast is identity on the pointer:
        // `return (Stream<T>) newRoot;` is the same pass-through as
        // `return newRoot;`, so the disarm/escape checks below must see
        // the underlying local. Value casts (`(int64) obj`) don't move
        // the object out — the scope keeps its drop — so only casts to
        // a concrete class or view type are peeled.
        AbstractSyntaxNodePtr returnee = expression;
        while (auto castExpr = dynamic_pointer_cast<CastExpression>(returnee)) {
            CajetaTypePtr dt = castExpr->getDestType();
            auto destClass = dynamic_pointer_cast<CajetaClass>(dt);
            auto destView = dynamic_pointer_cast<CajetaView>(dt);
            bool refCast = (bool) destView
                || (destClass && !destClass->isInterface()
                    && !destClass->isValueType());
            if (!refCast || castExpr->getChildren().empty()) break;
            returnee = castExpr->getChildren()[0];
        }
        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(returnee)) {
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
            // Arrays aren't covered here — they remain owned by the
            // declaring scope regardless of whether the array header
            // is returned (a separate pre-existing limitation).
            //
            // Owning views (a view whose ctor took #-transferred bytes)
            // DO have a drop entry — the owning ctor pushes one paired
            // with __cajeta_view_drop_owned. When the function returns
            // the view, ownership of the underlying buffer transfers
            // to the caller and this scope must not free it. The
            // generic `f->getDropEntry()` check covers both classes and
            // owning views.
            if (f) {
                auto klass = dynamic_pointer_cast<CajetaClass>(f->getType());
                auto view = dynamic_pointer_cast<CajetaView>(f->getType());
                bool transferShape =
                    (klass && !view && !klass->isInterface()) || (bool) view;
                if (transferShape) {
                    if (llvm::Value* entry = f->getDropEntry()) {
                        // 5.2.2 — a returned formal is a pass-through: its
                        // runtime flag rides out on the return-flag TLS
                        // (captured before deactivation).
                        if (dynamic_pointer_cast<ParameterField>(f)) {
                            if (llvm::Function* flagFn =
                                    module->getRuntimeFunction(
                                        "__cajeta_drop_entry_flag")) {
                                returnTitleFlag = builder->CreateCall(
                                    flagFn, {entry}, "ret_title_flag");
                            }
                        }
                        if (llvm::Function* mark = module->getRuntimeFunction(
                                "__cajeta_drop_mark_inactive")) {
                            builder->CreateCall(mark, {entry});
                        }
                    }
                }
            }
        }
        llvm::Value* val = expression->generateCode(module);
        // A returned CALL result rides the inner call's title flag out —
        // for plain returns the static borrow default would clobber a
        // flag-true result (the WsReadAction TITLE_MISS regression); for
        // `#` returns it would forge ownership over a forwarded borrow.
        // Capture the TLS here, before advice/finally/drop calls can
        // overwrite it; emitReturnFlag re-sets it at the ret.
        if (auto m = module->getCurrentMethod()) {
            if (m->returnsClassPointer()) {
                // MCE: only when the resolved callee actually stores the
                // flag — a raw-IR synthesized body leaves a STALE value in
                // the TLS (the sweep-4 JsonSynthesizer garbage reads).
                // Closures (CallExpression) always participate post-7.2.5.
                bool calleeParticipates = false;
                if (auto mceRet =
                        dynamic_pointer_cast<MethodCallExpression>(expression)) {
                    MethodPtr callee = mceRet->getResolvedMethod();
                    calleeParticipates = callee && callee->emitsReturnFlag();
                } else if (dynamic_pointer_cast<CallExpression>(expression)) {
                    calleeParticipates = true;
                }
                if (calleeParticipates) {
                    if (llvm::Function* getFlagFn = module->getRuntimeFunction(
                            "__cajeta_return_flag_get")) {
                        returnTitleFlag = builder->CreateCall(
                            getFlagFn, {}, "ret_flag_ride");
                    }
                }
            }
        }
        // slice-spec §6.1 copy hook, return-of-lvalue form: returning a
        // shared-capable VALUE from a field / element (`return this.tag;`)
        // COPIES it out while the owner keeps its stake — retain the source's
        // stakes so the returned copy carries its own. (A returned LOCAL is
        // the move-out case handled by the drop-entry deactivation above;
        // rvalue returns carry their stakes with the bytes.)
        if (val && val->getType()->isPointerTy()
                && (dynamic_pointer_cast<DotExpression>(expression)
                    || dynamic_pointer_cast<ArrayIndexExpression>(expression))) {
            auto exprAst = dynamic_pointer_cast<Expression>(expression);
            if (exprAst) {
                if (!exprAst->getResolvedType()) exprAst->resolveTypes(module);
                auto vClass = dynamic_pointer_cast<CajetaClass>(
                    exprAst->getResolvedType());
                if (vClass && vClass->isValueType()
                        && vClass->isSharedCapableValue()) {
                    vClass->emitValueSharedOp(*builder, val, module,
                        builder->GetInsertBlock()->getModule(),
                        /*retain=*/true);
                }
            }
        }
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
        // Interface returns travel by VALUE per the small-struct ABI:
        // the 24-byte fat pointer is returned as a value, not through
        // a heap pointer. When the source is a named local whose
        // HeapField slot holds a body pointer, we need a double-load:
        // load the slot to get the body pointer, then load the body to
        // get the fat-pointer value the return signature expects.
        //
        // The double-load path must fire BEFORE the general AllocaInst
        // load below, which would load the slot once and produce a
        // body-pointer where a struct value is expected.
        if (auto m = module->getCurrentMethod()) {
            CajetaTypePtr byValRet;
            if (auto ifaceRet = dynamic_pointer_cast<CajetaClass>(m->getReturnType())) {
                if (ifaceRet->isInterface()) byValRet = ifaceRet;
            }
            if (byValRet) {
                // Two shapes route through the double-load:
                //   (a) Named interface local: field type is the
                //       interface, the HeapField slot holds a pointer
                //       to the 24-byte body alloca.
                //   (b) `this` on an interface method: ThisExpression
                //       resolves through a ParameterField also named
                //       "this" whose type is "pointer"; the slot holds
                //       the caller-supplied body pointer.
                std::string fieldName;
                bool tryDoubleLoad = false;
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                    fieldName = idExpr->getTextValue();
                    auto scope = module->getScopeStack().peek();
                    if (scope) {
                        if (auto field = scope->getField(fieldName)) {
                            auto fldClass = dynamic_pointer_cast<CajetaClass>(field->getType());
                            bool isInterfaceLocal = fldClass && fldClass->isInterface();
                            bool isThisParam = (fieldName == "this");
                            if (isInterfaceLocal || isThisParam) {
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
                            emitTryFinallyUnwind(module);   // C1
                            emitScopeExitToWatermark(module);
                            if (auto curM = module->getCurrentMethod())
                                curM->emitOwnerDrops(module);
                            emitReturnFlag(module);
                            return builder->CreateRet(val);
                        }
                    }
                }
            }
        }

        // Load if the expression returned an l-value (alloca, array-slot GEP, or
        // struct/class field GEP) — return wants a value, not an address.
        // val may be nullptr when the inner expression failed to lower (e.g.
        // a chained MCE whose method lookup didn't pin a return type); guard
        // dyn_cast which asserts on null inputs.
        if (val && llvm::dyn_cast<llvm::AllocaInst>(val)) {
            auto* a = llvm::cast<llvm::AllocaInst>(val);
            val = builder->CreateLoad(a->getAllocatedType(), a);
        } else if (auto idx = dynamic_pointer_cast<ArrayIndexExpression>(expression)) {
            // ArrayIndex returned a slot address. Element type determines the load size:
            // primitive elements load their own LLVM type; reference elements load `ptr`.
            CajetaTypePtr elemType = idx->getResolvedType();
            if (elemType) {
                auto elemClass = dynamic_pointer_cast<CajetaClass>(elemType);
                bool elemIsInterface = elemClass && elemClass->isInterface();
                // A value-type (record / @ValueType) element is stored INLINE;
                // return it by copy (load the body struct), never as a pointer.
                // Value types carry STRUCT_FLAG, so check before that rule (the
                // `return pts[i]` shape for a value-type element array).
                bool elemIsValueType = elemClass && elemClass->isValueType();
                llvm::Type* loadTy;
                if (elemIsInterface || elemIsValueType) {
                    // Interface element: the GEP points AT the inline 24-byte
                    // fat-pointer body, and the by-value return ABI wants that
                    // body STRUCT. Load it directly (a single load). The
                    // reference-element path (loadTy=ptr) would load only the
                    // data word, and the return coercion below would then deref
                    // that as a body → garbage vtable → crash. (`return
                    // this.data[i]` in ArrayList<SomeInterface>.get is the case.)
                    // A value-type element loads its inline body the same way.
                    loadTy = elemType->getLlvmType();
                } else if (dynamic_pointer_cast<CajetaArray>(elemType) ||
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
                            // Class-pass-by-pointer rule (mirrors
                            // BinaryOpExpression's slotTy decision for
                            // assignment-LHS dotted fields): a class-
                            // typed or array-typed field is stored as
                            // `ptr` in the layout, not the inline
                            // struct body. Loading with the inline
                            // type reads too many bytes and the
                            // result mismatches the declared return.
                            // Views (zero-copy overlays) and
                            // interfaces (24-byte fat pointers) keep
                            // inline storage, so they fall through to
                            // getLlvmType().
                            auto foundCls = dynamic_pointer_cast<CajetaClass>(found->getType());
                            bool foundIsView = dynamic_pointer_cast<CajetaView>(found->getType()) != nullptr;
                            bool foundIsArray = dynamic_pointer_cast<CajetaArray>(found->getType()) != nullptr;
                            bool foundIsInterface = foundCls && foundCls->isInterface();
                            // A value-type (record / @ValueType) field is stored
                            // INLINE (its body sub-aggregate), not as a `ptr` — so
                            // load it as its body type and return the struct value
                            // by copy (the isValueTypeR ABI). Loading it as a
                            // pointer would return the first 8 bytes of the value
                            // and the caller would dereference them (the
                            // HashMap<K, value-type V>.get `return slots[i].val`
                            // crash).
                            bool foundIsValueType = foundCls && foundCls->isValueType();
                            llvm::Type* lt;
                            if (foundIsArray
                                    || (foundCls && !foundIsView && !foundIsInterface
                                        && !foundIsValueType)) {
                                lt = llvm::PointerType::get(
                                    *module->getLlvmContext(), 0);
                            } else {
                                lt = found->getType()->getLlvmType();
                            }
                            if (lt) {
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
        // L-03 polymorphism / MI upcast at return site. When the method's
        // declared return type is an ancestor of the returned expression's
        // class type AND the ancestor's sub-object sits at non-zero offset
        // (non-first-parent path), shift the returned pointer to that
        // sub-object's start so the caller's binding to the declared
        // return type lands on the right secondary vtable + field
        // offsets. Mirrors the LocalVariableDeclaration upcast (Phase 1
        // poly-MI) and the BinaryOpExpression assignment upcast — same
        // helper, applied at the return-site write.
        if (expression && val) {
            if (!expression->getResolvedType()) expression->resolveTypes(module);
            auto srcClass = dynamic_pointer_cast<CajetaClass>(
                expression->getResolvedType());
            CajetaTypePtr dstType;
            if (auto m = module->getCurrentMethod()) {
                dstType = m->getReturnType();
            }
            auto dstClass = dynamic_pointer_cast<CajetaClass>(dstType);
            if (srcClass && dstClass
                    && srcClass.get() != dstClass.get()
                    && !srcClass->isInterface()
                    && !dstClass->isInterface()) {
                val = CajetaClass::adjustForUpcast(
                    module, val, srcClass, dstClass);
            }
        }

        // Coerce to the enclosing function's return type. IntegerLiteralExpression picks
        // the smallest-fitting width and Cajeta otherwise lacks an upfront-promotion pass,
        // so cast/extend at the boundary so the return inst matches the function type.
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        llvm::Type* retTy = fn->getReturnType();
        if (!val) {
// A value-less `return;` already took the CreateRetVoid path above,
            // so reaching here means the source DID name an expression and that
            // expression lowered to nothing. That is an unresolved expression,
            // not a null value — emitting `ret null` here silently miscompiles it.
            // (Throwing also means no ret is emitted, so no return flag either.)
            throw locatedException(
                expression->getSourceLine(), expression->getSourceColumn() + 1,
                "returned expression did not resolve to a value (a sub-expression"
                " produced nothing — e.g. a method or member that does not exist"
                " on the receiver's type)",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        // A `void` call is present but valueless, so the guard above misses it and
        // `return D.nothing();` in a value-returning method compiled SILENTLY (1.2.3).
        if (!retTy->isVoidTy() && expression->getResolvedType()
                && expression->getResolvedType()->toCanonical() == "void") {
            throw locatedException(
                expression->getSourceLine(), expression->getSourceColumn() + 1,
                "returned expression is 'void', but the method returns a value",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        // owned-interface-return-fault — WRAP a concrete class into the
        // interface's fat body. MUST run before the aggregate coercion below.
        //
        // An interface value is a 24-byte `{ ptr data, ptr vtable, i64 kind }`
        // returned BY VALUE. Every other way of producing one builds that body:
        // `Face f = heap Plus(k)` wraps in LocalVariableDeclaration, and
        // interface-typed slots and fields load their inline body. `return heap
        // Plus(k)` through a `#Face` return had no wrap, and fell into the
        // `retTy->isAggregateType() && valTy->isPointerTy()` arm below — which
        // means "the expression yielded the ADDRESS of the aggregate, load it".
        // That is right for an @ValueType (`return stack Vec2(...)`) and wrong
        // here: the pointer is the Plus OBJECT, not a body. It emitted
        //
        //     %2 = call ptr @malloc(i64 16)          ; Plus is 16 bytes
        //     %4 = load %test.Face, ptr %2           ; reads 24
        //     ret %test.Face %4
        //
        // so the caller got data = Plus's vtable pointer, vtable = Plus's `k`
        // field, and kind = 8 bytes of heap past the allocation. Dispatch then
        // loaded through `k + 8` — with k = 10 that is address 0x12, the exact
        // reported fault.
        //
        // The `#BaseClass` sibling is unaffected: it returns a thin pointer and
        // needs no wrap, which is why it passed while this faulted — the
        // discriminator placing the defect in fat-value handling rather than
        // return-slot lifetime (spec 4.3).
        if (retTy->isAggregateType() && val->getType()->isPointerTy()) {
            if (auto m = module->getCurrentMethod()) {
                auto ifaceRet =
                    dynamic_pointer_cast<CajetaClass>(m->getReturnType());
                if (expression && !expression->getResolvedType()) {
                    expression->resolveTypes(module);
                }
                auto srcCls = dynamic_pointer_cast<CajetaClass>(
                    expression ? expression->getResolvedType() : nullptr);
                if (ifaceRet && ifaceRet->isInterface() && srcCls
                        && !srcCls->isInterface()
                        && retTy == ifaceRet->getLlvmType()) {
                    auto& lctx = *module->getLlvmContext();
                    llvm::Type* ptrTy = llvm::PointerType::get(lctx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(lctx);
                    llvm::Value* body = builder->CreateAlloca(retTy);
                    builder->CreateStore(val,
                        builder->CreateStructGEP(retTy, body, 0, "iface_data"));
                    std::string ifaceCanonical =
                        ifaceRet->getQName()->toCanonical();
                    llvm::Constant* vtableRef = nullptr;
                    if (auto gv = srcCls->getInterfaceVTable(ifaceCanonical)) {
                        vtableRef = CajetaModule::ensureGlobalInModule(
                            module->emitTargetLlvmModule(), gv);
                    }
                    if (!vtableRef) {
                        vtableRef = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                    }
                    builder->CreateStore(vtableRef,
                        builder->CreateStructGEP(retTy, body, 1,
                                                 "iface_vtable"));
                    // A `#Interface` return hands ownership out, and so does a
                    // fresh construction or an explicit `#x` through a plain
                    // return type. Anything else leaves the callee owning it.
                    bool ownedOut = m->isReturnsOwnership()
                        || dynamic_pointer_cast<NewExpression>(expression)
                        || dynamic_pointer_cast<MoveExpression>(expression);
                    builder->CreateStore(
                        llvm::ConstantInt::get(i64Ty,
                            (uint64_t) (ownedOut ? IFACE_KIND_OWNED_CLASS
                                                 : IFACE_KIND_BORROWED_CLASS)),
                        builder->CreateStructGEP(retTy, body, 2, "iface_kind"));
                    // By-value return ABI: hand back the body STRUCT. The
                    // alloca dies with this frame; the struct does not.
                    val = builder->CreateLoad(retTy, body);
                }
            }
        }

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
            } else if (retTy->isAggregateType() && valTy->isPointerTy()) {
                // By-value aggregate return (e.g. an @ValueType `Vec2 operator+`):
                // Method::generatePrototype set the function's return type to the
                // struct itself, but the returned expression (`return stack
                // Vec2(...)`) yielded a POINTER to it. Load the struct so the `ret`
                // matches the by-value signature. See value-type-overloading-plan.md.
                val = builder->CreateLoad(retTy, val);
            } else if ((retTy->isFloatingPointTy() || retTy->isIntegerTy())
                    && valTy->isPointerTy()) {
                // Scalar return whose expression yielded an l-value ADDRESS rather
                // than the loaded value — e.g. `return this.x;` or a ternary of
                // field accesses `(c) ? this.x : this.y` (the branches are field
                // GEP pointers, the phi is a pointer). A scalar return type with a
                // pointer operand can only mean "load the scalar from that
                // address." Surfaced by @ValueType `operator[]` read (S3).
                val = builder->CreateLoad(retTy, val);
            }
            // Remaining pointer/aggregate mismatches fall through; verifier flags.
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
        emitTryFinallyUnwind(module);   // C1: run enclosing try finallys + pop frames
        emitScopeExitToWatermark(module);
        // Fire drops before the typed return so all owned locals are released.
        if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
        // 5.2.2 — `return #x`: a runtime-owner source forwards its captured
        // flag; a static owner moved out is an unconditional surrender.
        if (auto mvRet = dynamic_pointer_cast<MoveExpression>(expression)) {
            returnTitleFlag = mvRet->getRuntimeTitleFlag()
                ? mvRet->getRuntimeTitleFlag()
                : (llvm::Value*) builder->getInt64(1);
            // mode-carrying-claim §5.4 — a `#R`-declared return is a CONTRACT:
            // the caller is promised a title. A runtime flag of 0 here means
            // the frame only ever held a borrow (the mode-carrying claim
            // forwarded a borrowed slot's bit, or a plain formal's word bit
            // was 0), and silently returning it would hand out a forged
            // title — the caller's static-owner claim frees the real owner's
            // value. Panic TITLE_MISS instead (`operator#[]` on a borrowed /
            // already-extracted slot). `return #= x` (modeCarrying) is the
            // sanctioned escape: it declares the return carries the mode.
            if (auto mM = module->getCurrentMethod()) {
                if (mM->isReturnsOwnership() && !modeCarrying
                        && returnTitleFlag
                        && !llvm::isa<llvm::ConstantInt>(returnTitleFlag)) {
                    auto& rctx = *module->getLlvmContext();
                    llvm::Value* hasTitle = builder->CreateICmpNE(
                        returnTitleFlag,
                        llvm::ConstantInt::get(returnTitleFlag->getType(), 0),
                        "ret_contract_ok");
                    llvm::Function* rfn =
                        builder->GetInsertBlock()->getParent();
                    auto* panicBB = llvm::BasicBlock::Create(
                        rctx, "ret_title_panic", rfn);
                    auto* okBB = llvm::BasicBlock::Create(
                        rctx, "ret_title_ok", rfn);
                    builder->CreateCondBr(hasTitle, okBB, panicBB);
                    builder->SetInsertPoint(panicBB);
                    // CAJETA_PANIC_TITLE_MISS = 3, integer-throw shape
                    // (< 4096 ⇒ first catch clause binds it) — same code the
                    // claim's own take sites use.
                    if (llvm::Function* throwFn =
                            module->getRuntimeFunction("__cajeta_throw")) {
                        llvm::Value* code = builder->CreateIntToPtr(
                            llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(rctx), 3),
                            llvm::PointerType::get(rctx, 0));
                        builder->CreateCall(throwFn, {code});
                    }
                    builder->CreateUnreachable();
                    builder->SetInsertPoint(okBB);
                }
            }
        }
        // 6.2.2 — `return Cajeta.flagged(v, owned)`: the container's own
        // bookkeeping decides the flag.
        if (auto mcRet = dynamic_pointer_cast<MethodCallExpression>(expression)) {
            if (llvm::Value* f = mcRet->getFlaggedTitleValue()) {
                returnTitleFlag = f;
            }
        }
        // 7.2.5 — lambda-body returns: classify by shape. A fresh
        // construction hands out a title; a returned CALL result lets the
        // inner call's flag ride through untouched; anything else is the
        // borrow default emitReturnFlag emits in lambda mode.
        if (!module->getCurrentMethod() && module->isLambdaClassPtrReturn()
                && !returnTitleFlag) {
            if (dynamic_pointer_cast<NewExpression>(expression)) {
                returnTitleFlag = builder->getInt64(1);
            } else if (dynamic_pointer_cast<MethodCallExpression>(expression)
                    || dynamic_pointer_cast<CallExpression>(expression)) {
                return builder->CreateRet(val);
            }
        }
        if (cajeta::ownership::ReturnTitleAudit::enabled()) {
            auditReturnTitle(module, returnTitleFlag, expression,
                             getSourceLine());
        }
        emitReturnFlag(module, returnTitleFlag);
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
        const auto* lc =
            label.empty() ? nullptr : module->findLoopContext(label);
        if (!lc) lc = &module->currentLoopContext();
        // C1 follow-up: run + pop the finallys of any try bodies entered inside
        // the loop before jumping out past them.
        emitTryFinallyUnwind(module, lc->tryFinallyDepth);
        builder->CreateBr(lc->breakTarget);
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
        const auto* lc =
            label.empty() ? nullptr : module->findLoopContext(label);
        if (!lc) lc = &module->currentLoopContext();
        // C1 follow-up: run + pop in-loop try finallys before jumping to the latch.
        emitTryFinallyUnwind(module, lc->tryFinallyDepth);
        builder->CreateBr(lc->continueTarget);
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

    // 7.2.4 — analysis-walk descent overrides. Statements park their
    // payloads in private fields, not `children` (the codegen list); these
    // hand each payload to the visitor so analysis passes see every
    // subtree. Each also forwards to the base for any `children` content.
    using SubNodeFn = std::function<void(const AbstractSyntaxNodePtr&)>;

    void ExpressionStatement::forEachSubNode(const SubNodeFn& fn) {
        if (expression) fn(expression);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void LabelStatement::forEachSubNode(const SubNodeFn& fn) {
        if (block) fn(block);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void ScopeStatement::forEachSubNode(const SubNodeFn& fn) {
        if (block) fn(block);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void IfStatement::forEachSubNode(const SubNodeFn& fn) {
        if (condition) fn(condition);
        if (thenBranch) fn(thenBranch);
        if (elseBranch) fn(elseBranch);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void ForStatement::forEachSubNode(const SubNodeFn& fn) {
        if (init) fn(init);
        if (condition) fn(condition);
        for (auto& u : update) { if (u) fn(u); }
        if (body) fn(body);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void EnhancedForStatement::forEachSubNode(const SubNodeFn& fn) {
        if (iterableExpr) fn(iterableExpr);
        if (body) fn(body);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void WhileStatement::forEachSubNode(const SubNodeFn& fn) {
        if (condition) fn(condition);
        if (body) fn(body);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void DoStatement::forEachSubNode(const SubNodeFn& fn) {
        if (body) fn(body);
        if (condition) fn(condition);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void TryStatement::forEachSubNode(const SubNodeFn& fn) {
        if (tryBlock) fn(tryBlock);
        for (auto& cc : catchClauses) { if (cc.body) fn(cc.body); }
        if (finallyBlock) fn(finallyBlock);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void SwitchStatement::forEachSubNode(const SubNodeFn& fn) {
        if (subject) fn(subject);
        for (auto& g : groups) {
            for (auto& cv : g.caseValues) { if (cv) fn(cv); }
            for (auto& st : g.statements) { if (st) fn(st); }
        }
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void ReturnStatement::forEachSubNode(const SubNodeFn& fn) {
        if (expression) fn(expression);
        AbstractSyntaxNode::forEachSubNode(fn);
    }

    void ThrowStatement::forEachSubNode(const SubNodeFn& fn) {
        if (expression) fn(expression);
        AbstractSyntaxNode::forEachSubNode(fn);
    }
}