//
// Created by James Klappenbach on 2/19/22.
//

#include "Statement.h"
#include "expression/Expression.h"
#include "expression/MethodCallExpression.h"
#include "expression/CallExpression.h"
#include "expression/BinaryOpExpression.h"
#include "expression/Identifier.h"
#include "expression/DotExpression.h"
#include "expression/LiteralExpression.h"
#include "expression/NewExpression.h"
#include <functional>
#include "expression/AggregateInitializerExpression.h"
#include "../compile/CajetaModule.h"
#include "../compile/ExcFrameSetjmp.h"
#include "../compile/ScriptUnitSynthesis.h"
#include "cajeta/dbg/DebugCodegen.h"
#include "cajeta/dbg/LineInfoCodegen.h"
#include "cajeta/prof/ProfileCodegen.h"
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
#include "cajeta/ownership/TitleClassifier.h"

/** Statement AST nodes: one class per `statement` grammar form, each built from
 *  its parse context and lowered to LLVM IR. */
namespace cajeta {

    static BlockStatementPtr buildBlockStatement(CajetaParser::BlockStatementContext* ctx);

    // Builds an Initializer from a `variableInitializer` context, recursing for
    // nested array literals. Module-free, so the Statement path (kernel bodies,
    // FOR-decls) can use it; returns nullptr for a malformed/empty initializer.
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

    // Builds a LocalVariableDeclaration from a parse context (nested blocks, FOR
    // inits). Types resolve without a module, so array types in nested
    // declarations are out of reach here; returns nullptr on malformed input.
    static shared_ptr<LocalVariableDeclaration>
    buildLocalVariableDeclaration(CajetaParser::LocalVariableDeclarationContext* lvdCtx) {
        if (!lvdCtx) return nullptr;
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
        CajetaTypePtr type = CajetaType::fromContext(lvdCtx->typeType(), nullptr);
        list<VariableDeclaratorPtr> declarators;
        if (auto* vdsCtx = lvdCtx->variableDeclarators()) {
            for (auto* vdCtx : vdsCtx->variableDeclarator()) {
                InitializerPtr initializer =
                    buildVariableInitializer(vdCtx->variableInitializer());
                if (vdCtx->SHARP_ASSIGN() != nullptr && initializer != nullptr) {
                    if (auto* viCtx = vdCtx->variableInitializer()) {
                        if (auto* eCtx = viCtx->expression()) {
                            // `T x #= #v` — `#=` already carries the source's
                            // mode, so the second `#` warns instead of erroring.
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
                                // MODE-CARRYING, exactly as the assignment form
                                // marks it (Expression.cpp): `#=` forwards the
                                // source's mode and claims no title, so the
                                // transfer-of-a-borrow rejection must not fire
                                // here either. Without this flag the declaration
                                // `T c #= b` rejected a borrow that the
                                // assignment `c #= b` accepted.
                                mv->setModeCarrying(true);
                                mv->addChild(inner);
                                initializer = make_shared<VariableInitializer>(
                                    mv, vdCtx->variableInitializer()->getStart());
                            }
                        }
                    }
                } else if (initializer != nullptr) {
                    markLegacyTransferAssign(initializer);
                }
                string identName = vdCtx->variableDeclaratorId()->identifier()->getText();
                int arrayDim = static_cast<int>(vdCtx->variableDeclaratorId()->LBRACK().size());
                declarators.push_back(make_shared<VariableDeclarator>(
                    identName, /*isReference=*/false, arrayDim, initializer, vdCtx->getStart()));
            }
        }
        return make_shared<LocalVariableDeclaration>(
            modifiers, type, declarators, lvdCtx->getStart());
    }

    // Builds a Block from a BlockContext: statements and local variable
    // declarations only — local type declarations are skipped.
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

    // Builds one block child; returns nullptr for a local type declaration.
    static BlockStatementPtr buildBlockStatement(CajetaParser::BlockStatementContext* ctx) {
        if (auto* lvdCtx = ctx->localVariableDeclaration()) {
            return buildLocalVariableDeclaration(lvdCtx);
        }
        if (auto* stmtCtx = ctx->statement()) {
            return Statement::fromContext(stmtCtx);
        }
        return nullptr;
    }

    // Builds the Statement for one `statement` parse context. Keyword forms are
    // tested before the bare-block form: TRY/SWITCH/FOR/WHILE/DO/IF/SCOPE each
    // contain a block sub-rule, so `ctx->block()` is non-null for them too.
    StatementPtr Statement::fromContext(CajetaParser::StatementContext* ctx) {
        StatementPtr result;

        antlr4::Token* token = ctx->getStart();

        if (ctx->IF()) {
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
            ExpressionPtr cond = ctx->parExpression()
                ? Expression::fromContext(ctx->parExpression()->expression())
                : nullptr;
            StatementPtr body = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            result = make_shared<WhileStatement>(token, cond, body);
        } else if (ctx->DO()) {
            StatementPtr body = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            ExpressionPtr cond = ctx->parExpression()
                ? Expression::fromContext(ctx->parExpression()->expression())
                : nullptr;
            result = make_shared<DoStatement>(token, body, cond);
        } else if (ctx->SWITCH()) {
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
            BlockPtr tryBlk = ctx->block() ? buildBlock(ctx->block()) : nullptr;
            std::vector<CatchClause> catches;
            for (auto* ccCtx : ctx->catchClause()) {
                CatchClause c;
                // Multi-catch is unsupported: only the first qualifiedName is taken.
                if (auto* catchType = ccCtx->catchType()) {
                    if (!catchType->qualifiedName().empty()) {
                        string typeName = catchType->qualifiedName(0)->getText();
                        // Parse-time pre-seed only; resolveTypes re-resolves the
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
            result = make_shared<ReturnStatement>(
                token, returnExpr, ctx->SHARP_ASSIGN() != nullptr);
        } else if (ctx->THROW()) {
            ExpressionPtr throwExpr = ctx->expression()
                ? Expression::fromContext(ctx->expression())
                : nullptr;
            result = make_shared<ThrowStatement>(token, throwExpr);
        } else if (ctx->BREAK()) {
            string label = ctx->identifier() ? ctx->identifier()->getText() : "";
            result = make_shared<BreakStatement>(token, std::move(label));
        } else if (ctx->CONTINUE()) {
            string label = ctx->identifier() ? ctx->identifier()->getText() : "";
            result = make_shared<ContinueStatement>(token, std::move(label));
        } else if (ctx->YIELD()) {
            result = make_shared<YieldStatement>(token);
        } else if (ctx->SCOPE()) {
            BlockPtr blk = ctx->block() ? buildBlock(ctx->block()) : nullptr;
            result = make_shared<ScopeStatement>(token, blk);
        } else if (ctx->block()) {
            result = make_shared<LabelStatement>(token, buildBlock(ctx->block()));
        } else if (ctx->expression()) {
            result = make_shared<ExpressionStatement>(Expression::fromContext(ctx->expression()), token);
        } else if (ctx->statementExpression) {
        } else if (ctx->switchExpression()) {
        } else if (ctx->identifierLabel) {
            string label = ctx->identifierLabel->getText();
            StatementPtr inner = ctx->statement().empty()
                ? nullptr
                : Statement::fromContext(ctx->statement(0));
            result = make_shared<IdentifierLabel>(token, std::move(label), inner);
        } else if (ctx->SEMI()) {
        }

        return result;
    }

    // Generates an expression in statement position: takes the script unit-result
    // mark, marks a discarded `spawn`, lints a discarded wildcard result, and
    // drops a class-pointer result whose callee surrendered a title.
    llvm::Value* ExpressionStatement::generateCode(CajetaModulePtr module) {
        bool unitResult = module->takeScriptResultPending();
        if (auto sp = dynamic_pointer_cast<SpawnExpression>(expression)) {
            sp->setDiscardedMode(true);
        }
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
                            std::ostringstream w;
                            w << "warning: [discarded-wildcard-next] "
                                << "call to '" << name
                                << "' on wildcard-typed receiver "
                                << recvClass->toCanonical()
                                << " in statement position in "
                                << currentMethod->getName()
                                << " — the result (a heap Optional<?>) is "
                                << "discarded; remove the call if you don't "
                                << "need its value, or bind the result and "
                                << "act on it. Suppress with "
                                << "@SuppressLint(\"discarded-wildcard-next\").\n";
                            logLine("warn", w.str());
                        }
                    }
                }
            }
        }
        llvm::Value* stmtVal = expression
            ? expression->generateCode(module) : nullptr;
        // Render the cell's result BEFORE the discard below: a flagged return is
        // about to be dropped, and reading it afterwards renders freed memory.
        if (unitResult) emitScriptUnitResult(module, expression, stmtVal);
        if (stmtVal && stmtVal->getType()->isPointerTy()) {
            if (auto mceD = dynamic_pointer_cast<MethodCallExpression>(
                    expression)) {
                MethodPtr rm = mceD->getResolvedMethod();
                // Only a callee that actually STORES the flag: one that never
                // touches the TLS leaves a STALE bit from a prior call, and
                // reading it drops a value the callee never surrendered.
                if (rm && rm->returnsClassPointer() && rm->emitsReturnFlag()) {
                    auto* b = module->getBuilder();
                    llvm::Function* getFlag = module->getRuntimeFunction(
                        "__cajeta_return_flag_get");
                    // Drop by the STATIC return type: virtual_drop on an ARRAY
                    // header would read its first word as a vtable.
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

    void ScopeStatement::resolveTypes(CajetaModulePtr module) {
        if (block) block->resolveTypes(module);
    }

    // Lowers `scope { ... }`: enters a runtime scope frame, generates the block,
    // then exits. scope_exit is emitted BEFORE the block's drops — a Task drop
    // would free the task whose exception slot scope_exit reads.
    llvm::Value* ScopeStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (llvm::Function* enterFn = module->getRuntimeFunction(
                "__cajeta_scope_enter")) {
            builder->CreateCall(enterFn, {});
        }
        auto m = module->getCurrentMethod();
        if (m) m->pushDropFrame();
        if (block) {
            for (auto& child : block->getChildren()) {
                child->generateCode(module);
            }
        }
        // Only when the body didn't terminate; an early return exits through the
        // method's own cleanup path. TODO: integrate scope_exit there.
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

    // Evaluates a control-flow condition to i1, loading through l-values. A null
    // `cond` means "always true" (`for (;;)`); an unresolved or `void` condition
    // throws rather than folding to a constant.
    static llvm::Value* evalCondition(CajetaModulePtr module, ExpressionPtr cond) {
        auto* builder = module->getBuilder();
        llvm::Type* i1Ty = llvm::Type::getInt1Ty(*module->getLlvmContext());
        if (!cond) {
            return llvm::ConstantInt::getTrue(*module->getLlvmContext());
        }
        llvm::Value* v = cond->generateCode(module);
        if (!v) {
            throw locatedException(
                cond->getSourceLine(), cond->getSourceColumn() + 1,
                "condition did not resolve to a value (a sub-expression produced"
                " nothing — e.g. a method or member that does not exist on the"
                " receiver's type)",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
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
        // Non-alloca address forms (array-slot and field GEPs) come back as
        // pointers: load through, or the ICmp below compares a pointer with an
        // integer zero and the verifier rejects it.
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

    // Lowers `if`/`else` into then/else/merge blocks, merging the definite-
    // assignment and move state of the two arms at the join.
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

        auto scope = module->getScopeStack().peek();
        std::set<std::string> preIfNYA;
        if (scope) preIfNYA = scope->snapshotNotYetAssigned();
        // The arms are exclusive though codegen is sequential: the else arm must
        // see the PRE-IF move state, and the join takes the union.
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

    // Lowers `while` into head/body/exit blocks. The body may run zero times, so
    // its definite-assignment deltas are restored at the exit.
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

        auto scope = module->getScopeStack().peek();
        std::set<std::string> preBodyNYA;
        if (scope) preBodyNYA = scope->snapshotNotYetAssigned();

        builder->SetInsertPoint(bodyBB);
        module->pushLoopContext(headBB, exitBB,
            (module->getCurrentMethod() ? module->getCurrentMethod()->dropFrameCount() : 0));
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

    // Lowers a C-style `for`. `continue` targets the update block, not the head,
    // so the update runs before the next condition test; the body may run zero
    // times, so its definite-assignment deltas are restored at the exit.
    llvm::Value* ForStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        if (init) init->generateCode(module);

        llvm::BasicBlock* headBB = llvm::BasicBlock::Create(ctx, "for_head", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx, "for_body", parentFn);
        llvm::BasicBlock* updateBB = llvm::BasicBlock::Create(ctx, "for_update", parentFn);
        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx, "for_exit", parentFn);

        builder->CreateBr(headBB);

        builder->SetInsertPoint(headBB);
        llvm::Value* condVal = evalCondition(module, condition);
        builder->CreateCondBr(condVal, bodyBB, exitBB);

        auto scope = module->getScopeStack().peek();
        std::set<std::string> preBodyNYA;
        if (scope) preBodyNYA = scope->snapshotNotYetAssigned();

        builder->SetInsertPoint(bodyBB);
        module->pushLoopContext(updateBB, exitBB,
            (module->getCurrentMethod() ? module->getCurrentMethod()->dropFrameCount() : 0));
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

    // Lowers `do ... while`. `continue` targets the tail (the condition test),
    // which branches back to the body or out.
    llvm::Value* DoStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx, "do_body", parentFn);
        llvm::BasicBlock* tailBB = llvm::BasicBlock::Create(ctx, "do_tail", parentFn);
        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx, "do_exit", parentFn);

        builder->CreateBr(bodyBB);

        builder->SetInsertPoint(bodyBB);
        module->pushLoopContext(tailBB, exitBB,
            (module->getCurrentMethod() ? module->getCurrentMethod()->dropFrameCount() : 0));
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

    // Lowers `for (T x : arr)` and the extended `for (int i, T x : arr)` form.
    // Array iterables only; the size is read once at entry with the shared-state
    // sign bit masked off, so a shared buffer's count is not seen as negative.
    llvm::Value* EnhancedForStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (!iterableExpr || !elementType || elementName.empty()) {
            return nullptr;
        }

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

        llvm::AllocaInst* idxSlot = builder->CreateAlloca(i64Ty, nullptr, "fe_idx");
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxSlot);

        bool elemIsPrimitive = (elementType->getTypeFlags() & PRIMITIVE_FLAG) != 0;
        bool elemIsArray = dynamic_pointer_cast<CajetaArray>(elementType) != nullptr;
        llvm::Type* elemSlotTy = (elemIsPrimitive && !elemIsArray)
            ? elementType->getLlvmType()
            : llvm::PointerType::get(ctx, 0);
        llvm::AllocaInst* elemSlot = builder->CreateAlloca(elemSlotTy, nullptr, elementName);

        llvm::AllocaInst* iterSlot = nullptr;
        if (iteratorType && !iteratorName.empty()) {
            iterSlot = builder->CreateAlloca(iteratorType->getLlvmType(), nullptr, iteratorName);
        }

        auto scope = module->getScopeStack().peek();
        auto elemField = make_shared<StackField>(module, elementName, elementType);
        elemField->setAllocation(elemSlot);
        scope->putField(elemField);
        if (iterSlot) {
            auto iterField = make_shared<StackField>(module, iteratorName, iteratorType);
            iterField->setAllocation(iterSlot);
            scope->putField(iterField);
        }

        llvm::Value* sizePtr = builder->CreateStructGEP(hdrTy, arrayVal,
            CajetaArray::SIZE_FIELD_INDEX, "size");
        llvm::Value* sizeVal = builder->CreateLoad(i64Ty, sizePtr, "size_v");
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
        // One 3-index GEP on the header `{ i64 size, [0 x T] data }` — the byte
        // offset the runtime's heap layout assumes.
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Value* elemPtr = builder->CreateGEP(hdrTy, arrayVal,
            {llvm::ConstantInt::get(i64Ty, 0),
             llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
             idxVal}, "elem_ptr");
        llvm::Value* elemVal = builder->CreateLoad(elemSlotTy, elemPtr, "elem");
        builder->CreateStore(elemVal, elemSlot);
        if (iterSlot) {
            llvm::Value* idxCast = idxVal;
            llvm::Type* itTy = iteratorType->getLlvmType();
            if (itTy != i64Ty && itTy->isIntegerTy()) {
                idxCast = builder->CreateIntCast(idxVal, itTy, /*isSigned=*/true);
            }
            builder->CreateStore(idxCast, iterSlot);
        }

        module->pushLoopContext(updateBB, exitBB,
            (module->getCurrentMethod() ? module->getCurrentMethod()->dropFrameCount() : 0));
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

    // Resolves the try, catch and finally subtrees, and re-resolves each catch
    // type AS WRITTEN through the scoped tier discipline: the parse-time pre-seed
    // can miss a sibling/archive class, and an unresolved clause is no catch-all.
    void TryStatement::resolveTypes(CajetaModulePtr module) {
        if (tryBlock) tryBlock->resolveTypes(module);
        for (auto& c : catchClauses) {
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

    // Lowers try/catch/finally on setjmp/longjmp: pushes a runtime exception
    // frame, setjmps, and on a throw dispatches to the first catch clause whose
    // type matches the thrown object's vtable chain, else re-raises.
    llvm::Value* TryStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

        llvm::Function* push = module->getRuntimeFunction("__cajeta_exc_push");
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
        llvm::Function* getThrown = module->getRuntimeFunction("__cajeta_get_thrown");
        if (!push || !pop || !getThrown) {
            if (tryBlock) tryBlock->generateCode(module);
            return nullptr;
        }

        // Allocated at function entry (setjmp captures the SP). 512 bytes covers
        // jmp_buf + prev + thrown_value; 16-byte aligned because MSVCRT's _setjmp
        // stores XMM registers with aligned stores (ExcFrameSetjmp.h).
        constexpr unsigned frameBytes = 512;
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::AllocaInst* frameAlloca = entryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, frameBytes));
        frameAlloca->setAlignment(llvm::Align(16));
        llvm::Value* framePtr = frameAlloca;

        llvm::BasicBlock* tryBB = llvm::BasicBlock::Create(ctx, "try_body", parentFn);
        llvm::BasicBlock* catchBB = llvm::BasicBlock::Create(ctx, "try_catch", parentFn);
        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(ctx, "try_after", parentFn);

        builder->CreateCall(push, {framePtr});
        // Capture the scope-chain top at try-entry so a throw caught here unwinds
        // the body's `scope { spawn ... }` children. Held in an alloca so it
        // survives the setjmp/longjmp.
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
        // The capture call is per-object-format (plain setjmp vs COFF's
        // non-unwinding _setjmp(frame, NULL)) — see ExcFrameSetjmp.h.
        llvm::Value* sjResult = emitExcFrameSetjmp(*builder, framePtr);
        llvm::Value* threw = builder->CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        builder->CreateCondBr(threw, catchBB, tryBB);

        auto daScope = module->getScopeStack().peek();
        std::set<std::string> preTryNYA;
        if (daScope) preTryNYA = daScope->snapshotNotYetAssigned();

        builder->SetInsertPoint(tryBB);
        // Catch types are visible to nested call-site lints for the try BODY only:
        // popped before catch-body codegen, so a throw inside a handler is not
        // considered caught by the same try's clauses.
        {
            std::vector<CajetaTypePtr> catchTypes;
            catchTypes.reserve(catchClauses.size());
            for (auto& c : catchClauses) {
                if (c.type) catchTypes.push_back(c.type);
            }
            module->pushTryCatchContext(std::move(catchTypes));
        }
        // The push records this try's finally (null for catch-only) so a return,
        // break or continue escaping the body unwinds it.
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

        if (daScope) daScope->restoreNotYetAssigned(preTryNYA);
        builder->SetInsertPoint(catchBB);
        // Read the thrown value BEFORE popping: the helper reads the topmost
        // frame, and the pop unwinds it.
        llvm::Value* thrownValPtr = builder->CreateCall(getThrown, {});
        builder->CreateCall(pop, {});
        // This frame is popped first, so unwinding the body's open scopes here
        // joins their children, and a re-raise targets the OUTER frame.
        if (scopeWmSlot && scopeExitTo) {
            llvm::Value* wm = builder->CreateLoad(
                llvm::PointerType::get(ctx, 0), scopeWmSlot);
            builder->CreateCall(scopeExitTo, {wm});
        }

        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Function* excMatches = module->getRuntimeFunction("__cajeta_exc_matches");
        llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");

        // Binds the thrown value to a clause's catch variable in the current scope.
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

        // Emits one matched clause's body and returns its normal-completion NYA
        // snapshot. With a finally, the body runs in its own exception frame, so a
        // throw out of it pops that frame, runs the finally, and re-raises.
        auto emitClauseBody = [&](const CatchClause& c) -> std::set<std::string> {
            if (!finallyBlock) {
                if (c.body) c.body->generateCode(module);
                return daScope ? daScope->snapshotNotYetAssigned()
                               : std::set<std::string>();
            }
            llvm::AllocaInst* catchFrame = entryBuilder.CreateAlloca(
                llvm::ArrayType::get(i8Ty, frameBytes));
            catchFrame->setAlignment(llvm::Align(16));
            llvm::BasicBlock* catchBodyBB =
                llvm::BasicBlock::Create(ctx, "catch_body", parentFn);
            llvm::BasicBlock* catchLandBB =
                llvm::BasicBlock::Create(ctx, "catch_finally", parentFn);
            builder->CreateCall(push, {catchFrame});
            llvm::Value* csj = emitExcFrameSetjmp(*builder, catchFrame);
            llvm::Value* cthrew = builder->CreateICmpNE(
                csj, llvm::ConstantInt::get(i32Ty, 0));
            builder->CreateCondBr(cthrew, catchLandBB, catchBodyBB);

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

            builder->SetInsertPoint(catchLandBB);
            llvm::Value* thrown2 = builder->CreateCall(getThrown, {});
            builder->CreateCall(pop, {});
            finallyBlock->generateCode(module);
            if (throwFn && !builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateCall(throwFn, {thrown2});
                builder->CreateUnreachable();
            }
            if (daScope) daScope->restoreNotYetAssigned(afterBodyNYA);
            return afterBodyNYA;
        };

        std::vector<std::set<std::string>> armNYAs;

        if (!catchClauses.empty()) {
            // A legacy `throw 42` is an integer IntToPtr'd into the throw slot,
            // with no vtable to walk: below 4096 it matches the first clause.
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

                // A non-class clause, or one with no vtable, is a catch-all. So
                // are the roots Throwable/Exception — matching them by vtable
                // would dereference a legacy integer throw and SIGSEGV.
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
                    llvm::Value* cond = builder->CreateOr(isLegacyInt, isM);
                    builder->CreateCondBr(cond, bindBB, nextBB);
                } else {
                    builder->CreateBr(bindBB);
                }

                builder->SetInsertPoint(bindBB);
                if (daScope) daScope->restoreNotYetAssigned(preTryNYA);
                bindCatchVar(c);
                std::set<std::string> armNYA = emitClauseBody(c);
                if (daScope) armNYAs.push_back(armNYA);
                if (!builder->GetInsertBlock()->hasTerminator()) {
                    builder->CreateBr(afterBB);
                }

                builder->SetInsertPoint(nextBB);
            }
            if (throwFn) {
                builder->CreateCall(throwFn, {thrownValPtr});
            }
            builder->CreateUnreachable();
        } else {
            // No catch clause: the throw must propagate, so run the finally on
            // this unwinding edge (the grammar guarantees one here) and re-raise.
            if (finallyBlock) finallyBlock->generateCode(module);
            if (throwFn && !builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateCall(throwFn, {thrownValPtr});
                builder->CreateUnreachable();
            } else if (!builder->GetInsertBlock()->hasTerminator()) {
                builder->CreateBr(afterBB);
            }
        }

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

    // Lowers `switch` to an LLVM SwitchInst, one block per group, with implicit
    // fall-through. Integer subjects only. `break` leaves the switch, but
    // `continue` still targets the ENCLOSING loop — a switch is not a loop.
    llvm::Value* SwitchStatement::generateCode(CajetaModulePtr module) {
        if (!subject) return nullptr;
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();

        llvm::Value* subjVal = subject->generateCode(module);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(subjVal)) {
            subjVal = builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (!subjVal || !subjVal->getType()->isIntegerTy()) {
            return nullptr;
        }
        unsigned subjBits = subjVal->getType()->getIntegerBitWidth();

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

        unsigned caseCount = 0;
        for (auto& g : groups) caseCount += static_cast<unsigned>(g.caseValues.size());

        llvm::SwitchInst* sw = builder->CreateSwitch(subjVal, defaultBB, caseCount);

        auto scope = module->getScopeStack().peek();
        std::set<std::string> preSwitchNYA;
        if (scope) preSwitchNYA = scope->snapshotNotYetAssigned();
        std::set<std::string> mergedPostNYA = preSwitchNYA;
        bool hasDefault = false;
        for (auto& g : groups) {
            if (g.isDefault) { hasDefault = true; break; }
        }

        llvm::BasicBlock* enclosingCont =
            module->hasLoopContext()
                ? module->currentLoopContext().continueTarget
                : afterBB;
        module->pushLoopContext(enclosingCont, afterBB,
            (module->getCurrentMethod() ? module->getCurrentMethod()->dropFrameCount() : 0));
        bool anyArmMerged = false;
        for (size_t i = 0; i < groups.size(); i++) {
            auto& g = groups[i];
            for (auto& valExpr : g.caseValues) {
                llvm::Value* v = valExpr->generateCode(module);
                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                    v = builder->CreateLoad(a->getAllocatedType(), a);
                }
                auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v);
                if (!ci) continue;
                if (ci->getBitWidth() != subjBits) {
                    ci = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(
                        llvm::cast<llvm::IntegerType>(subjVal->getType()),
                        ci->getValue().sextOrTrunc(subjBits)));
                }
                sw->addCase(ci, groupBBs[i]);
            }
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
            if (anyArmMerged) {
                for (auto& n : postGroupNYA) mergedPostNYA.insert(n);
            } else {
                mergedPostNYA = postGroupNYA;
                anyArmMerged = true;
            }
        }
        module->popLoopContext();

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

    // Unwinds active try frames from the innermost down to `stopDepth` (the count
    // to KEEP), popping each runtime frame and running its finally. Emitted at
    // every return (stopDepth 0) and at break/continue (the loop's depth).
    static void emitTryFinallyUnwind(CajetaModulePtr module, size_t stopDepth = 0) {
        auto& stack = module->getTryFinallyStack();
        if (stack.size() <= stopDepth) return;
        auto* builder = module->getBuilder();
        llvm::Function* pop = module->getRuntimeFunction("__cajeta_exc_pop");
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

    // Emitted at every explicit return: leaves the debug, line and profile frames,
    // then pops this method's scope frames back to its entry watermark, joining
    // child tasks. Line/profile leaves are frame-gated (a lambda pushed none).
    static void emitScopeExitToWatermark(CajetaModulePtr module) {
        {
            auto m = module->getCurrentMethod();
            dbg::emitDbgFrameLeave(module, m ? m->getDbgFrameSlot() : nullptr);
        }
        auto m = module->getCurrentMethod();
        if (m && m->hasLineFrame()) dbg::emitLineLeave(module);
        if (m) prof::emitProfileExit(module, m->getProfileFrame());
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

    // Stores the paired return flag of a class-pointer-returning method: called
    // immediately before `ret` and AFTER owner drops, so no drop_fn can clobber
    // the TLS before the caller reads it. `flagOverride` supplies a runtime bit.
    static void emitReturnFlag(CajetaModulePtr& module,
                               llvm::Value* flagOverride = nullptr) {
        auto m = module->getCurrentMethod();
        bool lambdaMode = !m && module->isLambdaClassPtrReturn();
        if (!lambdaMode && (!m || !m->returnsClassPointer())) {
            return;
        }
        llvm::Function* fn = module->getRuntimeFunction("__cajeta_return_flag_set");
        if (!fn) {
            return;
        }
        llvm::Value* flag = flagOverride ? flagOverride
            : (llvm::Value*) module->getBuilder()->getInt64(
                  (!lambdaMode && m->isReturnsOwnership()) ? 1 : 0);
        module->getBuilder()->CreateCall(fn, {flag});
    }

    // Records one plain class-pointer return for the return-title audit, reading
    // the flag the compiler is about to store so the enumeration measures rather
    // than re-classifies. No-op unless CAJETA_AUDIT_RETURN_TITLES is set.
    static void auditReturnTitle(CajetaModulePtr& module, llvm::Value* flag,
                                 const ExpressionPtr& expression,
                                 int sourceLine,
                                 cajeta::ownership::TitleVia via) {
        namespace own = cajeta::ownership;
        auto m = module->getCurrentMethod();
        if (!m || !m->returnsClassPointer() || m->isReturnsOwnership()) return;
        const std::string cls =
            m->getParent() ? m->getParent()->toCanonical() : "";
        const std::string ret =
            m->getReturnType() ? m->getReturnType()->toCanonical() : "";
        own::ReturnTitleAudit::consider(cls, m->getName(), ret);
        if (!flag) return;
        if (auto* konst = llvm::dyn_cast<llvm::ConstantInt>(flag)) {
            if (konst->isZero()) return;
        }

        own::ReturnTitleRecord rec;
        rec.className = cls;
        rec.methodName = m->getName();
        rec.returnType = ret;
        // A monomorphization's line counts lines in a synthesized instantiation
        // buffer, not in the file, so it is dropped rather than printed wrong.
        const bool synthesizedBuffer =
            m->isMethodTemplateInstantiation()
            || (m->getParent() && m->getParent()->isInstantiation());
        rec.line = synthesizedBuffer ? 0 : sourceLine;
        rec.carry = llvm::isa<llvm::ConstantInt>(flag)
            ? own::TitleCarry::StaticTitle : own::TitleCarry::RuntimeFlag;

        rec.via = via;
        if (via == own::TitleVia::CallRide) {
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
                if (ExpressionPtr target = ce->getCallee()) {
                    if (auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                            target->getResolvedType())) {
                        rec.calleeOwned = fnType->isReturnsOwnership();
                    }
                }
            }
        }
        own::ReturnTitleAudit::record(std::move(rec));
    }

    // Return-shape contracts: one policy row per returned expression.
    namespace {
        std::string stackWhat(const cajeta::ownership::TitleShape& sh) {
            if (sh.family == cajeta::ownership::TitleFamily::LocalRead && sh.field) {
                return "local '" + sh.field->getName() + "', a `stack` value,";
            }
            return "a `stack` construction";
        }

        // Throws the diagnostic for a `#` return whose shape holds no title, with
        // the message chosen by the verdict's error code.
        [[noreturn]] void throwOwnedReturn(const MethodPtr& m,
                                           const cajeta::ownership::TitleShape& sh,
                                           const cajeta::ownership::TitleVerdict& v) {
            namespace own = cajeta::ownership;
            const std::string code = v.error ? v.error : "CAJETA_ERROR_OWNED_RETURN_OF_BORROW";
            const std::string canonical = m->toCanonical(false);
            if (code == "CAJETA_ERROR_STACK_RETURN_ESCAPES") {
                throw Exception(
                    "method `" + canonical + "` returns " + stackWhat(sh)
                    + " through a `#` (ownership-transfer) return type. The "
                    "stack instance is reclaimed when this frame exits, but "
                    "`#` promises the caller an owned value that outlives "
                    "the call — the caller would register a drop over freed "
                    "stack memory. Fix: allocate with `heap` (the value "
                    "must outlive the frame), or drop the `#` from the "
                    "return type if you meant to return by value. See "
                    "docs/specification/lang/MemoryModel.md § Function "
                    "signatures.", code);
            }
            if (code == "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS") {
                throw Exception(
                    "method `" + canonical + "` returns "
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
                    "specs/owned-return-of-borrowed-this-spec.md.", code);
            }
            const std::string name = sh.field ? sh.field->getName() : std::string();
            if (code == "CAJETA_ERROR_BORROW_PARAM_ESCAPES") {
                throw Exception(
                    "method `" + canonical + "` declares a `#T` return "
                    "(ownership transfer) but returns borrowed parameter `"
                    + name + "` declared without `#`. The caller "
                    "would register a fresh drop entry on receipt and free a "
                    "value the original owner still references. Fix: mark "
                    "the parameter `#" + name + "` so the caller transfers "
                    "ownership, or change the return type to plain `T` "
                    "(borrow pass-through). See "
                    "docs/specification/lang/OwnershipTransfer.md.", code);
            }
            std::string held;
            std::string fix;
            if (sh.family == own::TitleFamily::LocalRead && sh.field) {
                std::string origin = sh.field->getCallBorrowOrigin();
                if (!origin.empty()) {
                    held = "`" + name + "` holds a BORROW — it came from the "
                        "borrow-returning call `" + origin + "`, whose receiver "
                        "still owns and frees the value";
                } else if (!(origin = sh.field->getParamBorrowOrigin()).empty()) {
                    held = "`" + name + "` holds a BORROW — it came from the "
                        "plain parameter `" + origin + "`, whose title stays "
                        "with the caller";
                } else {
                    held = "`" + name + "` holds a BORROW — it was bound "
                        "without a title (a literal, an alias of another "
                        "binding, or an arena value), so this frame never "
                        "owned what it names";
                }
                fix = "return `#= " + name + "` to carry whatever mode the "
                    "frame actually holds (the caller then registers a drop "
                    "only when it really received a title), or produce an "
                    "owned value, or drop the `#` from the return type";
            } else if (sh.family == own::TitleFamily::CallResult && sh.callee) {
                const std::string callee = sh.callee->toCanonical(false);
                if (sh.callee->isReturnsView()) {
                    held = "it returns the result of `" + callee + "`, which "
                        "is declared `^` — a VIEW interior to its receiver, "
                        "which still owns and frees it";
                } else {
                    held = "it returns the result of `" + callee + "`, whose "
                        "every return is an interior read of its receiver — a "
                        "BORROW the receiver still owns and frees";
                }
                fix = "declare this method `^` too and let the view travel "
                    "(the caller then never frees it), or produce an owned "
                    "value, or return `#= ` a local whose runtime mode you "
                    "actually hold";
            } else {
                held = "it returns " + std::string(sh.label)
                    + " — a BORROW of a value someone else owns and frees";
                fix = "produce an owned value (`heap ...`, a String concat, "
                    "`#local` to surrender an owned local, or a call), or "
                    "drop the `#` from the return type";
            }
            throw Exception(
                "method `" + canonical + "` promises ownership with a `#` "
                "return type, but " + held + ". The `#` return asserts a title "
                "this frame never held, so the caller arms a drop on a value "
                "someone else still owns and frees: a double free on the "
                "caller's scope exit. Fix: " + fix + ". See "
                "specs/stdlib-ownership-convention-spec.md §4.5 and §4.7.",
                "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
        }

        // The `#T` return contract, pre-codegen: every returned shape — and every
        // arm of a returned conditional — must hold a title. `modeCarrying`
        // (`return #= x`) ships the frame's real mode, so only frame-bound fails.
        void checkOwnedReturnShapes(CajetaModulePtr& module, const MethodPtr& m,
                                    const ExpressionPtr& expression, bool modeCarrying) {
            namespace own = cajeta::ownership;
            own::rejectEscape(expression, own::ConsumerRole::ReturnOwned, module, "a `#` return");
            own::TitleShape sh = own::classify(expression, module);
            if (sh.family == own::TitleFamily::Move) {
                auto& mch = expression->getChildren();
                if (!mch.empty()) {
                    if (auto in = dynamic_pointer_cast<Expression>(mch[0])) {
                        own::TitleShape ish = own::classify(in, module);
                        if (ish.family == own::TitleFamily::ThisRead) {
                            own::TitleVerdict tv{ish.answer, ish.source,
                                "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS", ish.label};
                            throwOwnedReturn(m, ish, tv);
                        }
                    }
                }
            }
            if (sh.family == own::TitleFamily::Conditional) {
                if (modeCarrying) return;
                bool found = false;
                own::TitleShape bad;
                BooleanSwitchExpression::forEachLeafArm(sh.leaf,
                    [&](const ExpressionPtr& arm) {
                        if (found || !arm) return;
                        own::TitleShape ash = own::classify(arm, module);
                        own::TitleVerdict av = own::policy(ash, own::ConsumerRole::Arm);
                        if (av.answer == own::TitleAnswer::Borrow
                                || av.answer == own::TitleAnswer::StackBound) {
                            bad = ash;
                            found = true;
                        }
                    });
                if (found) {
                    throw Exception(
                        "method `" + m->toCanonical(false)
                        + "` promises ownership with a `#` return type, "
                        "but returns a conditional (`c ? a : b`) one of "
                        "whose arms is " + std::string(bad.label)
                        + " — a BORROW. When that arm is taken the "
                        "caller arms a drop on a value someone else "
                        "still owns and frees: a double free. Fix: make "
                        "every arm produce a title (`heap ...`, a String "
                        "concat, `#local` to surrender an owned local, "
                        "or a call), return `#= (c ? a : b)` to carry "
                        "whatever mode the taken arm actually holds, or "
                        "drop the `#` from the return type. See "
                        "specs/stdlib-ownership-convention-spec.md §4.5.",
                        "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
                }
                return;
            }
            own::TitleVerdict v = own::policy(sh, own::ConsumerRole::ReturnOwned);
            if (!v.error) return;
            if (modeCarrying) {
                const std::string code = v.error;
                if (code != "CAJETA_ERROR_STACK_RETURN_ESCAPES"
                        && code != "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS") {
                    return;
                }
            }
            throwOwnedReturn(m, sh, v);
        }

        // The plain-return rules, pre-codegen: no fresh value, owned local or
        // `stack` value may leave through a non-`#` return. Held against every
        // leaf arm of a returned conditional.
        void checkPlainReturnShapes(CajetaModulePtr& module, const MethodPtr& m,
                                    const ExpressionPtr& expression) {
            namespace own = cajeta::ownership;
            own::TitleShape top = own::classify(expression, module);
            const bool viaConditional = top.family == own::TitleFamily::Conditional;
            const std::string armNote = viaConditional
                ? " The value is an arm of the returned conditional "
                  "(`c ? a : b`): every arm is held to this rule."
                : "";
            BooleanSwitchExpression::forEachLeafArm(top.leaf,
                [&](const ExpressionPtr& arm) {
                    if (!arm) return;
                    own::TitleShape sh = viaConditional ? own::classify(arm, module) : top;
                    own::TitleVerdict v = own::policy(sh, own::ConsumerRole::ReturnPlain);
                    if (!v.error) return;
                    const std::string code = v.error;
                    const std::string canonical = m->toCanonical(false);
                    if (code == "CAJETA_ERROR_STACK_RETURN_ESCAPES") {
                        throw Exception(
                            "method `" + canonical + "` returns " + stackWhat(sh)
                            + " through a plain class return. The stack "
                            "instance is reclaimed when this frame exits, so "
                            "the caller would hold a pointer into freed stack "
                            "memory. Fix: allocate with `heap` and mark the "
                            "return type `#T` so the title transfers, or make "
                            "the type a value type to return it by copy." + armNote,
                            code);
                    }
                    if (sh.family == own::TitleFamily::Fresh) {
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
                            "MemoryModel.md § Function signatures." + armNote,
                            code);
                    }
                    const std::string name = sh.field ? sh.field->getName() : std::string();
                    throw Exception(
                        "method `" + canonical + "` returns local "
                        "'" + name + "', which "
                        "has an active drop entry, through a "
                        "plain (non-`#`) return type. If the "
                        "local OWNS its value, the return "
                        "deactivates this scope's drop while the "
                        "caller registers none — the allocation "
                        "leaks; fix: mark the return type `#T`. "
                        "If the local only holds a BORROW from a "
                        "call, the compiler cannot tell (the drop "
                        "entry is the only static evidence, and "
                        "ownership is runtime state) — return the "
                        "call directly (`return f(...)`, which "
                        "rides the callee's flag) or use `return "
                        "#= " + name + "` "
                        "(which ships the frame's actual mode). "
                        "See docs/specification/MemoryModel.md "
                        "§ Function signatures." + armNote,
                        code);
                });
        }
    }  // namespace

    // Lowers `return`: enforces the `#T`, `^T` and plain-return contracts, runs
    // the advice, try-finally, scope-exit and drop chains, then stores the return
    // title flag immediately before the `ret`.
    llvm::Value* ReturnStatement::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (expression && expression->kind() == ExprKind::Move) {
            cajeta::ownership::rejectEscape(expression,
                cajeta::ownership::ConsumerRole::ReturnOwned, module, "a `#` return");
        }
        llvm::Value* returnTitleFlag = nullptr;
        cajeta::ownership::TitleVia via = cajeta::ownership::TitleVia::Other;
        // `return #= x` releases WHATEVER title this frame holds: the take reads
        // the local's drop entry and disarms it in one step, so the flag is 1 only
        // when we owned the value and 0 when we held a borrow.
        if (modeCarrying && expression) {
            if (auto mcId = dynamic_pointer_cast<IdentifierExpression>(expression)) {
                if (auto scope = module->getScopeStack().peek()) {
                    if (FieldPtr mcFld = scope->getField(mcId->getTextValue())) {
                        if (llvm::Value* mcEntry = mcFld->getDropEntry()) {
                            llvm::Function* takeFn = module->getRuntimeFunction(
                                mcFld->isEntryMayBeStale() ? "__cajeta_drop_take_active_if"
                                                            : "__cajeta_drop_take_active");
                            if (takeFn) {
                                llvm::Value* was = nullptr;
                                if (mcFld->isEntryMayBeStale()) {
                                    llvm::Value* mcCur = builder->CreateLoad(
                                        llvm::PointerType::get(*module->getLlvmContext(), 0),
                                        mcFld->getOrCreateAllocation(), "title.current");
                                    was = builder->CreateCall(takeFn, {mcEntry, mcCur}, "title.release");
                                } else {
                                    was = builder->CreateCall(takeFn, {mcEntry}, "title.release");
                                }
                                returnTitleFlag = builder->CreateZExt(
                                    was, llvm::Type::getInt64Ty(
                                        *module->getLlvmContext()),
                                    "title.release.i64");
                                via = cajeta::ownership::TitleVia::ModeCarry;
                            }
                        }
                    }
                }
            }
            if (!returnTitleFlag) {
                returnTitleFlag = builder->getInt64(0);
            }
        }
        if (!expression) {
            if (auto m = module->getCurrentMethod()) {
                m->emitAfterAdvice(module);
                m->emitAfterReturningAdvice(module);
                m->emitAfterThrowingTryPop(module);
            }
            emitTryFinallyUnwind(module);
            emitScopeExitToWatermark(module);
            if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
            return builder->CreateRetVoid();
        }
        // A returned literal is target-typed by the declared return type before
        // codegen consumes it: an array type widens the elements, a class type
        // rewrites it to a from-array construction.
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
            if (auto m = module->getCurrentMethod()) {
                CajetaTypePtr rt = m->getReturnType();
                if (dynamic_pointer_cast<CajetaClass>(rt)
                        && !dynamic_pointer_cast<CajetaArray>(rt)) {
                    agg->setExpectedType(rt);
                }
            }
        } else if (auto mapLit = dynamic_pointer_cast<
                       MapLiteralExpression>(expression)) {
            if (auto m = module->getCurrentMethod()) {
                CajetaTypePtr rt = m->getReturnType();
                if (dynamic_pointer_cast<CajetaClass>(rt)
                        && !dynamic_pointer_cast<CajetaArray>(rt)) {
                    mapLit->setExpectedType(rt);
                }
            }
        }
        // Value return (sret + NRVO): the LLVM signature returns void and takes
        // the result slot as hidden arg 0, so build the value into that caller-
        // owned slot and `ret void`, bypassing the pointer-return paths below.
        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        bool sretMethod = false;
        if (auto m = module->getCurrentMethod()) {
            sretMethod = m->returnsStackValue();
        }
        bool sretFnSig = curFn && curFn->getReturnType()->isVoidTy()
            && curFn->arg_size() > 0
            && curFn->getArg(0)->hasAttribute(llvm::Attribute::StructRet);
        if ((sretMethod || sretFnSig) && expression) {
            {
                std::string what;
                if (auto m = module->getCurrentMethod()) {
                    what = "`" + m->toCanonical(false) + "`";
                    if (m->getReturnType())
                        what += " returns `" + m->getReturnType()->toCanonical() + "` by value";
                } else {
                    what = "this lambda returns a value-shape class by value";
                }
                if (auto tl = dynamic_pointer_cast<TextLiteralExpression>(expression)) {
                    if (tl->getLiteralType() == LITERAL_TYPE_NULL) {
                        throw Exception(
                            what + ", so `return null` has no value to copy into the "
                            "caller's slot. Return an empty instance instead — for an "
                            "Optional, `return stack Optional<T>(false);`.",
                            "CAJETA_ERROR_NULL_RETURN_BY_VALUE");
                    }
                }
                if (auto ne = dynamic_pointer_cast<NewExpression>(expression)) {
                    if (!ne->getStackAlloc()) {
                        throw Exception(
                            what + ", so a `heap` construction here would be copied "
                            "out and leaked. Write `return stack ...(...)` — or, to "
                            "hand the caller an owned heap object, declare the return "
                            "type `#T`.",
                            "CAJETA_ERROR_HEAP_RETURN_BY_VALUE");
                    }
                }
            }
            llvm::Value* sretPtr = curFn->getArg(0);
            if (auto newExpr = dynamic_pointer_cast<NewExpression>(expression)) {
                newExpr->setNrvoTarget(sretPtr);
                expression->generateCode(module);
            } else {
                // Fallback copy. A class-typed identifier's allocation is a SLOT
                // (an alloca of `ptr`) holding the struct address: load through it
                // once, or the memcpy takes the slot's own pointer bytes.
                llvm::Value* v = expression->generateCode(module);
                llvm::Type* structTy = nullptr;
                if (auto m = module->getCurrentMethod()) {
                    if (m->getReturnType()) structTy = m->getReturnType()->getLlvmType();
                }
                if (!structTy && sretFnSig) {
                    structTy = curFn->getArg(0)->getAttribute(
                        llvm::Attribute::StructRet).getValueAsType();
                }
                if (v && structTy) {
                    if (v->getType()->isPointerTy()) {
                        llvm::Value* srcPtr = v;
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
            emitTryFinallyUnwind(module);
            emitScopeExitToWatermark(module);
            if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
            return builder->CreateRetVoid();
        }
        if (auto m = module->getCurrentMethod()) {
            if (m->isReturnsOwnership() && expression) {
                checkOwnedReturnShapes(module, m, expression, modeCarrying);
            }
        }
        // The `^T` body restriction, part 1: the arms decidable from the return
        // expression's shape alone. The CALL arm waits for part 2, after codegen —
        // a MethodCallExpression's resolvedMethod is null until it generates.
        if (auto m = module->getCurrentMethod()) {
            if (m->isReturnsView() && expression) {
                ExpressionPtr inner = expression;
                if (auto mvIn = moveInner(expression)) inner = mvIn;
                // Identity reference casts are peeled: a cast must not launder
                // the stance.
                while (auto castE = dynamic_pointer_cast<CastExpression>(inner)) {
                    CajetaTypePtr dt = castE->getDestType();
                    auto dc = dynamic_pointer_cast<CajetaClass>(dt);
                    auto dv = dynamic_pointer_cast<CajetaView>(dt);
                    bool refCast = (bool) dv
                        || (dc && !dc->isInterface() && !dc->isValueType());
                    if (!refCast || castE->getChildren().empty()) break;
                    auto peeled =
                        dynamic_pointer_cast<Expression>(castE->getChildren()[0]);
                    if (!peeled) break;
                    inner = peeled;
                }
                bool permitted =
                    dynamic_pointer_cast<ThisExpression>(inner) != nullptr
                    || Method::exprIsInteriorRead(inner)
                    // Part 2's arm — never judge a call before it resolves.
                    || dynamic_pointer_cast<MethodCallExpression>(inner) != nullptr;
                if (!permitted) {
                    if (auto tl =
                            dynamic_pointer_cast<TextLiteralExpression>(inner)) {
                        if (tl->getLiteralType() == LITERAL_TYPE_NULL) {
                            permitted = true;
                        }
                    }
                }
                std::string what;
                if (!permitted) {
                    if (auto idExpr =
                            dynamic_pointer_cast<IdentifierExpression>(inner)) {
                        const std::string& n = idExpr->getTextValue();
                        auto scope = module->getScopeStack().peek();
                        FieldPtr f = scope ? scope->getField(n) : nullptr;
                        if (n == "this") {
                            permitted = true;
                        } else if (!f) {
                            // Not a frame binding, so the name resolves through
                            // implicit-this: `return c;` IS `return this.c;`.
                            permitted = true;
                        } else if (m->getParameters().count(n) > 0) {
                            what = "parameter `" + n + "`, whose lifetime "
                                   "belongs to the CALLER, not to this "
                                   "receiver";
                        } else {
                            what = "local `" + n + "` — a named frame binding "
                                   "is outside §4.7's closed list whatever it "
                                   "holds (a title dies with this frame; a "
                                   "borrow's owner is not named by the "
                                   "signature). Inline the read the local "
                                   "stands for";
                        }
                    }
                }
                if (!permitted && what.empty()
                        && Method::exprIsStackConstruction(inner)) {
                    what = "a `stack` construction, which is reclaimed "
                           "when this frame exits";
                }
                if (!permitted && what.empty()
                        && dynamic_pointer_cast<DotExpression>(inner)) {
                    what = "a field of an object other than `this` — the "
                           "receiver's signature cannot vouch for another "
                           "object's interior (and a frame-owned root is freed "
                           "at scope exit, dangling the view)";
                }
                if (!permitted) {
                    if (what.empty()) {
                        what = "a value that is not interior to the receiver";
                    }
                    throw Exception(
                        "method `" + m->toCanonical(false) + "` is declared "
                        "`^` (a VIEW interior to the receiver, which the "
                        "caller must not free), but it returns " + what
                        + ". A `^` return may only hand back `this`, an "
                          "interior read (`this.field`, `this.field[i]`, or "
                          "the bare-name spelling), `null`, or another `^` "
                          "result on a `this`-rooted receiver. Fix: declare "
                          "the return `#` if the caller should receive a "
                          "title and free it, or return interior state if you "
                          "meant a view. See "
                          "specs/stdlib-ownership-convention-spec.md §4.7.",
                        "CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR");
                }
            }
        }
        if (auto m = module->getCurrentMethod()) {
            bool isLambda = m->getName().rfind("__cajeta_lambda_", 0) == 0;
            auto rtype = m->getReturnType();
            bool returnsValueType = rtype && rtype->isValueType();
            bool returnsByValuePrimitive =
                rtype && (rtype->getTypeFlags() & PRIMITIVE_FLAG) != 0;
            if (!isLambda && !m->isReturnsOwnership() && !returnsValueType
                    && !returnsByValuePrimitive && !modeCarrying) {
                checkPlainReturnShapes(module, m, expression);
            }
        }

        // Single-hop DANGLING LEND: a local that the returned holder merely LENT
        // dies at this scope's exit. Conservative and intra-procedural.
        {
            ExpressionPtr escapee = expression;
            if (auto mvIn = moveInner(escapee)) escapee = mvIn;
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

        // The returned NAME, pre-codegen: closure and view escapes are refused,
        // and the title flag is read HERE — before the drop entry is handed over —
        // so this scope cannot drop the caller's value. Identity casts peeled.
        {
            namespace own = cajeta::ownership;
            own::TitleShape nameShape = own::classify(expression, module);
            Field* f = nameShape.family == own::TitleFamily::LocalRead ? nameShape.field : nullptr;
            if (f && f->hasBorrowCaptures()) {
                throw Exception(
                    "cannot return closure '" + f->getName()
                    + "' — it captures one or more outer locals by borrow, "
                    "and those borrows would dangle past the function return; "
                    "transfer the captures via `#name` to give the closure "
                    "ownership it can carry past this scope",
                    "CAJETA_ERROR_BORROW_ESCAPE");
            }
            if (f && f->getViewSource()) {
                FieldPtr src = f->getViewSource();
                bool srcIsParam =
                    dynamic_pointer_cast<ParameterField>(src) != nullptr;
                if (!srcIsParam) {
                    throw Exception(
                        "cannot return struct view '" + f->getName()
                        + "' — it aliases the buffer '" + src->getName()
                        + "' which is a function-scope local and would drop "
                        "as this function returns, leaving the caller with "
                        "a view of freed memory; allocate the buffer on the "
                        "caller side and pass it in as a parameter, or have "
                        "the function return the buffer instead of the view",
                        "CAJETA_ERROR_VIEW_ESCAPE");
                }
            }
            if (f) {
                if (dynamic_pointer_cast<CajetaFunctionType>(f->getType())) {
                    if (f->getDropEntry()) {
                        ownership::deactivateLocalEntry(module, f);
                    }
                }
                auto klass = dynamic_pointer_cast<CajetaClass>(f->getType());
                auto view = dynamic_pointer_cast<CajetaView>(f->getType());
                bool transferShape =
                    (klass && !view && !klass->isInterface()) || (bool) view;
                if (transferShape) {
                    if (llvm::Value* entry = f->getDropEntry()) {
                        auto m = module->getCurrentMethod();
                        if (m && m->returnsClassPointer() && !modeCarrying) {
                            own::TitleVerdict nv = own::policy(nameShape,
                                m->isReturnsOwnership() ? own::ConsumerRole::ReturnOwned
                                                        : own::ConsumerRole::ReturnPlain);
                            if (llvm::Value* tf = own::verdictFlag(nameShape, nv, module)) {
                                returnTitleFlag = tf;
                                if (nv.source == own::TitleSource::DropEntry
                                        && nameShape.has(own::TitleShape::kIsParam)) {
                                    via = own::TitleVia::FormalPassThrough;
                                }
                            }
                        }
                        ownership::deactivateLocalEntry(module, f);
                    }
                }
            }
        }
        llvm::Value* val = expression->generateCode(module);
        cajeta::ownership::TitleShape rs = cajeta::ownership::classify(expression, module);
        // The `^T` body restriction, part 2: the CALL arm, deferred until codegen
        // resolved the callee. Delegation through another `^` is allowed only on a
        // `this`-rooted receiver chain, else the view rides a dying frame local.
        if (auto m = module->getCurrentMethod()) {
            if (m->isReturnsView() && expression) {
                ExpressionPtr innerV = expression;
                if (auto mvIn = moveInner(expression)) innerV = mvIn;
                while (auto castE = dynamic_pointer_cast<CastExpression>(innerV)) {
                    CajetaTypePtr dt = castE->getDestType();
                    auto dc = dynamic_pointer_cast<CajetaClass>(dt);
                    auto dv = dynamic_pointer_cast<CajetaView>(dt);
                    bool refCast = (bool) dv
                        || (dc && !dc->isInterface() && !dc->isValueType());
                    if (!refCast || castE->getChildren().empty()) break;
                    auto peeled =
                        dynamic_pointer_cast<Expression>(castE->getChildren()[0]);
                    if (!peeled) break;
                    innerV = peeled;
                }
                if (auto viewCall =
                        dynamic_pointer_cast<MethodCallExpression>(innerV)) {
                    MethodPtr vm = viewCall->getResolvedMethod();
                    // Is every receiver hop rooted at `this`? Accepts implicit
                    // this, `this`, an interior read, a name that is not a frame
                    // binding, or another `^` call whose receiver passes.
                    std::function<bool(const ExpressionPtr&)> thisRooted =
                        [&](const ExpressionPtr& r) -> bool {
                        if (!r) return true;
                        ExpressionPtr e = r;
                        while (auto cE = dynamic_pointer_cast<CastExpression>(e)) {
                            CajetaTypePtr dt = cE->getDestType();
                            auto dc = dynamic_pointer_cast<CajetaClass>(dt);
                            auto dv = dynamic_pointer_cast<CajetaView>(dt);
                            bool refCast = (bool) dv
                                || (dc && !dc->isInterface()
                                    && !dc->isValueType());
                            if (!refCast || cE->getChildren().empty()) break;
                            auto pe = dynamic_pointer_cast<Expression>(
                                cE->getChildren()[0]);
                            if (!pe) break;
                            e = pe;
                        }
                        if (dynamic_pointer_cast<ThisExpression>(e)) return true;
                        if (Method::exprIsInteriorRead(e)) return true;
                        if (auto id =
                                dynamic_pointer_cast<IdentifierExpression>(e)) {
                            if (id->getTextValue() == "this") return true;
                            auto scope = module->getScopeStack().peek();
                            return !(scope
                                     && scope->getField(id->getTextValue()));
                        }
                        if (auto mc =
                                dynamic_pointer_cast<MethodCallExpression>(e)) {
                            MethodPtr rm = mc->getResolvedMethod();
                            if (rm && rm->isReturnsView()) {
                                auto& ch = mc->getChildren();
                                return thisRooted(ch.empty()
                                    ? nullptr
                                    : dynamic_pointer_cast<Expression>(ch[0]));
                            }
                        }
                        return false;
                    };
                    std::string what;
                    if (vm && vm->isReturnsView()) {
                        auto& ch = viewCall->getChildren();
                        if (!thisRooted(ch.empty()
                                ? nullptr
                                : dynamic_pointer_cast<Expression>(ch[0]))) {
                            what = "the result of `" + vm->toCanonical(false)
                                 + "` on a receiver that is not rooted at "
                                   "`this` — the view rides THAT receiver's "
                                   "lifetime (a frame local dies at scope "
                                   "exit; a parameter's owner is the caller), "
                                   "which this method's signature cannot "
                                   "vouch for";
                        }
                    } else if (vm && vm->isReturnsOwnership()) {
                        what = "the result of `" + vm->toCanonical(false)
                             + "`, which is declared `#` — the title arrives "
                               "here and `^` then disclaims it, so the value "
                               "dies with nobody holding it";
                    } else if (vm) {
                        what = "the result of `" + vm->toCanonical(false)
                             + "`, whose plain return carries a RUNTIME mode "
                               "that `^`'s statically-borrow flag cannot "
                               "represent";
                    } else {
                        what = "an unresolved call result this check cannot "
                               "verify against the `^` promise";
                    }
                    if (!what.empty()) {
                        throw Exception(
                            "method `" + m->toCanonical(false) + "` is "
                            "declared `^` (a VIEW interior to the receiver, "
                            "which the caller must not free), but it returns "
                            + what
                            + ". A `^` return may only hand back `this`, an "
                              "interior read, `null`, or another `^` result "
                              "on a `this`-rooted receiver. Fix: declare the "
                              "return `#` if the caller should receive a "
                              "title and free it, or return interior state if "
                              "you meant a view. See "
                              "specs/stdlib-ownership-convention-spec.md "
                              "§4.7.",
                            "CAJETA_ERROR_VIEW_RETURN_NOT_INTERIOR");
                    }
                }
            }
        }
        // The returned VALUE's title, post-codegen: a flag-storing callee's bit is
        // captured from the TLS HERE, before advice or drop calls overwrite it;
        // one that stores none uses its declared stance.
        if (auto m = module->getCurrentMethod()) {
            namespace own = cajeta::ownership;
            const own::ConsumerRole role = m->isReturnsOwnership()
                ? own::ConsumerRole::ReturnOwned : own::ConsumerRole::ReturnPlain;
            switch (rs.family) {
                case own::TitleFamily::CallResult:
                case own::TitleFamily::ClosureCall: {
                    if (auto mceRet = dynamic_pointer_cast<MethodCallExpression>(rs.leaf)) {
                        // A null resolution is an intrinsic lowering that
                        // stored no flag, so the static mode stands.
                        if (!mceRet->getResolvedMethod()
                                && rs.source == own::TitleSource::ReturnFlag) break;
                        if (mceRet->getFlaggedTitleValue()) break;
                    }
                    own::TitleVerdict v = own::policy(rs, role);
                    if (v.error) throwOwnedReturn(m, rs, v);
                    if (m->returnsClassPointer()) {
                        if (llvm::Value* tf = own::verdictFlag(rs, v, module)) {
                            returnTitleFlag = tf;
                            via = own::TitleVia::CallRide;
                        }
                    }
                    break;
                }
                case own::TitleFamily::Fresh:
                case own::TitleFamily::Concat:
                case own::TitleFamily::Literal:
                case own::TitleFamily::FieldRead:
                case own::TitleFamily::ElementRead:
                case own::TitleFamily::Closure: {
                    if (m->returnsClassPointer() && !modeCarrying) {
                        own::TitleVerdict v = own::policy(rs, role);
                        if (!v.error) {
                            if (llvm::Value* tf = own::verdictFlag(rs, v, module)) {
                                returnTitleFlag = tf;
                            }
                        }
                    }
                    break;
                }
                case own::TitleFamily::LocalRead:
                case own::TitleFamily::Move:
                case own::TitleFamily::Conditional:
                case own::TitleFamily::ThisRead:
                case own::TitleFamily::Scalar:
                case own::TitleFamily::Unsupported:
                case own::TitleFamily::Count:
                    break;
            }
        }
        // Returning a shared-capable VALUE out of a field or element COPIES it
        // while the owner keeps its stake, so retain the source's stakes.
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
        // Deferred form: a directly returned lambda's capture flag is populated
        // by its own generateCode, hence the post-codegen position.
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
        // An interface return travels BY VALUE as a 24-byte fat pointer, and a
        // named local's slot holds a body POINTER — hence the double load, which
        // must fire before the general alloca load below.
        if (auto m = module->getCurrentMethod()) {
            CajetaTypePtr byValRet;
            if (auto ifaceRet = dynamic_pointer_cast<CajetaClass>(m->getReturnType())) {
                if (ifaceRet->isInterface()) byValRet = ifaceRet;
            }
            if (byValRet) {
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
                            if (auto curM = module->getCurrentMethod()) {
                                curM->emitAfterAdvice(module);
                                curM->emitAfterReturningAdvice(module);
                                curM->emitAfterThrowingTryPop(module);
                            }
                            emitTryFinallyUnwind(module);
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

        // Load through an l-value result — `ret` wants a value, not an address.
        if (val && llvm::dyn_cast<llvm::AllocaInst>(val)) {
            auto* a = llvm::cast<llvm::AllocaInst>(val);
            val = builder->CreateLoad(a->getAllocatedType(), a);
        } else if (auto idx = dynamic_pointer_cast<ArrayIndexExpression>(expression)) {
            CajetaTypePtr elemType = idx->getResolvedType();
            if (elemType) {
                auto elemClass = dynamic_pointer_cast<CajetaClass>(elemType);
                bool elemIsInterface = elemClass && elemClass->isInterface();
                bool elemIsValueType = elemClass && elemClass->isValueType();
                llvm::Type* loadTy;
                if (elemIsInterface || elemIsValueType) {
                    // Interface and value-type elements are stored INLINE: load
                    // the body struct itself. A `ptr` load would take only the
                    // data word and the caller would dereference it as a body.
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
            // A field-slot GEP: load through it with the field's declared type,
            // then bswap if the receiver carries a non-host endianness.
            if (!dot->getChildren().empty()) {
                auto recv = dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
                if (recv) {
                    if (!recv->getResolvedType()) recv->resolveTypes(module);
                    if (auto klass = dynamic_pointer_cast<CajetaClass>(recv->getResolvedType())) {
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
                            // Class- and array-typed fields are stored as `ptr`
                            // in the layout; views, interfaces and value types
                            // stay INLINE and load their body by copy.
                            auto foundCls = dynamic_pointer_cast<CajetaClass>(found->getType());
                            bool foundIsView = dynamic_pointer_cast<CajetaView>(found->getType()) != nullptr;
                            bool foundIsArray = dynamic_pointer_cast<CajetaArray>(found->getType()) != nullptr;
                            bool foundIsInterface = foundCls && foundCls->isInterface();
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
            // Implicit-this field access: a bare identifier resolving to a class
            // property yields the field's GEP slot, not an alloca.
            if (val && val->getType()->isPointerTy() && id->getResolvedType()) {
                if (llvm::Type* lt = id->getResolvedType()->getLlvmType()) {
                    if (lt != val->getType()) {
                        val = builder->CreateLoad(lt, val);
                    }
                }
            }
        }
        // MI upcast at the return site: shift to the ancestor sub-object when it
        // sits at a non-zero offset, so the caller's binding to the declared
        // return type lands on the right vtable and field offsets.
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

        // Coerce to the function's return type: integer literals pick the
        // smallest fitting width and there is no upfront-promotion pass.
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        llvm::Type* retTy = fn->getReturnType();
        if (!val) {
            // A value-less `return;` took the void path above, so a null here is
            // an unresolved expression — `ret null` would silently miscompile it.
            throw locatedException(
                expression->getSourceLine(), expression->getSourceColumn() + 1,
                "returned expression did not resolve to a value (a sub-expression"
                " produced nothing — e.g. a method or member that does not exist"
                " on the receiver's type)",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        if (!retTy->isVoidTy() && expression->getResolvedType()
                && expression->getResolvedType()->toCanonical() == "void") {
            throw locatedException(
                expression->getSourceLine(), expression->getSourceColumn() + 1,
                "returned expression is 'void', but the method returns a value",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        // Wrap a concrete class into the interface's 24-byte
        // `{ ptr data, ptr vtable, i64 kind }` body. MUST run before the aggregate
        // coercion below, which would instead load 24 bytes off the object.
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
                    // A `#` return, a fresh construction or an explicit `#x`
                    // hands the title out; anything else leaves the callee owning.
                    cajeta::ownership::TitleShape ifSh = cajeta::ownership::classify(expression, module);
                    bool ownedOut = m->isReturnsOwnership()
                        || (ifSh.family == cajeta::ownership::TitleFamily::Fresh
                            && ifSh.answer == cajeta::ownership::TitleAnswer::Owned)
                        || ifSh.family == cajeta::ownership::TitleFamily::Move;
                    builder->CreateStore(
                        llvm::ConstantInt::get(i64Ty,
                            (uint64_t) (ownedOut ? IFACE_KIND_OWNED_CLASS
                                                 : IFACE_KIND_BORROWED_CLASS)),
                        builder->CreateStructGEP(retTy, body, 2, "iface_kind"));
                    // By-value ABI: hand back the body STRUCT, not the alloca.
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
                // By-value aggregate return: the signature returns the struct, but
                // `return stack V(...)` yielded a pointer to it.
                val = builder->CreateLoad(retTy, val);
            } else if ((retTy->isFloatingPointTy() || retTy->isIntegerTy())
                    && valTy->isPointerTy()) {
                // A scalar return type with a pointer operand can only mean "load
                // the scalar from that address" (`return this.x;`, or a field phi).
                val = builder->CreateLoad(retTy, val);
            }
            // Remaining pointer/aggregate mismatches fall through; verifier flags.
        }

        if (auto m = module->getCurrentMethod()) {
            m->emitAfterAdvice(module);
            m->emitAfterReturningAdvice(module);
            m->emitAfterThrowingTryPop(module);
        }
        emitTryFinallyUnwind(module);
        emitScopeExitToWatermark(module);
        if (auto m = module->getCurrentMethod()) m->emitOwnerDrops(module);
        auto emitTitleContract = [&](llvm::Value* flag) {
            // A `#R` return is a CONTRACT: a runtime flag of 0 would hand the
            // caller a forged title, so panic TITLE_MISS instead. `return #= x`
            // is the sanctioned escape — it declares the return carries the mode.
            if (auto mM = module->getCurrentMethod()) {
                if (mM->isReturnsOwnership() && !modeCarrying
                        && flag && !llvm::isa<llvm::ConstantInt>(flag)) {
                    auto& rctx = *module->getLlvmContext();
                    llvm::Value* hasTitle = builder->CreateICmpNE(
                        flag,
                        llvm::ConstantInt::get(flag->getType(), 0),
                        "ret_contract_ok");
                    llvm::Function* rfn =
                        builder->GetInsertBlock()->getParent();
                    auto* panicBB = llvm::BasicBlock::Create(
                        rctx, "ret_title_panic", rfn);
                    auto* okBB = llvm::BasicBlock::Create(
                        rctx, "ret_title_ok", rfn);
                    builder->CreateCondBr(hasTitle, okBB, panicBB);
                    builder->SetInsertPoint(panicBB);
                    // CAJETA_PANIC_TITLE_MISS = 3, integer-throw shape (< 4096, so
                    // the first catch clause binds it).
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
        };
        if (rs.family == cajeta::ownership::TitleFamily::Move) {
            returnTitleFlag = cajeta::ownership::titleFlag(rs, module);
            via = cajeta::ownership::TitleVia::Move;
            emitTitleContract(returnTitleFlag);
        }
        if (auto mcRet = dynamic_pointer_cast<MethodCallExpression>(rs.leaf)) {
            if (llvm::Value* f = mcRet->getFlaggedTitleValue()) {
                returnTitleFlag = f;
                via = cajeta::ownership::TitleVia::Flagged;
            }
        }
        // A returned conditional forwards the TAKEN arm's flag: `#` already
        // rejected every provably-borrow arm, so what reaches here is decided at
        // runtime and meets the same TITLE_MISS contract.
        if (rs.family == cajeta::ownership::TitleFamily::Conditional) {
            if (llvm::Value* tf = conditionalTitleFlag(rs.leaf)) {
                returnTitleFlag = tf;
                emitTitleContract(tf);
            }
        }
        // Lambda-body returns by shape: a fresh value or concatenation hands out
        // a title; a CALL result lets the inner flag ride through.
        if (!module->getCurrentMethod() && module->isLambdaClassPtrReturn()
                && !returnTitleFlag) {
            if ((rs.family == cajeta::ownership::TitleFamily::Fresh
                        && rs.answer == cajeta::ownership::TitleAnswer::Owned)
                    || rs.family == cajeta::ownership::TitleFamily::Concat) {
                returnTitleFlag = builder->getInt64(1);
            } else if (rs.family == cajeta::ownership::TitleFamily::CallResult
                    || rs.family == cajeta::ownership::TitleFamily::ClosureCall) {
                return builder->CreateRet(val);
            }
        }
        if (cajeta::ownership::ReturnTitleAudit::enabled()) {
            auditReturnTitle(module, returnTitleFlag, expression,
                             getSourceLine(), via);
        }
        emitReturnFlag(module, returnTitleFlag);
        return builder->CreateRet(val);
    }

    void ThrowStatement::resolveTypes(CajetaModulePtr module) {
        if (expression) expression->resolveTypes(module);
    }

    // Lowers `throw`: hands the value to the runtime as a void* (integers are
    // IntToPtr'd, preserving the legacy `throw 42` shape the catch codegen
    // reverses), then marks the block unreachable — the runtime longjmps.
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
        builder->CreateUnreachable();
        llvm::BasicBlock* dead = llvm::BasicBlock::Create(ctx, "after_throw",
            builder->GetInsertBlock()->getParent());
        builder->SetInsertPoint(dead);
        return nullptr;
    }

    // Lowers `break` / `break label;`: unwinds in-loop try finallys and block
    // drops down to the target loop's depth, then branches. A fresh block follows
    // so statements after it still emit into a valid container.
    llvm::Value* BreakStatement::generateCode(CajetaModulePtr module) {
        if (!module->hasLoopContext()) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        const auto* lc =
            label.empty() ? nullptr : module->findLoopContext(label);
        if (!lc) lc = &module->currentLoopContext();
        emitTryFinallyUnwind(module, lc->tryFinallyDepth);
        // A skipped end-of-block pop_run leaves stale entries linked, which
        // poisons the next throw's chain walk.
        if (auto m = module->getCurrentMethod()) {
            m->emitFrameDropsToDepth(module, lc->dropFrameDepth);
        }
        builder->CreateBr(lc->breakTarget);
        llvm::BasicBlock* deadBB = llvm::BasicBlock::Create(
            *module->getLlvmContext(), "after_break",
            builder->GetInsertBlock()->getParent());
        builder->SetInsertPoint(deadBB);
        return nullptr;
    }

    // Lowers `continue` / `continue label;`: the same finally and drop unwinding
    // as break, then a branch to the loop's latch.
    llvm::Value* ContinueStatement::generateCode(CajetaModulePtr module) {
        if (!module->hasLoopContext()) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        const auto* lc =
            label.empty() ? nullptr : module->findLoopContext(label);
        if (!lc) lc = &module->currentLoopContext();
        emitTryFinallyUnwind(module, lc->tryFinallyDepth);
        if (auto m = module->getCurrentMethod()) {
            m->emitFrameDropsToDepth(module, lc->dropFrameDepth);
        }
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

    // Stashes the label on the module so the next pushLoopContext (typically the
    // inner loop) picks it up, then runs the labeled statement.
    llvm::Value* IdentifierLabel::generateCode(CajetaModulePtr module) {
        if (!identifier.empty()) {
            module->setPendingLoopLabel(identifier);
        }
        if (body) body->generateCode(module);
        return nullptr;
    }

    llvm::Value* SemiStatement::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    // Analysis-walk descent: hand each privately-parked payload to the visitor.
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