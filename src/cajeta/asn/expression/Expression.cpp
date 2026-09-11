//
// Created by James Klappenbach on 3/19/22.
//

#include "Expression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/ownership/TitleClassifier.h"
#include "cajeta/dbg/LineInfoCodegen.h"
#include "cajeta/prof/ProfileCodegen.h"
#include "cajeta/compile/ExcFrameSetjmp.h"
#include "cajeta/compile/ScriptUnitSynthesis.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/field/StackField.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/field/HeapField.h"
#include "cajeta/util/LiteralUtils.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaVector.h"
#include "cajeta/type/VectorOps.h"
#include "cajeta/type/CajetaMatrix.h"
#include "cajeta/type/MatrixOps.h"
#include "cajeta/type/CajetaTask.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/error/ExplicitCastRequiredException.h"
#include "cajeta/error/InvalidOperandException.h"
#include "BinaryOpExpression.h"
#include "DotExpression.h"
#include "LiteralExpression.h"
#include "AggregateInitializerExpression.h"
#include "MethodCallExpression.h"
#include "CallExpression.h"
#include "NewExpression.h"
#include "../Block.h"
#include "../LocalVariableDeclaration.h"
#include "../VariableDeclarator.h"
#include "../Statement.h"
#include "../LocalVariableDeclaration.h"
#include "../VariableDeclarator.h"
#include "../../error/Exception.h"
#include "../../error/DiagnosticEngine.h"
#include "../../error/Diagnostics.h"
#include "ArrayLowering.h"

namespace cajeta {
    ExpressionPtr collectionLiteralFromArray(CajetaTypePtr target,
                                             const ExpressionPtr& literal) {
        auto lit = dynamic_pointer_cast<ArrayLiteralExpression>(literal);
        if (!lit) return nullptr;
        if (dynamic_pointer_cast<CajetaArray>(target)) return nullptr;
        auto cls = dynamic_pointer_cast<CajetaClass>(target);
        if (!cls) return nullptr;

        const auto& targs = cls->getTypeArguments();
        if (!targs.empty()) {
            lit->setElementType(targs[0]);
        }
        bool stackAlloc = lit->isStackAlloc();
        bool sharedAlloc = lit->isSharedAlloc();
        lit->setStackAlloc(false);
        lit->setSharedAlloc(false);

        string shortName = cls->getQName()->getTypeName();
        auto lt = shortName.find('<');
        if (lt != string::npos) shortName = shortName.substr(0, lt);

        vector<MethodCallParameter> params;
        MethodCallParameter entry;
        entry.expression = lit;
        params.push_back(std::move(entry));

        auto rest = make_shared<ClassCreatorRest>(std::move(params), nullptr);
        auto neu = make_shared<NewExpression>(nullptr);
        neu->setTypeName(shortName);
        neu->setTypeArguments(targs);
        neu->setCreatorRest(rest);
        if (stackAlloc) neu->setStackAlloc(true);
        if (sharedAlloc) neu->setSharedAlloc(true);
        neu->setSourceSpan(lit->getSourceLine(), lit->getSourceColumn());
        return neu;
    }

bool cajetaRhsCarriesRedundantSharp(
        CajetaParser::ExpressionContext* rhs) {
    if (!rhs || rhs->REFERENCE() == nullptr) return false;
    return rhs->expression().size() == 1;
}

    // `(Name)(operand)` — returns the destination type when this postfix-call node is
    // really a CAST (parenthesized single identifier, one plain argument, and the name
    // resolves to a type), else null to leave it a call. Shape first, resolution last.
    static CajetaTypePtr castDestOfParenCallee(
            CajetaParser::ExpressionContext* ctx) {
        if (ctx->expression().size() != 1 || !ctx->parameterList()) {
            return nullptr;
        }
        auto entries = ctx->parameterList()->parameterEntry();
        if (entries.size() != 1) return nullptr;
        auto* only = entries[0];
        if (!only->expression() || only->REFERENCE() || only->parameterLabel()) {
            return nullptr;
        }
        auto* calleePrim = ctx->expression(0)->primary();
        if (!calleePrim || !calleePrim->LPAREN() || !calleePrim->expression()) {
            return nullptr;
        }
        auto* innerPrim = calleePrim->expression()->primary();
        if (!innerPrim || !innerPrim->identifier()) return nullptr;
        if (innerPrim->getText() != innerPrim->identifier()->getText()) {
            return nullptr;
        }
        return CajetaType::resolveNamed(
            QualifiedName::getOrInsert(innerPrim->identifier()->getText(),
                                       "code"),
            nullptr);
    }

    // Builds the Expression node for one parser expression context: the operator or
    // keyword present on `ctx` selects the node kind, then the loop at the bottom
    // attaches the child expressions (a `#=` RHS is wrapped in a mode-carrying Move).
    ExpressionPtr Expression::fromContext(CajetaParser::ExpressionContext* ctx) {
        antlr4::Token* token = ctx->getStart();
        ExpressionPtr result = nullptr;
        bool sharpAssign = false;
        bool castOfParenCallee = false;
        if (ctx->ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_ASSIGN, token);
        } else if (ctx->SHARP_ASSIGN()) {
            // `dst #= v` builds one assignment node with the RHS wrapped in a MoveExpression
            // below. It is MODE-CARRYING, not an unconditional `dst = #v`: the slot's title
            // bit is armed only when a title was actually tendered.
            result = make_shared<BinaryOpExpression>(BINARY_OP_ASSIGN, token);
            sharpAssign = true;
        } else if (ctx->COLONCOLON()) {
            // Method reference `expr::id` / `Type::id` / `Type::heap`. Checked before the
            // identifier form so the token-bearing constructor form is not mis-routed.
            CajetaTypePtr methodRefRecvType;
            ExpressionPtr methodRefRecvExpr;
            std::string methodRefName;
            bool methodRefIsCtor = ctx->HEAP() != nullptr;
            if (!ctx->typeType().empty()) {
                methodRefRecvType = CajetaType::fromContext(
                    ctx->typeType(0), nullptr);
            } else if (!ctx->expression().empty()) {
                methodRefRecvExpr = Expression::fromContext(ctx->expression(0));
            }
            if (!methodRefIsCtor && ctx->identifier()) {
                methodRefName = ctx->identifier()->getText();
            }
            result = make_shared<MethodReferenceExpression>(token,
                methodRefRecvType, methodRefRecvExpr,
                std::move(methodRefName), methodRefIsCtor);
        } else if (ctx->primary()) {
            result = PrimaryExpression::fromContext(ctx->primary());
        } else if (ctx->DOT()) {
            // DOT consumes all six suffix forms and is checked before methodCall, because
            // `obj.foo()` matches both and DOT must win.
            if (ctx->SUPER() || ctx->superSuffix()) {
                result = make_shared<UnsupportedExpression>("super call", token);
            } else if (ctx->innerCreator()) {
                result = make_shared<UnsupportedExpression>(
                    "inner-class instantiation (obj.heap Inner())", token);
            } else if (ctx->methodCall()) {
                result = make_shared<MethodCallExpression>(ctx->methodCall(), token);
            } else {
                result = make_shared<DotExpression>(ctx, token);
            }
        } else if (ctx->methodCall()) {
            result = make_shared<MethodCallExpression>(ctx->methodCall(), token);
        } else if (ctx->HEAP()) {
            if (ctx->creator()) {
                result = make_shared<NewExpression>(ctx->creator(), token);
            } else if (ctx->aggregateInitializer()) {
                auto agg = make_shared<AggregateInitializerExpression>(
                    ctx->aggregateInitializer(), token);
                agg->setStackAlloc(false);
                result = agg;
            } else if (ctx->arrayLiteral()) {
                result = arrayOrMapLiteralFromContext(
                    ctx->arrayLiteral(), token, /*stack=*/false, /*shared=*/false);
            }
        } else if (ctx->STACK()) {
            if (ctx->aggregateInitializer()) {
                result = make_shared<AggregateInitializerExpression>(
                    ctx->aggregateInitializer(), token);
            } else if (ctx->creator()) {
                auto newExpr = make_shared<NewExpression>(ctx->creator(), token);
                newExpr->setStackAlloc(true);
                result = newExpr;
            } else if (ctx->arrayLiteral()) {
                result = arrayOrMapLiteralFromContext(
                    ctx->arrayLiteral(), token, /*stack=*/true, /*shared=*/false);
            }
        } else if (ctx->SHARED()) {
            // `shared` is GPU workgroup memory (device-only): the kernel lowerer turns
            // `shared T[N]` into one addrspace(3) global, and the host path rejects it.
            if (ctx->creator()) {
                auto newExpr = make_shared<NewExpression>(ctx->creator(), token);
                newExpr->setSharedAlloc(true);
                result = newExpr;
            } else if (ctx->aggregateInitializer()) {
                auto agg = make_shared<AggregateInitializerExpression>(
                    ctx->aggregateInitializer(), token);
                agg->setStackAlloc(false);
                result = agg;  // host path rejects; v1 has no shared-aggregate
            } else if (ctx->arrayLiteral()) {
                result = arrayOrMapLiteralFromContext(
                    ctx->arrayLiteral(), token, /*stack=*/false, /*shared=*/true);
            }
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), ctx->primary() != nullptr);
        } else if (ctx->LPAREN()) {
            // Two forms carry a top-level LPAREN: a cast (`'(' typeType ')' expression`) and
            // a postfix call. The cast carries a typeType; the call does not. A bare
            // `foo(...)` lives inside a MethodCallContext and never surfaces here.
            if (!ctx->typeType().empty()) {
                // Intersection casts (multiple typeTypes) aren't supported yet; take the first.
                CajetaTypePtr destType = CajetaType::fromContext(ctx->typeType(0), nullptr);
                result = make_shared<CastExpression>(destType, token);
            } else if (CajetaTypePtr namedDest = castDestOfParenCallee(ctx)) {
                // `(Name)(expr)` — the postfix-call alternative wins this in the grammar, so the
                // cast is reinterpreted HERE: reordering the alternatives would lift casts above
                // every suffix operator between them and regress `(T) a.b()` to `((T) a).b()`.
                result = make_shared<CastExpression>(namedDest, token);
                castOfParenCallee = true;
            } else {
                result = make_shared<CallExpression>(ctx, token);
            }
        } else if (ctx->LBRACK() && ctx->COLON() && !ctx->QUESTION()) {
            // COLON with LBRACK on the SAME ctx only occurs for the slice alternative — a
            // ternary carries QUESTION, and a nested index/ternary is a child context.
            result = make_shared<ArraySliceExpression>(ctx, token);
        } else if (ctx->LBRACK()) {
            result = make_shared<ArrayIndexExpression>(ctx, token);
        } else if (!ctx->BITAND().empty()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITAND, token);
        } else if (ctx->ADD()) {
            // The same ADD/SUB tokens cover binary and prefix unary; `ctx->prefix` tags unary.
            result = ctx->prefix
                ? static_pointer_cast<Expression>(make_shared<PrefixExpression>(PREFIX_OP_POSITIVE, token))
                : static_pointer_cast<Expression>(make_shared<BinaryOpExpression>(BINARY_OP_ADD, token));
        } else if (ctx->SUB()) {
            result = ctx->prefix
                ? static_pointer_cast<Expression>(make_shared<PrefixExpression>(PREFIX_OP_NEGATIVE, token))
                : static_pointer_cast<Expression>(make_shared<BinaryOpExpression>(BINARY_OP_SUB, token));
        } else if (ctx->MUL()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_MUL, token);
        } else if (ctx->DIV()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_DIV, token);
        } else if (ctx->MOD()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_MOD, token);
        } else if (ctx->INC()) {
            if (ctx->prefix) {
                result = make_shared<PrefixExpression>(PREFIX_OP_INC, token);
            } else {
                result = make_shared<PostfixExpression>(POSTFIX_OP_INC, token);
            }
        } else if (ctx->DEC()) {
            if (ctx->prefix) {
                result = make_shared<PrefixExpression>(PREFIX_OP_DEC, token);
            } else {
                result = make_shared<PostfixExpression>(POSTFIX_OP_DEC, token);
            }
        } else if (ctx->TILDE()) {
            result = make_shared<PrefixExpression>(PREFIX_OP_BITNOT, token);
        } else if (ctx->BANG()) {
            result = make_shared<PrefixExpression>(PREFIX_OP_LOGNOT, token);
        } else if (ctx->lambdaExpression()) {
            // Parameter forms: explicit types, `()`, a bare-name list, and a single bare name
            // (those two take their types from context). Other forms fall through below.
            auto* lx = ctx->lambdaExpression();
            auto* lp = lx->lambdaParameters();
            std::vector<std::string> names;
            std::vector<CajetaTypePtr> types;
            bool acceptable = false;
            if (lp) {
                if (auto* fpl = lp->formalParameterList()) {
                    // `var`-list params must fall through to UnsupportedExpression: FormalParameter::
                    // fromContext throws UNRESOLVED_TYPE for `var` before the NOT_IMPLEMENTED funnel.
                    bool varForm = false;
                    for (auto* fp : fpl->formalParameter()) {
                        if (fp->typeType() && fp->typeType()->getText() == "var") {
                            varForm = true;
                            break;
                        }
                    }
                    if (!varForm) {
                        for (auto* fp : fpl->formalParameter()) {
                            if (auto p = FormalParameter::fromContext(fp, nullptr)) {
                                names.push_back(p->getName());
                                types.push_back(p->getType());
                            }
                        }
                        acceptable = !names.empty() || fpl->formalParameter().empty();
                    }
                } else if (lp->LPAREN() && lp->RPAREN() && !lp->lambdaLVTIList()) {
                    for (auto* id : lp->identifier()) {
                        names.push_back(id->getText());
                    }
                    acceptable = true;
                } else if (!lp->identifier().empty() && !lp->LPAREN()) {
                    names.push_back(lp->identifier(0)->getText());
                    acceptable = true;
                }
            }
            auto* lb = lx->lambdaBody();
            AbstractSyntaxNodePtr body;
            if (lb && lb->expression()) {
                body = Expression::fromContext(lb->expression());
            } else if (lb && lb->block()) {
                body = Statement::buildBlockFromContext(lb->block());
            }
            if (acceptable && body) {
                result = make_shared<LambdaExpression>(token,
                    std::move(names), std::move(types), std::move(body));
            } else {
                result = make_shared<UnsupportedExpression>(
                    "lambda expression (this form needs var-list params, "
                    "still unsupported in L2)", token);
            }
        } else if (ctx->switchExpression()) {
            // Arrow form with single-expression bodies; richer forms reject at codegen.
            auto* sx = ctx->switchExpression();
            ExpressionPtr disc = sx->parExpression()
                ? Expression::fromContext(sx->parExpression()->expression())
                : nullptr;
            list<SwitchExpression::Case> cases;
            for (auto* rule : sx->switchLabeledRule()) {
                SwitchExpression::Case cs;
                if (rule->CASE() && rule->expressionList()) {
                    for (auto* e : rule->expressionList()->expression()) {
                        cs.labels.push_back(Expression::fromContext(e));
                    }
                }
                auto outcome = rule->switchRuleOutcome();
                if (outcome) {
                    auto bs = outcome->blockStatement();
                    if (bs.size() == 1) {
                        auto* stmt = bs[0]->statement();
                        if (stmt && stmt->statementExpression) {
                            cs.body = Expression::fromContext(stmt->statementExpression);
                        }
                    }
                }
                cases.push_back(std::move(cs));
            }
            result = make_shared<SwitchExpression>(token, disc, std::move(cases));
        } else if (!ctx->LT().empty()) {
            // LT().size() == 2 in the grammar means '<' '<' (shift-left); a single '<' is comparison.
            result = make_shared<BinaryOpExpression>(
                ctx->LT().size() >= 2 ? BINARY_OP_SHIFTLEFT : BINARY_OP_LT, token);
        } else if (!ctx->GT().empty()) {
            // GT().size() == 2 means '>' '>' (shift-right); 3 means '>' '>' '>' (unsigned shift); 1 is comparison.
            BinaryOp op;
            switch (ctx->GT().size()) {
                case 3:  op = BINARY_OP_USHIFTRIGHT; break;
                case 2:  op = BINARY_OP_SHIFTRIGHT;  break;
                default: op = BINARY_OP_GT;          break;
            }
            result = make_shared<BinaryOpExpression>(op, token);
        } else if (ctx->LE()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_LE, token);
        } else if (ctx->GE()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_GE, token);
        } else if (ctx->EQUAL()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_EQ, token);
        } else if (ctx->NOTEQUAL()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_NE, token);
        } else if (ctx->CARET()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITXOR, token);
        } else if (ctx->BITOR()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITOR, token);
        } else if (ctx->AND()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_LOGAND, token);
        } else if (ctx->OR()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_LOGOR, token);
        } else if (ctx->QUESTION()) {
            // The grammar matches QUESTION and COLON together; QUESTION is the discriminator.
            // Children are populated as [cond, then, else] by the loop at the bottom.
            result = make_shared<BooleanSwitchExpression>(token);
        } else if (ctx->ADD_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_ADD_EQUALS, token);
        } else if (ctx->SUB_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_SUB_EQUALS, token);
        } else if (ctx->MUL_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_MUL_EQUALS, token);
        } else if (ctx->DIV_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_DIV_EQUALS, token);
        } else if (ctx->AND_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITAND_EQUALS, token);
        } else if (ctx->OR_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITOR_EQUALS, token);
        } else if (ctx->XOR_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITXOR_EQUALS, token);
        } else if (ctx->RSHIFT_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_SHIFTRIGHT_EQUALS, token);
        } else if (ctx->URSHIFT_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_USHIFTRIGHT_EQUALS, token);
        } else if (ctx->LSHIFT_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_SHIFTLEFT_EQUALS, token);
        } else if (ctx->MOD_ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_MOD_EQUALS, token);
        } else if (ctx->THIS()) {
            result = make_shared<ThisExpression>(ctx);
        } else if (ctx->REFERENCE()) {
            // `#expr` — wrapped in MoveExpression so consumers detect it by cast.
            result = make_shared<MoveExpression>(token);
        } else if (ctx->AWAIT()) {
            result = make_shared<AwaitExpression>(token);
        } else if (ctx->SPAWN()) {
            result = make_shared<SpawnExpression>(token);
        } else if (ctx->DETACH()) {
            result = make_shared<DetachExpression>(token);
        } else if (ctx->INSTANCEOF()) {
            // In the PATTERN form (`expr instanceof Type id`) the type and identifier live
            // inside `pattern`, and the top-level typeType list is empty.
            CajetaTypePtr targetType;
            string patternName;
            if (!ctx->typeType().empty()) {
                targetType = CajetaType::fromContext(ctx->typeType(0), nullptr);
            } else if (ctx->pattern()) {
                auto* pat = ctx->pattern();
                if (pat->typeType()) {
                    targetType = CajetaType::fromContext(pat->typeType(), nullptr);
                }
                if (pat->identifier()) {
                    patternName = pat->identifier()->getText();
                }
            }
            result = make_shared<InstanceOfExpression>(targetType, patternName, token);
        }

        if (result && castOfParenCallee) {
            // The operand is the postfix call's single ARGUMENT: the node's only
            // `expression()` child is the callee `(Name)`, which is the destination type.
            auto* only = ctx->parameterList()->parameterEntry(0);
            result->addChild(Expression::fromContext(only->expression()));
        } else if (result) {
            if (!ctx->expression().empty()) {
                size_t childIndex = 0;
                for (auto childContext: ctx->expression()) {
                    ExpressionPtr child = Expression::fromContext(childContext);
                    if (sharpAssign && childIndex == 1
                            && cajetaRhsCarriesRedundantSharp(childContext)) {
                        // `x #= #y` is valid and means `x #= y`, so it warns rather than rejects; the
                        // report itself fires from MoveExpression::generateCode, which has the module.
                        if (auto redMv = dynamic_pointer_cast<MoveExpression>(child)) {
                            redMv->setRedundantSharp(true);
                        }
                    }
                    if (sharpAssign && childIndex == 1) {
                        auto mv = make_shared<MoveExpression>(
                            childContext->getStart());
                        mv->addChild(child);
                        // Mark the wrapper MODE-CARRYING: `#=` claims no title, only forwards the mode
                        // the source holds, so the transfer-of-a-borrow rejection must not fire on it.
                        mv->setModeCarrying(true);
                        mv->setSharpStore(true);
                        child = mv;
                    }
                    // The legacy `dst = #v`: a plain ASSIGN whose RHS is itself a parsed `#expr`.
                    // Only reachable when !sharpAssign, so `dst #= #v` is correctly left alone.
                    if (!sharpAssign && childIndex == 1 && ctx->ASSIGN()) {
                        if (auto rhsMove = dynamic_pointer_cast<MoveExpression>(child)) {
                            rhsMove->setLegacyTransferAssign(true);
                        }
                    }
                    result->addChild(child);
                    ++childIndex;
                }
            }
        }
        return result;
    }

    ArrayIndexExpression::ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(
        token) { exprKind = ExprKind::ArrayIndex;

    }

    ArraySliceExpression::ArraySliceExpression(
        CajetaParser::ExpressionContext* ctx, antlr4::Token* token)
        : Expression(token) { exprKind = ExprKind::ArraySlice;
    }

    // Types `base[a:b]` as `Slice<E>`; sub-slicing a Slice keeps its type. Leaves
    // resolvedType null when the base is not resolvable yet — generateCode re-resolves
    // against the live scope and hard-errors there.
    void ArraySliceExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (resolvedType || children.empty()) return;
        auto base = dynamic_pointer_cast<Expression>(children[0]);
        if (!base) return;
        if (!base->getResolvedType()) base->resolveTypes(module);
        CajetaTypePtr bt = base->getResolvedType();
        if (auto arr = dynamic_pointer_cast<CajetaArray>(bt)) {
            auto sliceTmpl = dynamic_pointer_cast<CajetaClass>(
                CajetaType::of("Slice", "cajeta.lang"));
            if (sliceTmpl) {
                resolvedType = sliceTmpl->instantiate({arr->getElementType()});
            }
        } else if (auto cls = dynamic_pointer_cast<CajetaClass>(bt)) {
            if (cls->getQName()
                    && cls->getQName()->getPackageName() == "cajeta.lang"
                    && cls->getQName()->getTypeName().rfind("Slice", 0) == 0) {
                resolvedType = bt;
            }
        }
    }

    // Emits the window value: clamps `from`/`to` against the base's length and builds a
    // `{ store, off, len }` Slice in an entry-block alloca, returning its address.
    // Throws CAJETA_ERROR_SLICE_BASE when the base is neither an array nor a Slice.
    llvm::Value* ArraySliceExpression::generateCode(CajetaModulePtr module) {
        if (!resolvedType) resolveTypes(module);
        if (!resolvedType) {
            throw Exception(
                "the window form `base[a:b]` requires an array or Slice<T> "
                "base",
                "CAJETA_ERROR_SLICE_BASE");
        }
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        auto sliceCls = dynamic_pointer_cast<CajetaClass>(resolvedType);
        auto base = dynamic_pointer_cast<Expression>(children[0]);
        auto fromE = dynamic_pointer_cast<Expression>(children[1]);
        auto toE = dynamic_pointer_cast<Expression>(children[2]);
        if (!sliceCls || !base || !fromE || !toE) return nullptr;

        auto asI64 = [&](ExpressionPtr e) {
            llvm::Value* v = loadIfLValue(module, e->generateCode(module), e);
            if (v->getType() != i64Ty) {
                v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
            }
            return v;
        };

        // Field slots on the Slice<E> body (never hardcode indices).
        llvm::StructType* bodyTy =
            llvm::dyn_cast_or_null<llvm::StructType>(sliceCls->getLlvmType());
        if (!bodyTy) return nullptr;
        auto fieldIdx = [&](const char* name) -> unsigned {
            auto& props = sliceCls->getProperties();
            auto it = props.find(name);
            return it == props.end() ? 0u
                : (unsigned) sliceCls->getFieldLlvmIndex(it->second);
        };
        unsigned storeIdx = fieldIdx("store");
        unsigned offIdx = fieldIdx("off");
        unsigned lenIdx = fieldIdx("len");

        llvm::Value* from = asI64(fromE);
        llvm::Value* to = asI64(toE);
        llvm::Value* zero = llvm::ConstantInt::get(i64Ty, 0);

        llvm::Value* storeV;
        llvm::Value* baseOff;
        llvm::Value* limit;   // window length of the base (masked root count / base.len)
        CajetaTypePtr bt = base->getResolvedType();
        if (dynamic_pointer_cast<CajetaArray>(bt)) {
            llvm::Value* arrPtr = loadIfLValue(module, base->generateCode(module), base);
            storeV = arrPtr;
            baseOff = zero;
            // Root count word, masked of the shared sign bit (slice-spec §3.3).
            llvm::Value* cnt = builder->CreateLoad(i64Ty, arrPtr, "slice_root_count");
            limit = builder->CreateAnd(cnt,
                llvm::ConstantInt::get(i64Ty, 0x7FFFFFFFFFFFFFFFULL));
        } else {
            // Slice base: its VALUE address (a value-type lvalue IS the storage).
            llvm::Value* baseAddr = base->generateCode(module);
            storeV = builder->CreateLoad(ptrTy,
                builder->CreateStructGEP(bodyTy, baseAddr, storeIdx), "sub_store");
            baseOff = builder->CreateLoad(i64Ty,
                builder->CreateStructGEP(bodyTy, baseAddr, offIdx), "sub_off");
            limit = builder->CreateLoad(i64Ty,
                builder->CreateStructGEP(bodyTy, baseAddr, lenIdx), "sub_len");
        }

        from = builder->CreateSelect(
            builder->CreateICmpSLT(from, zero), zero, from, "slice_from");
        to = builder->CreateSelect(
            builder->CreateICmpSGT(to, limit), limit, to, "slice_to");
        to = builder->CreateSelect(
            builder->CreateICmpSLT(to, from), from, to, "slice_to2");

        // Build the value in an entry-block alloca (stable across the scope).
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
            parentFn->getEntryBlock().begin());
        llvm::Value* out = entryBuilder.CreateAlloca(bodyTy, nullptr, "slice_val");

        builder->CreateStore(storeV,
            builder->CreateStructGEP(bodyTy, out, storeIdx));
        builder->CreateStore(builder->CreateAdd(baseOff, from),
            builder->CreateStructGEP(bodyTy, out, offIdx));
        builder->CreateStore(builder->CreateSub(to, from),
            builder->CreateStructGEP(bodyTy, out, lenIdx));
        return out;
    }

    ArrayLiteralExpression::ArrayLiteralExpression(
        vector<ExpressionPtr> elems, antlr4::Token* token)
        : Expression(token), elements(std::move(elems)) { exprKind = ExprKind::ArrayLiteral;
        for (auto& e : elements) addChild(e);
    }

    ExpressionPtr arrayOrMapLiteralFromContext(
            CajetaParser::ArrayLiteralContext* ctx, antlr4::Token* token,
            bool stackAlloc, bool sharedAlloc) {
        auto* entriesCtx = ctx->arrayLiteralEntries();
        if (!entriesCtx) {
            auto lit = make_shared<ArrayLiteralExpression>(
                vector<ExpressionPtr>{}, token);
            lit->setStackAlloc(stackAlloc);
            lit->setSharedAlloc(sharedAlloc);
            return lit;
        }
        auto entryCtxs = entriesCtx->arrayLiteralEntry();
        bool emptyMap = entriesCtx->COLON() != nullptr && entryCtxs.empty();
        bool anyMap = emptyMap, anySeq = false;
        for (auto* e : entryCtxs) {
            (e->COLON() != nullptr ? anyMap : anySeq) = true;
        }
        if (anyMap && anySeq) {
            throw locatedException(
                token ? token->getLine() : 0,
                (token ? token->getCharPositionInLine() : 0) + 1,
                "bracket literal mixes `key: value` map entries with plain "
                "sequence elements; use one form (all `k: v`, or all values)",
                "CAJETA_ERROR_MIXED_MAP_SEQUENCE");
        }
        if (anyMap) {
            vector<pair<ExpressionPtr, ExpressionPtr>> mapEntries;
            for (auto* e : entryCtxs) {
                auto exprs = e->expression();
                mapEntries.emplace_back(
                    Expression::fromContext(exprs[0]),
                    Expression::fromContext(exprs[1]));
            }
            auto lit = make_shared<MapLiteralExpression>(
                std::move(mapEntries), token);
            lit->setStackAlloc(stackAlloc);
            lit->setSharedAlloc(sharedAlloc);
            return lit;
        }
        vector<ExpressionPtr> elems;
        for (auto* e : entryCtxs) {
            elems.push_back(Expression::fromContext(e->expression()[0]));
        }
        auto lit = make_shared<ArrayLiteralExpression>(std::move(elems), token);
        lit->setStackAlloc(stackAlloc);
        lit->setSharedAlloc(sharedAlloc);
        return lit;
    }

    namespace {
        // Numeric width from the BIT_* flags (array-literals §3.3).
        int arrayLiteralNumericBits(CajetaTypeFlags f) {
            if (f & BIT_128_FLAG) return 128;
            if (f & BIT_64_FLAG) return 64;
            if (f & BIT_32_FLAG) return 32;
            if (f & BIT_16_FLAG) return 16;
            return 8;
        }

        // Nearest common superclass of two reference types, or null if none
        // (array-literals §3.3). Walks the super-class chains by canonical name.
        CajetaTypePtr arrayLiteralCommonSuper(CajetaTypePtr a, CajetaTypePtr b) {
            auto ca = dynamic_pointer_cast<CajetaClass>(a);
            auto cb = dynamic_pointer_cast<CajetaClass>(b);
            if (!ca || !cb) return nullptr;
            vector<CajetaClassPtr> aChain;
            std::set<std::string> aSeen;
            vector<CajetaClassPtr> stack{ca};
            while (!stack.empty()) {
                auto c = stack.back(); stack.pop_back();
                if (!c || !aSeen.insert(c->toCanonical()).second) continue;
                aChain.push_back(c);
                for (auto& s : c->getSuperClasses()) if (s) stack.push_back(s);
            }
            std::set<std::string> bSeen;
            vector<CajetaClassPtr> bStack{cb};
            while (!bStack.empty()) {
                auto c = bStack.back(); bStack.pop_back();
                if (!c || !bSeen.insert(c->toCanonical()).second) continue;
                for (auto& anc : aChain)
                    if (anc->toCanonical() == c->toCanonical()) return anc;
                for (auto& s : c->getSuperClasses()) if (s) bStack.push_back(s);
            }
            return nullptr;
        }
    } // namespace

    CajetaTypePtr ArrayLiteralExpression::unifyElementType(CajetaModulePtr module) {
        if (elements.empty()) {
            throw locatedException(getSourceLine(), getSourceColumn() + 1,
                "cannot infer the element type of an empty array literal '[]'; "
                "give it a target type (e.g. int32[] xs = [])",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        CajetaTypePtr result;
        for (auto& e : elements) {
            CajetaTypePtr t = e->getResolvedType();
            if (!t) { e->resolveTypes(module); t = e->getResolvedType(); }
            if (!t) {
                throw locatedException(e->getSourceLine(), e->getSourceColumn() + 1,
                    "array literal element has no resolved type",
                    "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
            }
            if (!result) { result = t; continue; }
            if (result->toCanonical() == t->toCanonical()) continue;

            CajetaTypeFlags rf = result->getTypeFlags();
            CajetaTypeFlags tf = t->getTypeFlags();
            if ((rf & NUMBER_FLAG) && (tf & NUMBER_FLAG)) {
                bool rFloat = (rf & FLOAT_FLAG) != 0;
                bool tFloat = (tf & FLOAT_FLAG) != 0;
                if (rFloat != tFloat) {
                    result = rFloat ? result : t;
                } else if (arrayLiteralNumericBits(tf) > arrayLiteralNumericBits(rf)) {
                    result = t;
                }
                continue;
            }
            CajetaTypePtr common = arrayLiteralCommonSuper(result, t);
            if (!common) {
                throw locatedException(getSourceLine(), getSourceColumn() + 1,
                    "array literal elements '" + result->toCanonical() + "' and '"
                    + t->toCanonical() + "' have no common type; give the literal "
                    "a target type",
                    "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
            }
            result = common;
        }
        return result;
    }

    // Resolves the elements, then infers the element type unless a target type was
    // already pushed. An empty literal with no target stays unresolved here and is
    // decided, or diagnosed, at generateCode once the target has arrived.
    void ArrayLiteralExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (!elementType && !elements.empty()) {
            elementType = unifyElementType(module);
        }
        if (elementType) {
            resolvedType = make_shared<CajetaArray>(module, elementType);
        }
    }

    // Allocates the array and stores each element, first pushing the element type into
    // nested `[...]` and prefixless `{...}` elements. Rejects `shared` placement:
    // workgroup memory is emitted only by the @Kernel device lowerer.
    llvm::Value* ArrayLiteralExpression::generateCode(CajetaModulePtr module) {
        if (sharedAlloc) {
            throw Exception(
                "`shared` placement is only valid inside an @Kernel body "
                "(GPU workgroup-shared memory)", "XPU-K03");
        }
        if (!elementType) {
            elementType = unifyElementType(module);
        }
        if (auto innerArr = dynamic_pointer_cast<CajetaArray>(elementType)) {
            for (auto& e : elements) {
                if (auto innerLit =
                        dynamic_pointer_cast<ArrayLiteralExpression>(e)) {
                    innerLit->setElementType(innerArr->getElementType());
                }
            }
        } else if (auto elemClass =
                       dynamic_pointer_cast<CajetaClass>(elementType)) {
            // A reference-class element escapes into the array, so its aggregate must be heap:
            // a bare `{...}` defaults to stack and would dangle. Value types copy inline.
            bool elemIsValueType = elemClass->isValueType();
            for (auto& e : elements) {
                if (auto aggLit =
                        dynamic_pointer_cast<AggregateInitializerExpression>(e)) {
                    aggLit->setExpectedType(elemClass);
                    if (!elemIsValueType) aggLit->setStackAlloc(false);
                }
            }
        }
        borrowedLocalSlots.clear();
        return emitArrayFromElements(module, elementType, children, arenaEligible,
                                     &borrowedLocalSlots);
    }

    // ---- MapLiteralExpression (collection-literals §3) ----------------------

    MapLiteralExpression::MapLiteralExpression(
        vector<pair<ExpressionPtr, ExpressionPtr>> entries, antlr4::Token* token)
        : Expression(token), entries(std::move(entries)) { exprKind = ExprKind::MapLiteral;
        for (auto& e : this->entries) {
            if (e.first) addChild(e.first);
            if (e.second) addChild(e.second);
        }
    }

    // Exposes the pushed target type, if any, so a slot-sizing consumer sees
    // `HashMap<K,V>`; the concrete map type is decided at generateCode.
    void MapLiteralExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (!resolvedType) resolvedType = expectedType;
    }

    namespace {
        // Unify a set of expressions to a single element type by reusing the
        // sequence literal's least-upper-bound logic (numeric widest / nearest
        // common superclass). Throws on empty / no-common, like the array path.
        CajetaTypePtr unifyExprs(CajetaModulePtr module,
                                 const vector<ExpressionPtr>& exprs,
                                 antlr4::Token* token) {
            auto tmp = make_shared<ArrayLiteralExpression>(exprs, token);
            tmp->resolveTypes(module);
            if (auto arr = dynamic_pointer_cast<CajetaArray>(
                    tmp->getResolvedType())) {
                return arr->getElementType();
            }
            return nullptr;
        }
    } // namespace

    // Lowers `["k": v, ...]` to `heap HashMap<K,V>(Pair<K,V>[])`: resolves K and V from
    // the pushed target or by unifying the entries, builds one `heap Pair(k, v)` per
    // entry, then generates that construction. Rejects `shared` placement.
    llvm::Value* MapLiteralExpression::generateCode(CajetaModulePtr module) {
        if (sharedAlloc) {
            throw Exception(
                "`shared` placement is only valid inside an @Kernel body "
                "(GPU workgroup-shared memory)", "XPU-K03");
        }
        CajetaTypePtr keyType, valType;
        string mapName = "HashMap";
        if (auto cls = dynamic_pointer_cast<CajetaClass>(expectedType)) {
            const auto& targs = cls->getTypeArguments();
            if (targs.size() >= 2) {
                keyType = targs[0];
                valType = targs[1];
                mapName = cls->getQName()->getTypeName();
                auto lt = mapName.find('<');
                if (lt != string::npos) mapName = mapName.substr(0, lt);
            }
        }
        if (!keyType || !valType) {
            if (entries.empty()) {
                throw locatedException(getSourceLine(), getSourceColumn() + 1,
                    "cannot infer the type of an empty map literal `[:]`; give "
                    "it a target type (e.g. HashMap<String,int32> m = [:])",
                    "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
            }
            vector<ExpressionPtr> keys, vals;
            for (auto& e : entries) { keys.push_back(e.first); vals.push_back(e.second); }
            keyType = unifyExprs(module, keys, nullptr);
            valType = unifyExprs(module, vals, nullptr);
            if (!keyType || !valType) {
                throw locatedException(getSourceLine(), getSourceColumn() + 1,
                    "cannot infer the key/value type of this map literal; give "
                    "it a target type (e.g. HashMap<K,V> m = [...])",
                    "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
            }
        }

        CajetaTypePtr pairBase = CajetaType::of("Pair");
        auto pairClass = dynamic_pointer_cast<CajetaClass>(pairBase);
        if (!pairClass || !pairClass->isTemplate()) {
            if (auto t = CajetaType::findTemplateByShortName("Pair"))
                pairClass = dynamic_pointer_cast<CajetaClass>(t);
        }
        if (!pairClass) {
            throw Exception("map literal lowering: cajeta.lang.Pair is unavailable",
                            "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        CajetaTypePtr pairType = pairClass->instantiate({keyType, valType});

        vector<ExpressionPtr> pairCreators;
        // A reference-class key/value escapes into the map, so its aggregate must be heap.
        auto pushAgg = [](const ExpressionPtr& e, const CajetaTypePtr& t) {
            if (auto agg = dynamic_pointer_cast<AggregateInitializerExpression>(e)) {
                if (auto cls = dynamic_pointer_cast<CajetaClass>(t)) {
                    agg->setExpectedType(t);
                    if (!cls->isValueType()) agg->setStackAlloc(false);
                }
            }
        };
        for (auto& e : entries) {
            pushAgg(e.first, keyType);
            pushAgg(e.second, valType);
            vector<MethodCallParameter> pairArgs;
            MethodCallParameter kp; kp.expression = e.first; pairArgs.push_back(kp);
            MethodCallParameter vp; vp.expression = e.second; pairArgs.push_back(vp);
            auto pairRest = make_shared<ClassCreatorRest>(std::move(pairArgs), nullptr);
            auto pairNew = make_shared<NewExpression>(nullptr);
            pairNew->setTypeName("Pair");
            pairNew->setTypeArguments({keyType, valType});
            pairNew->setCreatorRest(pairRest);
            pairNew->setSourceSpan(getSourceLine(), getSourceColumn());
            pairCreators.push_back(pairNew);
        }

        auto pairArray = make_shared<ArrayLiteralExpression>(
            std::move(pairCreators), nullptr);
        pairArray->setElementType(pairType);
        pairArray->setSourceSpan(getSourceLine(), getSourceColumn());

        vector<MethodCallParameter> mapArgs;
        MethodCallParameter mapArg; mapArg.expression = pairArray;
        mapArgs.push_back(mapArg);
        auto mapRest = make_shared<ClassCreatorRest>(std::move(mapArgs), nullptr);
        auto mapNew = make_shared<NewExpression>(nullptr);
        mapNew->setTypeName(mapName);
        mapNew->setTypeArguments({keyType, valType});
        mapNew->setCreatorRest(mapRest);
        if (stackAlloc) mapNew->setStackAlloc(true);
        mapNew->setSourceSpan(getSourceLine(), getSourceColumn());

        mapNew->resolveTypes(module);
        resolvedType = mapNew->getResolvedType();
        return mapNew->generateCode(module);
    }

    // One index level unwraps one array or vector layer (a matrix row yields
    // Vector<T,C>); for a class receiver the type is its `operator[]` return, which
    // needs the index expression resolved first.
    void ArrayIndexExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (!children.empty()) {
            if (auto exprChild = dynamic_pointer_cast<Expression>(children[0])) {
                CajetaTypePtr lhsType = exprChild->getResolvedType();
                if (auto arr = dynamic_pointer_cast<CajetaArray>(lhsType)) {
                    resolvedType = arr->getElementType();
                } else if (auto vecT = dynamic_pointer_cast<CajetaVector>(lhsType)) {
                    resolvedType = vecT->getElementType();
                } else if (auto matT = dynamic_pointer_cast<CajetaMatrix>(lhsType)) {
                    resolvedType = CajetaVector::getOrCreate(
                        module, matT->getElementType(), matT->getCols());
                } else if (auto klass = dynamic_pointer_cast<CajetaClass>(lhsType)) {
                    // Resolve the index expression first: the operator[] lookup needs a real
                    // parameter type to match against.
                    if (children.size() >= 2) {
                        if (auto idxExpr = dynamic_pointer_cast<Expression>(children[1])) {
                            if (!idxExpr->getResolvedType()) {
                                idxExpr->resolveTypes(module);
                            }
                            CajetaTypePtr idxType = idxExpr->getResolvedType();
                            if (idxType && !klass->isInterface()
                                    && !(klass->getTypeFlags() & PRIMITIVE_FLAG)) {
                                vector<ParameterEntry> entries;
                                entries.push_back(
                                    ParameterEntry(idxType, "", nullptr));
                                std::string name = "operator[]";
                                if (auto m = klass->resolveMethod(name, entries,
                                        /*isConstructor=*/false,
                                        /*floatingParams=*/false)) {
                                    resolvedType = m->getReturnType();
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Constant-folds an integer index off the AST — an integer literal, or unary +/- on
    // one — into `out`, returning whether it folded. Reads the AST because a checked
    // negation lowers to an intrinsic, so `f[-1]` is no folded ConstantInt.
    static bool tryEvalConstIntIndex(const AbstractSyntaxNodePtr& node, int64_t& out) {
        if (auto lit = dynamic_pointer_cast<IntegerLiteralExpression>(node)) {
            string raw = lit->getRawValue();
            __int128_t v;
            switch (lit->getIntegerLiteralType()) {
                case INTEGER_LITERAL_TYPE_HEX:    v = LiteralUtils::hexToInt128(raw, 64); break;
                case INTEGER_LITERAL_TYPE_BINARY: v = LiteralUtils::binaryToInt128(raw, 64); break;
                case INTEGER_LITERAL_TYPE_OCT:    v = LiteralUtils::octalToInt128(raw, 64); break;
                default:                          v = LiteralUtils::decimalToInt128(raw, 64); break;
            }
            out = (int64_t) v;
            return true;
        }
        if (auto pre = dynamic_pointer_cast<PrefixExpression>(node)) {
            PrefixOp op = pre->getOp();
            if ((op == PREFIX_OP_NEGATIVE || op == PREFIX_OP_POSITIVE)
                    && !pre->getChildren().empty()) {
                int64_t inner;
                if (tryEvalConstIntIndex(pre->getChildren()[0], inner)) {
                    out = (op == PREFIX_OP_NEGATIVE) ? -inner : inner;
                    return true;
                }
            }
        }
        return false;
    }

    // Emits ONE index level: children[0] is the indexed expression, children[1] the
    // index, and `a[i][j]` nests two nodes. Covers operator[] classes, vector lanes,
    // matrix rows and view element arrays; otherwise GEPs into the array header.
    llvm::Value* ArrayIndexExpression::generateCode(CajetaModulePtr module) {
        if (children.size() < 2) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);

        // `v[i]` reads via extractelement; the assignment form lives in BinaryOpExpression.
        if (auto lhsVecExpr = dynamic_pointer_cast<Expression>(children[0])) {
            if (!lhsVecExpr->getResolvedType()) lhsVecExpr->resolveTypes(module);
            if (auto vecT = dynamic_pointer_cast<CajetaVector>(
                    lhsVecExpr->getResolvedType())) {
                llvm::Value* vecVal = loadIfLValue(
                    module, children[0]->generateCode(module), lhsVecExpr);
                llvm::Value* idx = loadIfLValue(
                    module, children[1]->generateCode(module),
                    dynamic_pointer_cast<Expression>(children[1]));
                resolvedType = vecT->getElementType();
                llvm::Value* elt = vecops::extractLane(*builder, vecVal, idx);
                llvm::AllocaInst* slot = module->createEntryAlloca(
                    elt->getType(), "vec.idx.slot");
                builder->CreateStore(elt, slot);
                return slot;
            }
        }

        // `m[r]` yields the row Vector<T,C> (flat lanes [r*C, r*C+C)); `m[r][c]` composes.
        if (auto lhsMatExpr = dynamic_pointer_cast<Expression>(children[0])) {
            if (!lhsMatExpr->getResolvedType()) lhsMatExpr->resolveTypes(module);
            if (auto matT = dynamic_pointer_cast<CajetaMatrix>(
                    lhsMatExpr->getResolvedType())) {
                llvm::Value* matVal = loadIfLValue(
                    module, children[0]->generateCode(module), lhsMatExpr);
                llvm::Value* r = loadIfLValue(
                    module, children[1]->generateCode(module),
                    dynamic_pointer_cast<Expression>(children[1]));
                if (r->getType() != i32Ty)
                    r = builder->CreateIntCast(r, i32Ty, /*isSigned=*/false,
                                               "mat.row.idx");
                resolvedType = CajetaVector::getOrCreate(
                    module, matT->getElementType(), matT->getCols());
                llvm::Value* rowVal = matops::row(
                    *builder, matVal, matT->getRows(), matT->getCols(), r);
                llvm::AllocaInst* slot = module->createEntryAlloca(
                    rowVal->getType(), "mat.row.slot");
                builder->CreateStore(rowVal, slot);
                return slot;
            }
        }

        if (auto dotChild = dynamic_pointer_cast<DotExpression>(children[0])) {
            if (auto eaProp = dotChild->resolveViewElementArrayProperty(module)) {
                dotChild->setElementArrayPrefixMode(true);
                llvm::Value* prefixPtr = dotChild->generateCode(module);
                if (!prefixPtr) return nullptr;
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                ViewEndianness earrE = dotChild->getEarrViewType()
                    ? dotChild->getEarrViewType()->getEndianness()
                    : ViewEndianness::Host;
                llvm::Value* count = builder->CreateIntCast(
                    CajetaView::emitSwapIfNeeded(module, earrE,
                        builder->CreateLoad(i32Ty, prefixPtr, "earr_count32")),
                    i64Ty, /*isSigned=*/true, "earr_count");
                llvm::Value* idx = loadIfLValue(
                    module, children[1]->generateCode(module),
                    dynamic_pointer_cast<Expression>(children[1]));
                if (idx->getType() != i64Ty) {
                    idx = builder->CreateIntCast(idx, i64Ty, /*isSigned=*/true,
                        "earr_idx");
                }
                llvm::Value* inBounds = builder->CreateAnd(
                    builder->CreateICmpSGE(idx,
                        llvm::ConstantInt::get(i64Ty, 0)),
                    builder->CreateICmpSLT(idx, count), "earr_inbounds");
                llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                llvm::BasicBlock* failBB = llvm::BasicBlock::Create(
                    ctx, "earr_oob", parentFn);
                llvm::BasicBlock* okBB = llvm::BasicBlock::Create(
                    ctx, "earr_ok", parentFn);
                builder->CreateCondBr(inBounds, okBB, failBB);
                builder->SetInsertPoint(failBB);
                if (llvm::Function* throwFn =
                        module->getRuntimeFunction("__cajeta_throw")) {
                    llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
                    llvm::Value* tagPtr = builder->CreateIntToPtr(
                        llvm::ConstantInt::get(i64Ty, 0xCA1E7B00), ptrTy);
                    builder->CreateCall(throwFn, {tagPtr});
                }
                builder->CreateUnreachable();
                builder->SetInsertPoint(okBB);

                auto arrT = dynamic_pointer_cast<CajetaArray>(eaProp->getType());
                auto elemView = arrT ? dynamic_pointer_cast<CajetaView>(
                    arrT->getElementType()) : nullptr;
                // Offset table: an element's offset is table[region + i] (absolute), where
                // region = table[slot + 1]. Fixed-stride elements skip the region and use
                // stride math off the field start (table[slot] + 4).
                if (dotChild->getEarrTable()) {
                    llvm::Value* tbl = dotChild->getEarrTable();
                    llvm::Value* dataBase = dotChild->getEarrDataBase();
                    int slot = dotChild->getEarrSlot();
                    llvm::Value* absOff;
                    if (elemView && elemView->getVariableSizeFieldCount() == 0) {
                        llvm::Value* fieldStart = builder->CreateLoad(i64Ty,
                            builder->CreateInBoundsGEP(i64Ty, tbl,
                                llvm::ConstantInt::get(i64Ty, slot)),
                            "earr_tbl_start");
                        absOff = builder->CreateAdd(
                            builder->CreateAdd(fieldStart,
                                llvm::ConstantInt::get(i64Ty, 4)),
                            builder->CreateMul(idx, llvm::ConstantInt::get(
                                i64Ty, elemView->getFixedSize())),
                            "earr_tbl_stride");
                    } else {
                        llvm::Value* region = builder->CreateLoad(i64Ty,
                            builder->CreateInBoundsGEP(i64Ty, tbl,
                                llvm::ConstantInt::get(i64Ty, slot + 1)),
                            "earr_tbl_region");
                        absOff = builder->CreateLoad(i64Ty,
                            builder->CreateInBoundsGEP(i64Ty, tbl,
                                builder->CreateAdd(region, idx)),
                            "earr_tbl_elem");
                    }
                    llvm::Value* tblElemPtr = builder->CreateInBoundsGEP(
                        i8Ty, dataBase, absOff, "earr_elem");
                    if (elemView) {
                        resolvedType = elemView;
                        llvm::AllocaInst* slot2 = builder->CreateAlloca(
                            llvm::PointerType::get(ctx, 0), nullptr,
                            "earr.slot");
                        builder->CreateStore(tblElemPtr, slot2);
                        return slot2;
                    }
                    llvm::Value* sLen2 = builder->CreateIntCast(
                        CajetaView::emitSwapIfNeeded(module, earrE,
                            builder->CreateLoad(i32Ty, tblElemPtr,
                                "earr_str_len32")),
                        i64Ty, /*isSigned=*/true);
                    llvm::Value* sData2 = builder->CreateInBoundsGEP(
                        i8Ty, tblElemPtr, llvm::ConstantInt::get(i64Ty, 4),
                        "earr_str_data");
                    llvm::Function* toOwned2 = module->getRuntimeFunction(
                        "__cajeta_str_view_to_owned");
                    if (!toOwned2) return nullptr;
                    resolvedType = CajetaType::of("String");
                    llvm::Value* str2 = wrapCStringIntoClassString(module,
                        builder->CreateCall(toOwned2, {sData2, sLen2}),
                        "earr_str");
                    llvm::AllocaInst* sSlot2 = builder->CreateAlloca(
                        llvm::PointerType::get(ctx, 0), nullptr,
                        "earr.str.slot");
                    builder->CreateStore(str2, sSlot2);
                    return sSlot2;
                }
                llvm::Value* elemOff;
                if (elemView && elemView->getVariableSizeFieldCount() == 0) {
                    elemOff = builder->CreateAdd(
                        llvm::ConstantInt::get(i64Ty, 4),
                        builder->CreateMul(idx, llvm::ConstantInt::get(
                            i64Ty, elemView->getFixedSize())),
                        "earr_stride_off");
                } else {
                    llvm::BasicBlock* preBB = builder->GetInsertBlock();
                    llvm::BasicBlock* hdrBB = llvm::BasicBlock::Create(
                        ctx, "earr_walk_hdr", parentFn);
                    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                        ctx, "earr_walk_body", parentFn);
                    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(
                        ctx, "earr_walk_exit", parentFn);
                    builder->CreateBr(hdrBB);
                    builder->SetInsertPoint(hdrBB);
                    llvm::PHINode* kPhi = builder->CreatePHI(i64Ty, 2, "earr_walk_k");
                    llvm::PHINode* offPhi = builder->CreatePHI(i64Ty, 2, "earr_walk_off");
                    kPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), preBB);
                    offPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 4), preBB);
                    builder->CreateCondBr(
                        builder->CreateICmpSLT(kPhi, idx), bodyBB, exitBB);
                    builder->SetInsertPoint(bodyBB);
                    llvm::Value* offAfter;
                    if (elemView) {
                        offAfter = CajetaView::emitElementAdvance(
                            module, elemView, prefixPtr, offPhi);
                    } else {
                        // String[] element: i32 len + len bytes.
                        llvm::Value* sPtr = builder->CreateInBoundsGEP(
                            i8Ty, prefixPtr, offPhi, "earr_slen_ptr");
                        llvm::Value* sLen = builder->CreateIntCast(
                            CajetaView::emitSwapIfNeeded(module, earrE,
                                builder->CreateLoad(i32Ty, sPtr)), i64Ty,
                            /*isSigned=*/true);
                        offAfter = builder->CreateAdd(
                            builder->CreateAdd(offPhi,
                                llvm::ConstantInt::get(i64Ty, 4)),
                            sLen, "earr_walk_after");
                    }
                    llvm::Value* kNext = builder->CreateAdd(
                        kPhi, llvm::ConstantInt::get(i64Ty, 1));
                    llvm::BasicBlock* bodyEndBB = builder->GetInsertBlock();
                    builder->CreateBr(hdrBB);
                    kPhi->addIncoming(kNext, bodyEndBB);
                    offPhi->addIncoming(offAfter, bodyEndBB);
                    builder->SetInsertPoint(exitBB);
                    elemOff = offPhi;
                }
                llvm::Value* elemPtr = builder->CreateInBoundsGEP(
                    i8Ty, prefixPtr, elemOff, "earr_elem");
                if (elemView) {
                    resolvedType = elemView;
                    llvm::AllocaInst* slot = builder->CreateAlloca(
                        llvm::PointerType::get(ctx, 0), nullptr, "earr.slot");
                    builder->CreateStore(elemPtr, slot);
                    return slot;
                }
                llvm::Value* sLen = builder->CreateIntCast(
                    CajetaView::emitSwapIfNeeded(module, earrE,
                        builder->CreateLoad(i32Ty, elemPtr, "earr_str_len32")),
                    i64Ty, /*isSigned=*/true);
                llvm::Value* sData = builder->CreateInBoundsGEP(
                    i8Ty, elemPtr, llvm::ConstantInt::get(i64Ty, 4),
                    "earr_str_data");
                llvm::Function* toOwned = module->getRuntimeFunction(
                    "__cajeta_str_view_to_owned");
                if (!toOwned) return nullptr;
                resolvedType = CajetaType::of("String");
                llvm::Value* cstr = builder->CreateCall(toOwned, {sData, sLen});
                llvm::Value* str = wrapCStringIntoClassString(
                    module, cstr, "earr_str");
                llvm::AllocaInst* sSlot = builder->CreateAlloca(
                    llvm::PointerType::get(ctx, 0), nullptr, "earr.str.slot");
                builder->CreateStore(str, sSlot);
                return sSlot;
            }
        }

        // A class LHS with `operator[]` routes through it. GET only: `arr[i] = v` is
        // BinaryOpExpression's assignment path, which still assumes a native array.
        auto lhsExprForOp = dynamic_pointer_cast<Expression>(children[0]);
        // Generate the receiver EARLY when the pre-pass could not type it, and let both
        // paths below consume it, so the receiver's side effects happen exactly once.
        llvm::Value* lhsPregen = nullptr;
        if (lhsExprForOp) {
            if (!lhsExprForOp->getResolvedType()) {
                lhsExprForOp->resolveTypes(module);
                if (!lhsExprForOp->getResolvedType()) {
                    lhsPregen = children[0]->generateCode(module);
                }
            }
            auto lhsClass = dynamic_pointer_cast<CajetaClass>(
                lhsExprForOp->getResolvedType());
            bool isNativeArrType =
                dynamic_pointer_cast<CajetaArray>(
                    lhsExprForOp->getResolvedType()) != nullptr;
            if (lhsClass && !isNativeArrType && !lhsClass->isInterface()
                    && !(lhsClass->getTypeFlags() & PRIMITIVE_FLAG)) {
                auto idxExprForOp = dynamic_pointer_cast<Expression>(children[1]);
                if (idxExprForOp && !idxExprForOp->getResolvedType()) {
                    idxExprForOp->resolveTypes(module);
                }
                CajetaTypePtr idxType = idxExprForOp
                    ? idxExprForOp->getResolvedType() : nullptr;
                llvm::Value* lhsForOp = lhsPregen
                    ? lhsPregen : children[0]->generateCode(module);
                llvm::Value* idxForOp = children[1]->generateCode(module);
                if (idxForOp && !idxType) {
                    idxType = CajetaType::of(idxForOp);
                }
                if (!idxType) {
                    // Without a usable index type the canonical lookup crashes.
                    goto fall_through_to_native_array;
                }
                // A @ValueType receiver's storage address IS `this` (instance methods take them
                // by reference), so pass the alloca/GEP through and load only an alloca that
                // holds a `ptr` to the aggregate. Every other receiver shape loads normally.
                if (lhsClass->isValueType()) {
                    if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhsForOp)) {
                        if (a->getAllocatedType()->isPointerTy()) {
                            lhsForOp = builder->CreateLoad(
                                a->getAllocatedType(), a);
                        }
                    }
                } else {
                    lhsForOp = loadIfLValue(module, lhsForOp, lhsExprForOp);
                }
                idxForOp = loadIfLValue(module, idxForOp, idxExprForOp);
                vector<ParameterEntry> entries;
                entries.push_back(ParameterEntry(idxType, "", idxForOp));
                std::string opName = "operator[]";
                if (auto m = lhsClass->resolveMethod(opName, entries,
                        /*isConstructor=*/false,
                        /*floatingParams=*/false)) {
                    if (!resolvedType) {
                        resolvedType = m->getReturnType();
                    }
                    llvm::Value* callResult = lhsClass->invokeMethod(
                        opName, entries,
                        /*isConstructor=*/false, lhsForOp,
                        /*callerModule=*/module);
                    if (!callResult) return nullptr;
                    // Wrap the call result in an alloca: consumers expect the native path's `load
                    // from this address` shape, and a load off a raw value is rejected by verify.
                    llvm::AllocaInst* slot = builder->CreateAlloca(
                        callResult->getType(), nullptr, "opidx.slot");
                    builder->CreateStore(callResult, slot);
                    return slot;
                }
                throw Exception(
                    "no matching `operator[](" + idxType->toCanonical()
                        + ")` on `" + lhsClass->toCanonical()
                        + "` for index read",
                    "CAJETA_ERROR_NO_INDEX_OPERATOR");
            }
        }
        fall_through_to_native_array:;

        // The array value is a header pointer `{ i64 size, [0 x T] data }`. A local's
        // alloca, a nested index slot and a class-field GEP all hold a POINTER to it
        // and must be loaded through; a call result already IS the header pointer.
        llvm::Value* arrayVal = lhsPregen
            ? lhsPregen : children[0]->generateCode(module);
        auto lhsExpr = dynamic_pointer_cast<Expression>(children[0]);
        if (!arrayVal) {
            throw Exception(
                "indexed expression's receiver did not lower to a value — "
                "the receiver names an unknown or non-addressable property ("
                + module->getSourcePath() + ":"
                + std::to_string(getSourceLine()) + ")",
                "CAJETA_ERROR_INDEX_RECEIVER_INVALID");
        }

        // A `T[N]` field's slot GEP already addresses the inline storage — there is no
        // header pointer. Loading it, as the heap `T[]` path does, would read the first
        // inline element as a pointer.
        bool inlineArrayAccess = false;
        if (lhsExpr) {
            if (!lhsExpr->getResolvedType()) lhsExpr->resolveTypes(module);
            if (auto la = dynamic_pointer_cast<CajetaArray>(lhsExpr->getResolvedType())) {
                inlineArrayAccess = la->isInlineArray();
            }
        }

        if (inlineArrayAccess) {
            // arrayVal already addresses the inline storage; do not load it.
        } else if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(arrayVal)) {
            arrayVal = builder->CreateLoad(a->getAllocatedType(), a);
        } else if (lhsExpr && dynamic_pointer_cast<ArrayIndexExpression>(lhsExpr)) {
            arrayVal = builder->CreateLoad(
                llvm::PointerType::get(ctx, 0), arrayVal);
        } else if (lhsExpr && dynamic_pointer_cast<DotExpression>(lhsExpr)) {
            if (dynamic_pointer_cast<CajetaArray>(lhsExpr->getResolvedType())) {
                arrayVal = builder->CreateLoad(
                    llvm::PointerType::get(ctx, 0), arrayVal);
            }
        }

        // With opaque pointers the LLVM type says nothing, so the element type comes from
        // children[0]'s CajetaArray. The pre-pass ran before the scope was populated, so
        // re-resolve here and publish our element type for consumers.
        CajetaArrayPtr arrayType;
        if (auto exprChild = dynamic_pointer_cast<Expression>(children[0])) {
            if (!exprChild->getResolvedType()) {
                exprChild->resolveTypes(module);
            }
            arrayType = dynamic_pointer_cast<CajetaArray>(exprChild->getResolvedType());
        }
        if (!arrayType) {
            return nullptr;
        }
        if (!resolvedType) {
            resolvedType = arrayType->getElementType();
        }
        llvm::Type* headerTy = arrayType->getLlvmType();

        // TBAA kind for this array's elements: i8 buffers are reinterpretation-prone and
        // alias-all as Char; every other element type gets the disjoint array tag.
        auto tbaaElemKind = [&]() -> CajetaModule::TbaaKind {
            llvm::Type* et = resolvedType ? resolvedType->getLlvmType() : nullptr;
            if (et && et->isIntegerTy(8)) {
                return CajetaModule::TbaaKind::Char;
            }
            return CajetaModule::TbaaKind::ArrayElem;
        };

        // Load through a field or element index GEP first, or the width cast sees a ptr.
        llvm::Value* idx = children[1]->generateCode(module);
        auto idxAst = dynamic_pointer_cast<Expression>(children[1]);
        idx = loadIfLValue(module, idx, idxAst);
        if (idx->getType() != i64Ty) {
            idx = builder->CreateIntCast(idx, i64Ty, /*isSigned=*/true);
        }

        if (arrayType->isInlineArray()) {
            int64_t n = arrayType->getFixedLength();
            // N is known at compile time, so a constant index proven outside [0, N) is a
            // compile error rather than a runtime trap.
            int64_t civ = 0;
            bool indexIsConst = tryEvalConstIntIndex(children[1], civ);
            if (!indexIsConst) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
                    civ = ci->getSExtValue();
                    indexIsConst = true;
                }
            }
            if (indexIsConst && (civ < 0 || civ >= n)) {
                std::string fieldName;
                if (auto dot = dynamic_pointer_cast<DotExpression>(lhsExpr)) {
                    fieldName = dot->getIdentifier();
                }
                throw Exception(
                    "inline array index " + std::to_string(civ)
                        + " out of bounds for field '" + fieldName
                        + "' of length " + std::to_string(n)
                        + " (valid 0.." + std::to_string(n - 1) + ")",
                    "CAJETA_ERROR_INLINE_ARRAY_INDEX_OUT_OF_BOUNDS");
            }
            BoundsCheck boundsMode = module->getFlags().bounds;
            if (boundsMode != BoundsCheck::Off) {
                llvm::Value* size = llvm::ConstantInt::get(i64Ty, (uint64_t) n);
                llvm::Value* outOfBounds = builder->CreateICmpUGE(idx, size);
                llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                llvm::BasicBlock* failBB = llvm::BasicBlock::Create(ctx, "bounds_fail", parentFn);
                llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx, "bounds_ok", parentFn);
                builder->CreateCondBr(outOfBounds, failBB, okBB);
                builder->SetInsertPoint(failBB);
                if (boundsMode == BoundsCheck::Trap) {
                    llvm::Function* trapFn = llvm::Intrinsic::getOrInsertDeclaration(
                        module->getLlvmModule(), llvm::Intrinsic::trap);
                    builder->CreateCall(trapFn);
                } else {
                    llvm::Function* boundsFail =
                        module->getRuntimeFunction("__cajeta_array_bounds_fail");
                    if (boundsFail) {
                        builder->CreateCall(boundsFail, {idx, size});
                    }
                }
                builder->CreateUnreachable();
                builder->SetInsertPoint(okBB);
            }
            llvm::Type* inlineTy = arrayType->getInlineLlvmType(&ctx);
            vector<llvm::Value*> inlineGep = {
                llvm::ConstantInt::get(i64Ty, 0),
                idx,
            };
            llvm::Value* inlineElemPtr = builder->CreateGEP(inlineTy, arrayVal, inlineGep);
            module->recordTbaaProvenance(inlineElemPtr, tbaaElemKind());
            return inlineElemPtr;
        }

        // `--bounds=off` drops the check; `--bounds=trap` swaps the abort helper for
        // @llvm.trap, so the failure surfaces as SIGILL with no message.
        BoundsCheck boundsMode = module->getFlags().bounds;
        if (boundsMode != BoundsCheck::Off) {
            llvm::Value* sizePtr = builder->CreateStructGEP(headerTy, arrayVal,
                CajetaArray::SIZE_FIELD_INDEX);
            llvm::Value* size = builder->CreateLoad(i64Ty, sizePtr);
            // Mask the shared-state sign bit (slice-spec §3.3) so a shared
            // buffer still bounds-checks against its true count.
            size = builder->CreateAnd(size,
                llvm::ConstantInt::get(i64Ty, 0x7FFFFFFFFFFFFFFFULL));
            llvm::Value* outOfBounds = builder->CreateICmpUGE(idx, size);
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* failBB = llvm::BasicBlock::Create(ctx, "bounds_fail", parentFn);
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx, "bounds_ok", parentFn);
            builder->CreateCondBr(outOfBounds, failBB, okBB);
            builder->SetInsertPoint(failBB);
            if (boundsMode == BoundsCheck::Trap) {
                llvm::Function* trapFn = llvm::Intrinsic::getOrInsertDeclaration(
                    module->getLlvmModule(), llvm::Intrinsic::trap);
                builder->CreateCall(trapFn);
            } else {
                llvm::Function* boundsFail =
                    module->getRuntimeFunction("__cajeta_array_bounds_fail");
                if (boundsFail) {
                    builder->CreateCall(boundsFail, {idx, size});
                }
            }
            builder->CreateUnreachable();
            builder->SetInsertPoint(okBB);
        }

        vector<llvm::Value*> gepIndices = {
            llvm::ConstantInt::get(i64Ty, 0),
            llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
            idx,
        };
        llvm::Value* elemPtr = builder->CreateGEP(headerTy, arrayVal, gepIndices);
        module->recordTbaaProvenance(elemPtr, tbaaElemKind());
        return elemPtr;
    }

    // Helper for prefix/postfix: child is the operand. Returns (addr, value) where addr is
    // non-null iff the operand is an l-value (we may need to store back to it for ++/--).
    static std::pair<llvm::Value*, llvm::Value*> loadOperand(CajetaModulePtr module,
                                                              const AbstractSyntaxNodePtr& child) {
        llvm::Value* raw = child->generateCode(module);
        if (!raw) {
            return {nullptr, nullptr};
        }
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(raw)) {
            return {a, module->getBuilder()->CreateLoad(a->getAllocatedType(), a)};
        }
        auto exprAst = std::dynamic_pointer_cast<Expression>(child);
        if (exprAst) {
            llvm::Value* loaded = loadIfLValue(module, raw, exprAst);
            // The l-value address is `raw` only for an alloca slot; writing back through a
            // field GEP is a DotExpression concern, so those return a null address.
            return {nullptr, loaded};
        }
        return {nullptr, raw};
    }

    // Dispatches a unary operator on a class operand to a user overload: `++`/`--` to a
    // zero-param INSTANCE method, `- + ! ~` to a one-param STATIC method. Returns the
    // call's result, or null to fall through to the primitive path.
    static llvm::Value* tryDispatchUnaryClassOperator(
            CajetaModulePtr module, AbstractSyntaxNodePtr operandAst,
            llvm::Value* operandVal, const char* opSym, bool mutating) {
        if (!operandAst) return nullptr;
        auto opExprAst = std::dynamic_pointer_cast<Expression>(operandAst);
        if (!opExprAst) return nullptr;
        if (!opExprAst->getResolvedType()) opExprAst->resolveTypes(module);
        auto opClass = std::dynamic_pointer_cast<CajetaClass>(
            opExprAst->getResolvedType());
        if (!opClass) return nullptr;
        if (opClass->isInterface()) return nullptr;
        if (opClass->getTypeFlags() & PRIMITIVE_FLAG) return nullptr;

        std::string opName = std::string("operator") + opSym;
        std::vector<ParameterEntry> entries;
        if (mutating) {
            if (!opClass->resolveMethod(opName, entries,
                    /*isConstructor=*/false, /*floatingParams=*/false)) {
                return nullptr;
            }
            return opClass->invokeMethod(opName, entries,
                /*isConstructor=*/false,
                /*thisInstance=*/operandVal,
                /*callerModule=*/module);
        }
        entries.push_back(ParameterEntry(opExprAst->getResolvedType(),
            "", operandVal));
        if (!opClass->resolveMethod(opName, entries,
                /*isConstructor=*/false, /*floatingParams=*/false)) {
            return nullptr;
        }
        return opClass->invokeMethod(opName, entries,
            /*isConstructor=*/false,
            /*thisInstance=*/nullptr,
            /*callerModule=*/module);
    }

    // `!x` resolves to boolean; every other prefix operator keeps the operand's type.
    void PrefixExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) return;
        auto operand = dynamic_pointer_cast<Expression>(children[0]);
        if (!operand) return;
        CajetaTypePtr operandType = operand->getResolvedType();
        if (!operandType) return;
        if (op == PREFIX_OP_LOGNOT) {
            resolvedType = CajetaType::of("boolean");
        } else {
            resolvedType = operandType;
        }
    }

    // Emits the prefix operator, trying a class-operator overload before the primitive
    // lowering. Signed `-`, `++` and `--` route through the checked intrinsics when
    // overflow checking is on.
    llvm::Value* PrefixExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto* builder = module->getBuilder();
        auto [addr, val] = loadOperand(module, children[0]);
        if (!val) return nullptr;
        llvm::Type* ty = val->getType();

        // Class-operator dispatch runs BEFORE the primitive switch, so a class with its
        // own operator wins over the (nonsensical) primitive lowering on a pointer.
        const char* opSym = nullptr;
        bool mutating = false;
        switch (op) {
            case PREFIX_OP_INC:      opSym = "++"; mutating = true;  break;
            case PREFIX_OP_DEC:      opSym = "--"; mutating = true;  break;
            case PREFIX_OP_NEGATIVE: opSym = "-";  mutating = false; break;
            case PREFIX_OP_POSITIVE: opSym = "+";  mutating = false; break;
            case PREFIX_OP_LOGNOT:   opSym = "!";  mutating = false; break;
            case PREFIX_OP_BITNOT:   opSym = "~";  mutating = false; break;
        }
        if (opSym) {
            if (auto* result = tryDispatchUnaryClassOperator(
                    module, children[0], val, opSym, mutating)) {
                return result;
            }
        }

        switch (op) {
            case PREFIX_OP_POSITIVE:
                return val;
            case PREFIX_OP_NEGATIVE: {
                if (ty->isFloatingPointTy()) return builder->CreateFNeg(val);
                // -INT_MIN does not fit: route unary minus through llvm.ssub.with.overflow when
                // the operand's AST type is signed and overflow checking is on.
                bool isSigned = false;
                if (!children.empty()) {
                    if (auto ce = dynamic_pointer_cast<Expression>(children[0])) {
                        auto t = ce->getResolvedType();
                        if (!t) {
                            ce->resolveTypes(module);
                            t = ce->getResolvedType();
                        }
                        if (t) {
                            isSigned = (t->getTypeFlags() & SIGNED_FLAG) != 0;
                        }
                    }
                }
                if (isSigned && ty->isIntegerTy()
                        && module->getFlags().overflowChecks == OverflowChecks::On) {
                    return emitSignedOverflowOp(module, *builder,
                        llvm::Intrinsic::ssub_with_overflow,
                        llvm::ConstantInt::get(ty, 0), val, "ofc.neg");
                }
                return builder->CreateNeg(val);
            }
            case PREFIX_OP_BITNOT:
                return builder->CreateNot(val);
            case PREFIX_OP_LOGNOT: {
                llvm::Value* zero = ty->isFloatingPointTy()
                    ? (llvm::Value*) llvm::ConstantFP::getZero(ty)
                    : (llvm::Value*) llvm::ConstantInt::get(ty, 0);
                return ty->isFloatingPointTy()
                    ? builder->CreateFCmpOEQ(val, zero)
                    : builder->CreateICmpEQ(val, zero);
            }
            case PREFIX_OP_INC:
            case PREFIX_OP_DEC: {
                if (!addr) return val; // can't increment a non-l-value; emit the unchanged value
                llvm::Value* one = ty->isFloatingPointTy()
                    ? (llvm::Value*) llvm::ConstantFP::get(ty, 1.0)
                    : (llvm::Value*) llvm::ConstantInt::get(ty, 1);
                bool isSignedOperand = false;
                if (!children.empty()) {
                    if (auto ce = dynamic_pointer_cast<Expression>(children[0])) {
                        auto t = ce->getResolvedType();
                        if (!t) { ce->resolveTypes(module); t = ce->getResolvedType(); }
                        if (t) isSignedOperand = (t->getTypeFlags() & SIGNED_FLAG) != 0;
                    }
                }
                bool emitOfTrap = !ty->isFloatingPointTy() && ty->isIntegerTy()
                    && isSignedOperand
                    && module->getFlags().overflowChecks == OverflowChecks::On;
                llvm::Value* newVal;
                if (op == PREFIX_OP_INC) {
                    if (ty->isFloatingPointTy()) newVal = builder->CreateFAdd(val, one);
                    else if (emitOfTrap) newVal = emitSignedOverflowOp(module, *builder,
                        llvm::Intrinsic::sadd_with_overflow, val, one, "ofc.preinc");
                    else newVal = builder->CreateAdd(val, one);
                } else {
                    if (ty->isFloatingPointTy()) newVal = builder->CreateFSub(val, one);
                    else if (emitOfTrap) newVal = emitSignedOverflowOp(module, *builder,
                        llvm::Intrinsic::ssub_with_overflow, val, one, "ofc.predec");
                    else newVal = builder->CreateSub(val, one);
                }
                builder->CreateStore(newVal, addr);
                return newVal;
            }
        }
        return nullptr;
    }

    // Defined later in this TU; forward-declared for the capture-cast guards below.
    static std::string erasedBaseOf(const std::string& canonical);

    // True when `type` is a class instantiation whose sole type argument is a bounded
    // EXTENDS wildcard, filling baseCanon, argIndex and boundCanon. Single-argument
    // targets only; defined later in this TU.
    static bool boundedWildcardTarget(const CajetaTypePtr& type,
                                      std::string& baseCanon, int& argIndex,
                                      std::string& boundCanon);

    // Emits a compile-time std::string as a static view-mode cajeta.lang.String and
    // returns its global, so synthesized codegen can build a message without the
    // parser. Falls back to a bare C-string global before class String is registered.
    static llvm::Value* materializeStringConstant(CajetaModulePtr module,
                                                  const std::string& text) {
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        CajetaTypePtr stringTy = CajetaType::of("String");
        auto klass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
        if (!klass || !klass->getLlvmType()
                || !llvm::isa<llvm::StructType>(klass->getLlvmType())) {
            return module->getBuilder()->CreateGlobalString(text, "str");
        }
        auto* structTy = llvm::cast<llvm::StructType>(klass->getLlvmType());
        auto* mod = module->emitTargetLlvmModule();
        auto* i8Ty = llvm::Type::getInt8Ty(ctx);
        auto* i32Ty = llvm::Type::getInt32Ty(ctx);
        auto* i64Ty = llvm::Type::getInt64Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        int64_t len = (int64_t) text.size();
        llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(ptrTy);
        if (auto* vt = klass->getVirtualTableGlobal()) {
            vtableRef = CajetaModule::ensureGlobalInModule(mod, vt);
        }
        // Tagged core, as LiteralExpression builds it: <= 12 B packs the text into the
        // aux/base constant slots (Inline); longer emits the Static pointer form.
        llvm::Constant* lenTagC;
        llvm::Constant* auxC;
        llvm::Constant* baseC;
        if (len <= 12) {
            uint8_t ibuf[12] = {0};
            memcpy(ibuf, text.data(), (size_t) len);
            uint32_t auxBits = 0;
            uint64_t baseBits = 0;
            memcpy(&auxBits, ibuf, 4);
            memcpy(&baseBits, ibuf + 4, 8);
            lenTagC = llvm::ConstantInt::get(i32Ty,
                llvm::APInt(32, (uint64_t) len, true));
            auxC = llvm::ConstantInt::get(i32Ty,
                llvm::APInt(32, (uint64_t) auxBits, false));
            baseC = llvm::ConstantExpr::getIntToPtr(
                llvm::ConstantInt::get(i64Ty,
                    llvm::APInt(64, baseBits, false)),
                ptrTy);
        } else {
            auto* dataInit = llvm::ConstantDataArray::getString(ctx, text, true);
            auto* arrStructTy = llvm::StructType::get(ctx,
                llvm::ArrayRef<llvm::Type*>{i64Ty, dataInit->getType()});
            auto* arrInit = llvm::ConstantStruct::get(arrStructTy,
                llvm::ArrayRef<llvm::Constant*>{
                    llvm::ConstantInt::get(i64Ty,
                        llvm::APInt(64, (uint64_t) len, false)),
                    dataInit});
            auto* bytesGv = new llvm::GlobalVariable(*mod, arrStructTy,
                /*isConst=*/true, llvm::GlobalValue::PrivateLinkage, arrInit,
                ".str.bytes");
            lenTagC = llvm::ConstantInt::get(i32Ty,
                llvm::APInt(32, (uint64_t) (len | ((int64_t) 1 << 29)), true));
            auxC = llvm::ConstantInt::get(i32Ty, 0);
            baseC = bytesGv;
        }
        (void) i8Ty;
        std::vector<llvm::Constant*> instFields = {
            vtableRef, lenTagC, auxC, baseC,
            llvm::ConstantInt::get(i32Ty,
                llvm::APInt(32, (uint64_t) -1, true))};
        for (unsigned fi = (unsigned) instFields.size();
                fi < structTy->getNumElements(); ++fi) {
            instFields.push_back(
                llvm::Constant::getNullValue(structTy->getElementType(fi)));
        }
        auto* instInit = llvm::ConstantStruct::get(structTy,
            llvm::ArrayRef<llvm::Constant*>(instFields));
        auto* instGv = new llvm::GlobalVariable(*mod, structTy,
            /*isConst=*/false, llvm::GlobalValue::PrivateLinkage, instInit,
            ".str.inst");
        return instGv;
    }

    // Branches on `matchBit` and throws ClassCastException("... <msgDetail>") on the
    // false edge, leaving the builder in the match-true block for the caller. Throws
    // through __cajeta_throw, so owned-local unwinding matches a user `throw`.
    static void emitCaptureCastThrowBranch(CajetaModulePtr module,
                                           llvm::Value* matchBit,
                                           const std::string& msgDetail) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB =
            llvm::BasicBlock::Create(ctx, "cast_ok", parentFn);
        llvm::BasicBlock* failBB =
            llvm::BasicBlock::Create(ctx, "cast_fail", parentFn);
        builder->CreateCondBr(matchBit, okBB, failBB);

        builder->SetInsertPoint(failBB);
        bool threw = false;
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find("cajeta.error.ClassCastException");
        if (it != cmap.end()) {
            if (auto cce = std::dynamic_pointer_cast<CajetaClass>(it->second)) {
                llvm::Value* msg = materializeStringConstant(module,
                    "capture cast failed: " + msgDetail);
                std::vector<ParameterEntry> entries;
                entries.push_back(
                    ParameterEntry(CajetaType::of("String"), "", msg));
                llvm::Value* exc = cce->heapConstruct(module, entries);
                if (llvm::Function* throwFn =
                        module->getRuntimeFunction("__cajeta_throw")) {
                    builder->CreateCall(throwFn, {exc});
                    builder->CreateUnreachable();
                    threw = true;
                }
            }
        }
        if (!threw) {
            // ClassCastException is eager (cajeta.error) so this is unreached;
            // terminate the block regardless to keep the IR well-formed.
            builder->CreateUnreachable();
        }

        builder->SetInsertPoint(okBB);
    }

    // Reified guard for a capture downcast `(Foo<A...>) w`: if w's runtime
    // instantiation isn't `targetCanon`, throw cajeta.error.ClassCastException
    // instead of handing back a mis-typed pointer (reified-capture-spec.md §3).
    static void emitCaptureCastGuard(CajetaModulePtr module, llvm::Value* objPtr,
                                     const std::string& targetCanon) {
        auto* builder = module->getBuilder();
        llvm::Function* checkFn =
            module->getRuntimeFunction("__cajeta_instanceof_named");
        if (!checkFn) return;  // no runtime check — fall through (legacy cast)
        llvm::Value* namePtr =
            builder->CreateGlobalString(targetCanon, "cast.target");
        llvm::Value* r =
            builder->CreateCall(checkFn, {objPtr, namePtr}, "cast.match");
        llvm::Value* matchBit = builder->CreateICmpNE(
            r, llvm::ConstantInt::get(r->getType(), 0));
        emitCaptureCastThrowBranch(module, matchBit,
            "value is not a " + targetCanon);
    }

    // Reified guard for `(Base<? extends Bound>) w`: throws ClassCastException unless w
    // is a `baseCanon<...>` whose reified type argument at `argIndex` conforms to
    // `boundCanon`.
    static void emitBoundedCaptureCastGuard(CajetaModulePtr module,
                                            llvm::Value* objPtr,
                                            const std::string& baseCanon,
                                            int argIndex,
                                            const std::string& boundCanon) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Function* checkFn =
            module->getRuntimeFunction("__cajeta_instanceof_bounded");
        if (!checkFn) return;  // no runtime check — fall through
        llvm::Value* baseStr =
            builder->CreateGlobalString(baseCanon, "cast.base");
        llvm::Value* boundStr =
            builder->CreateGlobalString(boundCanon, "cast.bound");
        llvm::Value* idx =
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), argIndex);
        llvm::Value* r = builder->CreateCall(
            checkFn, {objPtr, baseStr, idx, boundStr}, "cast.bmatch");
        llvm::Value* matchBit = builder->CreateICmpNE(
            r, llvm::ConstantInt::get(r->getType(), 0));
        emitCaptureCastThrowBranch(module, matchBit,
            "element of " + baseCanon + " is not a " + boundCanon);
    }

    // Emits the cast: an interface source unwraps the fat body, a record upcast slices
    // the base prefix, a reified downcast is guarded, and primitives convert by the
    // SOURCE's signedness. A primitive crossing a `?` is a bit reinterpretation.
    llvm::Value* CastExpression::generateCode(CajetaModulePtr module) {
        if (children.empty() || !destType) return nullptr;
        auto* builder = module->getBuilder();
        // loadOperand unwraps only allocas, so a field GEP would be ptrtoint'd instead of
        // loaded; loadIfLValue loads through it at the ast's resolved element type.
        llvm::Value* raw = children[0]->generateCode(module);
        if (!raw) return nullptr;
        auto childAst = dynamic_pointer_cast<Expression>(children[0]);
        llvm::Value* val = loadIfLValue(module, raw, childAst);
        if (!val) return nullptr;
        // An interface value points at the 24-byte fat body `{ data, vtable, kind }`, and
        // slot 0 is the class instance. Class references are stored as `ptr`, so the
        // loaded data pointer is what every class-typed value site expects.
        if (childAst) {
            if (!childAst->getResolvedType()) {
                childAst->resolveTypes(module);
            }
            CajetaTypePtr srcCajetaType = childAst->getResolvedType();
            auto srcClass = dynamic_pointer_cast<CajetaClass>(srcCajetaType);
            auto dstClass = dynamic_pointer_cast<CajetaClass>(destType);
            // A record upcast SLICES: the flat layout embeds ancestors first, so `(Base) d`
            // copies the base-field prefix into a fresh Base value. No other pair casts.
            if (srcClass && dstClass
                    && srcClass->isRecordType() && dstClass->isRecordType()
                    && srcClass.get() != dstClass.get()) {
                bool dstIsAncestor = false;
                std::function<void(const CajetaClassPtr&)> walkSup =
                    [&](const CajetaClassPtr& c) {
                        for (auto& sup : c->getSuperClasses()) {
                            if (!sup || dstIsAncestor) continue;
                            if (sup.get() == dstClass.get()) {
                                dstIsAncestor = true;
                                return;
                            }
                            walkSup(sup);
                        }
                    };
                walkSup(srcClass);
                if (!dstIsAncestor) {
                    throw Exception(
                        "cannot cast record '" + srcClass->toCanonical()
                            + "' to '" + dstClass->toCanonical()
                            + "' — records cast only to their own ancestors "
                              "(an upcast slices to the base fields)",
                        "CAJETA_ERROR_RECORD_CAST");
                }
                llvm::Type* dstBody = dstClass->getLlvmType();
                llvm::Value* srcAddr = raw;
                if (!srcAddr->getType()->isPointerTy()) {
                    llvm::Value* tmp = builder->CreateAlloca(srcAddr->getType());
                    builder->CreateStore(srcAddr, tmp);
                    srcAddr = tmp;
                }
                const llvm::DataLayout& dl =
                    module->getLlvmModule()->getDataLayout();
                llvm::Value* slice = builder->CreateAlloca(
                    dstBody, nullptr, "slice");
                llvm::Align align(dl.getABITypeAlign(dstBody));
                builder->CreateMemCpy(slice, align, srcAddr, align,
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(*module->getLlvmContext()),
                        dl.getTypeAllocSize(dstBody)));
                resolvedType = destType;
                return slice;
            }
            bool srcIsIface = srcClass && srcClass->isInterface();
            bool dstIsIface = dstClass && dstClass->isInterface();
            if (srcIsIface && dstClass && !dstIsIface) {
                llvm::Type* bodyTy = srcClass->getLlvmType();
                llvm::Type* ptrTy = llvm::PointerType::get(
                    *module->getLlvmContext(), 0);
                llvm::Value* dataSlot = builder->CreateStructGEP(
                    bodyTy, val, 0, "iface_data_slot");
                return builder->CreateLoad(ptrTy, dataSlot, "iface_to_class");
            }
        }
        llvm::Type* srcTy = val->getType();
        llvm::Type* dstTy = destType->getLlvmType();
        // Class-typed destinations store as `ptr` though getLlvmType() is the body struct,
        // so a `ptr` source needs no cast — the fallback bitcast would be rejected.
        if (srcTy->isPointerTy()) {
            auto dstClass = dynamic_pointer_cast<CajetaClass>(destType);
            bool dstIsArr =
                dynamic_pointer_cast<CajetaArray>(destType) != nullptr;
            bool dstIsIface = dstClass && dstClass->isInterface();
            bool dstIsPrim =
                destType && (destType->getTypeFlags() & PRIMITIVE_FLAG);
            bool dstStoresAsPointer = dstClass
                && (dstIsArr || !dstIsPrim)
                && !dstIsIface;
            if (dstStoresAsPointer) {
                // A wildcard source, or a different instantiation of the same base, against a
                // concrete destination is a genuine reified downcast. Plain upcasts and
                // same-type casts do not qualify and stay untouched.
                CajetaTypePtr srcType = childAst ? childAst->getResolvedType()
                                                 : nullptr;
                std::string dstCanon = destType->toCanonical();
                std::string srcCanon = srcType ? srcType->toCanonical()
                                               : std::string();
                bool srcIsWildcard = srcCanon.find('?') != std::string::npos;
                bool dstConcrete = dstCanon.find('?') == std::string::npos;
                bool sameBase = srcType
                    && erasedBaseOf(srcCanon) == erasedBaseOf(dstCanon);
                bool captureDowncast = dstClass && dstConcrete && srcType
                    && srcCanon != dstCanon && (srcIsWildcard || sameBase);
                std::string boundBaseCanon, boundCanon;
                int boundArgIdx = -1;
                bool boundedDowncast = srcType
                    && boundedWildcardTarget(destType, boundBaseCanon,
                                             boundArgIdx, boundCanon)
                    && srcCanon != dstCanon && (srcIsWildcard || sameBase);
                if (boundedDowncast && val->getType()->isPointerTy()) {
                    emitBoundedCaptureCastGuard(module, val, boundBaseCanon,
                                                boundArgIdx, boundCanon);
                } else if (captureDowncast && val->getType()->isPointerTy()) {
                    emitCaptureCastGuard(module, val, dstCanon);
                }
                return val;
            }
        }
        if (srcTy == dstTy) return val;

        bool srcInt = srcTy->isIntegerTy();
        bool dstInt = dstTy->isIntegerTy();
        bool srcFp  = srcTy->isFloatingPointTy();
        bool dstFp  = dstTy->isFloatingPointTy();
        bool srcPtr = srcTy->isPointerTy();
        bool dstPtr = dstTy->isPointerTy();
        unsigned long destFlags = destType->getTypeFlags();
        bool destSigned = (destFlags & SIGNED_FLAG) != 0;
        // Integer widening and int->fp follow the SOURCE operand's signedness (a `uint8`
        // zero-extends); truncation ignores it, and fp->int keys off the destination.
        bool srcSigned = destSigned;
        if (childAst && childAst->getResolvedType()) {
            srcSigned = (childAst->getResolvedType()->getTypeFlags()
                         & SIGNED_FLAG) != 0;
        }

        if (srcInt && dstInt) {
            return builder->CreateIntCast(val, dstTy, srcSigned);
        }
        if (srcFp && dstFp) {
            return builder->CreateFPCast(val, dstTy);
        }
        if (srcInt && dstFp) {
            return srcSigned ? builder->CreateSIToFP(val, dstTy)
                             : builder->CreateUIToFP(val, dstTy);
        }
        if (srcFp && dstInt) {
            return destSigned ? builder->CreateFPToSI(val, dstTy)
                              : builder->CreateFPToUI(val, dstTy);
        }
        if (srcPtr && dstPtr) {
            return builder->CreateBitCast(val, dstTy);
        }
        if (srcPtr && dstInt) {
            return builder->CreatePtrToInt(val, dstTy);
        }
        if (srcInt && dstPtr) {
            return builder->CreateIntToPtr(val, dstTy);
        }
        // A `?` slot holds a primitive's BITS in one pointer-sized word, so this crossing
        // is a reinterpretation. LLVM rejects a bitcast with a pointer on exactly one
        // side, so route through the integer domain, sized to the POINTER word.
        if ((srcFp && dstPtr) || (srcPtr && dstFp)) {
            const llvm::DataLayout& dl =
                module->getLlvmModule()->getDataLayout();
            llvm::Type* word = dl.getIntPtrType(dstTy->getContext(), 0);
            if (srcFp) {
                llvm::Value* bits = builder->CreateBitCast(
                    val, llvm::Type::getIntNTy(
                             dstTy->getContext(),
                             srcTy->getPrimitiveSizeInBits()), "fp.bits");
                return builder->CreateIntToPtr(
                    builder->CreateZExtOrTrunc(bits, word), dstTy, "fp.slot");
            }
            llvm::Value* bits = builder->CreatePtrToInt(val, word, "slot.bits");
            bits = builder->CreateZExtOrTrunc(
                bits, llvm::Type::getIntNTy(dstTy->getContext(),
                                            dstTy->getPrimitiveSizeInBits()));
            return builder->CreateBitCast(bits, dstTy, "slot.fp");
        }
        return builder->CreateBitCast(val, dstTy);
    }

    // Emits `x++` / `x--`, dispatching to a class `operator++` first. The primitive path
    // stores the updated value back through the operand's alloca and yields the
    // ORIGINAL value.
    llvm::Value* PostfixExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto* builder = module->getBuilder();
        auto [addr, val] = loadOperand(module, children[0]);
        if (!val) return nullptr;
        // Try class dispatch BEFORE the alloca-only address the primitive path needs: on
        // a class the operator mutates `this`, so prefix and postfix collapse.
        const char* opSym = (op == POSTFIX_OP_INC) ? "++"
                          : (op == POSTFIX_OP_DEC) ? "--"
                          : nullptr;
        if (opSym) {
            if (auto* result = tryDispatchUnaryClassOperator(
                    module, children[0], val, opSym, /*mutating=*/true)) {
                return result;
            }
        }
        if (!addr) return val;
        llvm::Type* ty = val->getType();
        llvm::Value* one = ty->isFloatingPointTy()
            ? (llvm::Value*) llvm::ConstantFP::get(ty, 1.0)
            : (llvm::Value*) llvm::ConstantInt::get(ty, 1);
        bool isSignedOperand = false;
        if (!children.empty()) {
            if (auto ce = dynamic_pointer_cast<Expression>(children[0])) {
                auto t = ce->getResolvedType();
                if (!t) { ce->resolveTypes(module); t = ce->getResolvedType(); }
                if (t) isSignedOperand = (t->getTypeFlags() & SIGNED_FLAG) != 0;
            }
        }
        bool emitOfTrap = !ty->isFloatingPointTy() && ty->isIntegerTy()
            && isSignedOperand
            && module->getFlags().overflowChecks == OverflowChecks::On;
        llvm::Value* newVal;
        if (op == POSTFIX_OP_INC) {
            if (ty->isFloatingPointTy()) newVal = builder->CreateFAdd(val, one);
            else if (emitOfTrap) newVal = emitSignedOverflowOp(module, *builder,
                llvm::Intrinsic::sadd_with_overflow, val, one, "ofc.postinc");
            else newVal = builder->CreateAdd(val, one);
        } else {
            if (ty->isFloatingPointTy()) newVal = builder->CreateFSub(val, one);
            else if (emitOfTrap) newVal = emitSignedOverflowOp(module, *builder,
                llvm::Intrinsic::ssub_with_overflow, val, one, "ofc.postdec");
            else newVal = builder->CreateSub(val, one);
        }
        builder->CreateStore(newVal, addr);
        return val;
    }


    // Builds the node for a `primary` context: aggregate initializer, bracket literal,
    // `T.class`, `this`/`super` (with an optional `<Base>` ancestor), a literal, an
    // identifier, or a parenthesized expression.
    ExpressionPtr PrimaryExpression::fromContext(CajetaParser::PrimaryContext* ctx) {
        ExpressionPtr result = nullptr;
        if (ctx->LPAREN()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->literal()) {
            result = LiteralExpression::fromContext(ctx->literal());
        } else if (ctx->aggregateInitializer()) {
            // Matched before the identifier alternative so the longer form wins; a bare `Foo`
            // still takes the identifier branch, since this one requires the `{ ... }`.
            result = make_shared<AggregateInitializerExpression>(
                ctx->aggregateInitializer(), ctx->getStart());
        } else if (ctx->arrayLiteral()) {
            result = arrayOrMapLiteralFromContext(
                ctx->arrayLiteral(), ctx->getStart(), /*stack=*/false, /*shared=*/false);
        } else if (ctx->typeTypeOrVoid() && ctx->CLASS()) {
            // Capture the type's text now — the ANTLR context is freed before codegen; the
            // class itself is resolved by name at resolveTypes time.
            result = make_shared<ClassLiteralExpression>(
                ctx->typeTypeOrVoid()->getText(), ctx->getStart());
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), true);
        } else if (ctx->THIS()) {
            // Pass the Token directly: `ctx->expression()` is null for the THIS form, so the
            // expression-taking ThisExpression overload would null-deref on getStart().
            auto thisExpr = make_shared<ThisExpression>(ctx->getStart());
            if (ctx->typeType()) {
                thisExpr->setChosenAncestorName(ctx->typeType()->getText());
            }
            result = thisExpr;
        } else if (ctx->SUPER()) {
            auto superExpr = make_shared<SuperExpression>(ctx->getStart());
            if (ctx->typeType()) {
                superExpr->setChosenAncestorName(ctx->typeType()->getText());
            }
            result = superExpr;
        }
        return result;
    }

    // Never emits: fromContext always yields a concrete expression node, so a
    // PrimaryExpression reaching codegen produces no value.
    llvm::Value* PrimaryExpression::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    // Resolves `T.class`. A type parameter binds through the module's substitution
    // stack, which is live only during an instantiation walk — hence resolved and
    // cached eagerly here; otherwise the name is looked up in the canonical map.
    void ClassLiteralExpression::resolveTypes(CajetaModulePtr module) {
        if (resolvedType) return;
        auto& cmap = CajetaType::getCanonicalMap();
        if (module) {
            if (auto bound = module->lookupTypeParameter(namedTypeName)) {
                namedType = bound;
            }
        }
        if (!namedType) {
            auto nit = cmap.find(namedTypeName);
            if (nit != cmap.end()) {
                namedType = nit->second;
            }
        }
        if (!namedType) return;   // unresolved type — generateCode reports it
        auto it = cmap.find("cajeta.reflect.Class");
        auto classTmpl = (it != cmap.end())
            ? dynamic_pointer_cast<CajetaClass>(it->second) : nullptr;
        if (classTmpl && classTmpl->isTemplate()) {
            resolvedType = classTmpl->instantiate({namedType});
        }
    }

    // Returns the address of the class's cached #ClassObject, a process-lifetime
    // constant, so the result is a borrow that is never freed. Throws when the type is
    // a primitive or its reflection metadata was not emitted.
    llvm::Value* ClassLiteralExpression::generateCode(CajetaModulePtr module) {
        if (!resolvedType) resolveTypes(module);
        auto klass = dynamic_pointer_cast<CajetaClass>(namedType);
        if (!klass) {
            throw Exception(
                "`.class` requires a class type — a primitive has no runtime "
                "Class object; use a reference type before `.class`",
                "CAJETA_ERROR_CLASS_LITERAL");
        }
        llvm::GlobalVariable* co = klass->getClassObjectGlobal();
        if (!co) {
            throw Exception(
                "no #ClassObject for '" + klass->toCanonical()
                + "' — its reflection metadata was not emitted",
                "CAJETA_ERROR_CLASS_LITERAL");
        }
        return CajetaModule::ensureGlobalInModule(module->emitTargetLlvmModule(), co);
    }

    // Types the ternary from the `then` arm; there is no least-upper-bound across arms
    // yet, so codegen coerces the `else` arm to match.
    void BooleanSwitchExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.size() >= 2) {
            if (auto thenExpr = dynamic_pointer_cast<Expression>(children[1])) {
                resolvedType = thenExpr->getResolvedType();
            }
        }
    }

    // The title flag one ternary arm hands the merge (see the member's note
    // in Expression.h). Emitted in the arm's own block, right after the arm's
    // value, so a call arm's return-flag TLS is still that call's.
    static llvm::Value* armTitleFlag(CajetaModulePtr module, const ExpressionPtr& arm) {
        if (!arm) return module->getBuilder()->getInt64(0);
        ownership::TitleShape s = ownership::classify(arm, module);
        llvm::Value* f = ownership::titleFlag(s, module);
        return f ? f : (llvm::Value*) module->getBuilder()->getInt64(0);
    }

    // Emits the ternary as two arm blocks and a phi, coercing the `else` arm's width to
    // the `then` arm's, and merges the arms' title flags alongside the value.
    llvm::Value* BooleanSwitchExpression::generateCode(CajetaModulePtr module) {
        // Stale-Value guard (as MoveExpression): this node re-generates per
        // instantiation; a flag from a previous function must never leak.
        runtimeTitleFlag = nullptr;
        if (children.size() < 3) return nullptr;
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        // The flag is computed for any pointer-valued result: only the class/String local
        // paths consume it, and resolvedType is not reliably a class by codegen time.
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);

        // Use loadIfLValue, not the alloca-only load: a condition that is a boolean FIELD
        // or an element is a GEP, and the i1 coercion would build an ICmp on a pointer.
        auto condAst = dynamic_pointer_cast<Expression>(children[0]);
        llvm::Value* cond = loadIfLValue(module, children[0]->generateCode(module), condAst);
        llvm::Type* i1Ty = llvm::Type::getInt1Ty(ctx);
        if (cond->getType() != i1Ty) {
            llvm::Value* zero = llvm::ConstantInt::get(cond->getType(), 0);
            cond = builder->CreateICmpNE(cond, zero);
        }

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(ctx, "ternary_then", parentFn);
        llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(ctx, "ternary_else", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx, "ternary_merge", parentFn);

        builder->CreateCondBr(cond, thenBB, elseBB);

        builder->SetInsertPoint(thenBB);
        auto thenAst = dynamic_pointer_cast<Expression>(children[1]);
        llvm::Value* thenVal = loadIfLValue(module, children[1]->generateCode(module), thenAst);
        bool classLike = thenVal && thenVal->getType()->isPointerTy();
        llvm::Value* thenFlag = classLike
            ? armTitleFlag(module, thenAst) : nullptr;
        llvm::BasicBlock* thenEnd = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(elseBB);
        auto elseAst = dynamic_pointer_cast<Expression>(children[2]);
        llvm::Value* elseVal = loadIfLValue(module, children[2]->generateCode(module), elseAst);
        llvm::Value* elseFlag = classLike
            ? armTitleFlag(module, elseAst) : nullptr;
        if (elseVal->getType() != thenVal->getType()) {
            llvm::Type* tt = thenVal->getType();
            llvm::Type* et = elseVal->getType();
            if (tt->isIntegerTy() && et->isIntegerTy()) {
                elseVal = builder->CreateIntCast(elseVal, tt, /*isSigned=*/true);
            } else if (tt->isFloatingPointTy() && et->isFloatingPointTy()) {
                elseVal = builder->CreateFPCast(elseVal, tt);
            } else if (tt->isFloatingPointTy() && et->isIntegerTy()) {
                elseVal = builder->CreateSIToFP(elseVal, tt);
            } else if (tt->isIntegerTy() && et->isFloatingPointTy()) {
                elseVal = builder->CreateFPToSI(elseVal, tt);
            }
        }
        llvm::BasicBlock* elseEnd = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(thenVal->getType(), 2);
        phi->addIncoming(thenVal, thenEnd);
        phi->addIncoming(elseVal, elseEnd);
        if (classLike && thenFlag && elseFlag) {
            auto* tc = llvm::dyn_cast<llvm::ConstantInt>(thenFlag);
            auto* ec = llvm::dyn_cast<llvm::ConstantInt>(elseFlag);
            if (tc && ec && tc->getZExtValue() == ec->getZExtValue()) {
                runtimeTitleFlag = thenFlag;     // both arms decide statically
            } else {
                llvm::PHINode* fphi = builder->CreatePHI(i64, 2, "tern_title");
                fphi->addIncoming(thenFlag, thenEnd);
                fphi->addIncoming(elseFlag, elseEnd);
                runtimeTitleFlag = fphi;
            }
        }
        return phi;
    }

    // `#x` carries x's type: the sharp is an ownership operation, not a coercion.
    void MoveExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (!children.empty()) {
            if (auto inner = dynamic_pointer_cast<Expression>(children[0])) {
                resolvedType = inner->getResolvedType();
            }
        }
    }

    // Emits `#expr`: a take protocol chosen by the source's kind (element, member,
    // static, identifier, call result), then ONE classification taken after codegen,
    // leaving runtimeTitleFlag holding the mode the consuming store or return records.
    llvm::Value* MoveExpression::generateCode(CajetaModulePtr module) {
        // Stale-Value guard: this node re-generates per instantiation, and a
        // flag captured in a previous function must never leak into this one.
        runtimeTitleFlag = nullptr;
        if (children.empty()) return nullptr;
        // The legacy `dst = #v` spelling warns and still transfers. Reported here because
        // this is the one node both spellings do not share; call args and returns are
        // deliberately silent.
        if (redundantSharp) {
            if (DiagnosticEngine* eng = DiagnosticEngine::active()) {
                eng->report("warning", "CAJETA_WARN_REDUNDANT_TRANSFER",
                    "`#= #x` spells the transfer twice — `#=` already carries "
                    "the source's mode (a title when it has one, a borrow "
                    "otherwise), so the second `#` adds nothing. Write "
                    "`dst #= x`.",
                    module->getSourcePath(), (int) getSourceLine(), -1);
            }
        }
        if (legacyTransferAssign) {
            if (DiagnosticEngine* eng = DiagnosticEngine::active()) {
                std::string rhs;
                if (auto srcId = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                    rhs = srcId->getTextValue();
                }
                eng->report("warning", "CAJETA_WARN_DEPRECATED_TRANSFER_ASSIGN",
                    "`= #" + rhs + "` is the deprecated spelling of an "
                    "ownership store; write `dst #= " + rhs + "` instead. "
                    "`#` stays required at call arguments, returns, and "
                    "extraction reads — those are not assignments",
                    module->getSourcePath(), (int) getSourceLine(), -1);
            }
        }
        // Evaluate the wrapped expression BEFORE marking the source moved, or the
        // use-after-move check trips on the very read that performs the transfer.
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        // A nested Move (the `#=` desugar wrapping a parsed `#expr`) delegates entirely:
        // generate the inner Move and propagate its flag outward.
        if (inner && inner->kind() == ExprKind::Move) {
            auto nestedMv = std::static_pointer_cast<MoveExpression>(inner);
            llvm::Value* nv = nestedMv->generateCode(module);
            runtimeTitleFlag = nestedMv->getRuntimeTitleFlag();
            return nv;
        }
        // `#map[k]` on a class receiver binds the author's `operator#[]` — the
        // title-extracting index, a distinct canonical name because dispatch is
        // mode-erased. It is TOTAL: a title out, or a panic. The receiver is not moved.
        if (inner && inner->kind() == ExprKind::ArrayIndex) {
            auto idxInner = std::static_pointer_cast<ArrayIndexExpression>(inner);
            auto& ich = idxInner->getChildren();
            auto idxRecv = ich.size() >= 1
                ? dynamic_pointer_cast<Expression>(ich[0]) : nullptr;
            auto idxArg = ich.size() >= 2
                ? dynamic_pointer_cast<Expression>(ich[1]) : nullptr;
            if (idxRecv && !idxRecv->getResolvedType()) {
                idxRecv->resolveTypes(module);
            }
            auto recvClass = idxRecv
                ? dynamic_pointer_cast<CajetaClass>(idxRecv->getResolvedType())
                : nullptr;
            bool recvIsArray = recvClass
                && dynamic_pointer_cast<CajetaArray>(idxRecv->getResolvedType());
            if (recvClass && !recvIsArray && !recvClass->isInterface()
                    && idxArg) {
                auto* builder = module->getBuilder();
                llvm::Value* recvVal = idxRecv->generateCode(module);
                recvVal = loadIfLValue(module, recvVal, idxRecv);
                llvm::Value* idxVal = idxArg->generateCode(module);
                idxVal = loadIfLValue(module, idxVal, idxArg);
                if (!idxArg->getResolvedType()) idxArg->resolveTypes(module);
                std::vector<ParameterEntry> entries;
                entries.push_back(ParameterEntry(
                    idxArg->getResolvedType(), "", idxVal));
                std::string opName = "operator#[]";
                MethodPtr op;
                try {
                    op = recvClass->resolveMethod(opName, entries,
                        /*isConstructor=*/false, /*floatingParams=*/false);
                } catch (...) {
                    op = nullptr;
                }
                if (!op) {
                    throw Exception(
                        "type `" + recvClass->toCanonical() + "` has no "
                        "`operator#[]` — `#" "container[key]` extraction "
                        "needs the title-extracting index operator. Plain "
                        "reads use `container[key]`; declare `#V operator#[]"
                        "(K key)` on the container to support extraction.",
                        "CAJETA_ERROR_NO_TITLE_INDEX_OPERATOR");
                }
                llvm::Value* out = recvClass->invokeMethod(opName, entries,
                    /*isConstructor=*/false, recvVal, /*callerModule=*/module);
                resolvedType = op->getReturnType();
                return out;
            }
        }
        // `#h.f` on a bit-carrying field: a set bit moves the title and the bit decays to
        // borrowed (the field stays resident and readable), a clear bit panics. Returns
        // the loaded r-value and deliberately does NOT demote the path to a borrow.
        if (inner && inner->kind() == ExprKind::Dot) {
            auto dotInner = std::static_pointer_cast<DotExpression>(inner);
            if (!dotInner->getResolvedType()) dotInner->resolveTypes(module);
            auto& xch = dotInner->getChildren();
            auto xRecv = xch.empty() ? nullptr
                : dynamic_pointer_cast<Expression>(xch[0]);
            if (xRecv && !xRecv->getResolvedType()) xRecv->resolveTypes(module);
            auto xRecvClass = xRecv
                ? dynamic_pointer_cast<CajetaClass>(xRecv->getResolvedType())
                : nullptr;
            StructurePropertyPtr xProp;
            CajetaClassPtr xDecl;
            if (xRecvClass) {
                std::function<bool(const CajetaClassPtr&)> xFind =
                    [&](const CajetaClassPtr& cls) -> bool {
                        if (!cls) return false;
                        auto pit = cls->getProperties().find(
                            dotInner->getIdentifier());
                        if (pit != cls->getProperties().end()) {
                            xProp = pit->second;
                            xDecl = cls;
                            return true;
                        }
                        for (auto& sup : cls->getSuperClasses()) {
                            if (xFind(sup)) return true;
                        }
                        return false;
                    };
                xFind(xRecvClass);
            }
            // A `#x.f` move of a PRIMITIVE member is identity: load the value so the
            // enclosing store writes bytes, not the member slot's address.
            if (xProp && xDecl && !CajetaClass::fieldHasOwnershipBit(xProp)
                    && xProp->getType()
                    && !dynamic_pointer_cast<CajetaClass>(xProp->getType())) {
                llvm::Value* pv = dotInner->generateCode(module);
                pv = loadIfLValue(module, pv, dotInner);
                resolvedType = xProp->getType();
                return pv;
            }
            // A String MEMBER of a value-struct slot carries no ownership bit, but the member
            // walk drops resident wrappers unconditionally — so the move must TAKE: load the
            // wrapper and NULL the member, or the old table frees what the new one stored.
            if (xProp && xDecl && xRecvClass && xRecvClass->isValueType()
                    && !CajetaClass::fieldHasOwnershipBit(xProp)) {
                auto xPropCls = dynamic_pointer_cast<CajetaClass>(
                    xProp->getType());
                if (xPropCls && xPropCls->getQName()
                        && xPropCls->getQName()->getTypeName() == "String"
                        && xPropCls->getQName()->getPackageName()
                               == "cajeta.lang") {
                    llvm::Value* mslot = dotInner->generateCode(module);
                    if (mslot && mslot->getType()->isPointerTy()) {
                        auto* sb = module->getBuilder();
                        llvm::PointerType* sptr = llvm::PointerType::get(
                            *module->getLlvmContext(), 0);
                        llvm::Value* sv = sb->CreateLoad(
                            sptr, mslot, "mstr.take.val");
                        sb->CreateStore(
                            llvm::ConstantPointerNull::get(sptr), mslot);
                        resolvedType = xProp->getType();
                        return sv;
                    }
                }
            }
            if (xProp && xDecl && CajetaClass::fieldHasOwnershipBit(xProp)) {
                llvm::Value* slot = dotInner->generateCode(module);
                if (slot && slot->getType()->isPointerTy()) {
                    int fieldIdx = xDecl->getFieldLlvmIndex(xProp);
                    int wordIdx = xDecl->getOwnershipWordLlvmIndex();
                    auto* declTy = llvm::dyn_cast_or_null<llvm::StructType>(
                        xDecl->getLlvmType());
                    if (fieldIdx >= 0 && wordIdx >= 0 && declTy
                            && !declTy->isOpaque()) {
                        auto* b = module->getBuilder();
                        auto& xctx = *module->getLlvmContext();
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(xctx);
                        llvm::Type* i64Ty = llvm::Type::getInt64Ty(xctx);
                        llvm::PointerType* ptrTy =
                            llvm::PointerType::get(xctx, 0);
                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        const llvm::StructLayout* sl =
                            dl.getStructLayout(declTy);
                        int64_t delta =
                            (int64_t) sl->getElementOffset((unsigned) wordIdx)
                            - (int64_t) sl->getElementOffset(
                                  (unsigned) fieldIdx);
                        llvm::Value* wordPtr = b->CreateInBoundsGEP(
                            i8Ty, slot,
                            llvm::ConstantInt::get(i64Ty, delta),
                            "xtract_bits_addr");
                        uint64_t bitIdx =
                            (uint64_t) xDecl->ownershipBitIndexOf(xProp);
                        llvm::Value* w = b->CreateLoad(
                            i64Ty, wordPtr, "xtract_bits");
                        llvm::Value* bit = b->CreateAnd(
                            b->CreateLShr(w,
                                llvm::ConstantInt::get(i64Ty, bitIdx)),
                            llvm::ConstantInt::get(i64Ty, 1));
                        // The fused claim (`V out #= #slots[i].val`) forwards the bit VERBATIM, so a
                        // borrow forwards as a borrow with no panic. Bare `= #x.f` keeps the guard.
                        if (isForwardingSlotMove()) {
                            runtimeTitleFlag = bit;
                            llvm::Value* fwdW = b->CreateAnd(w,
                                llvm::ConstantInt::get(
                                    i64Ty, ~(1ULL << bitIdx)));
                            b->CreateStore(fwdW, wordPtr);
                            return b->CreateLoad(ptrTy, slot, "xtract_fwd");
                        }
                        llvm::Value* owned = b->CreateICmpNE(
                            bit, llvm::ConstantInt::get(i64Ty, 0));
                        llvm::Function* fn =
                            b->GetInsertBlock()->getParent();
                        llvm::BasicBlock* panicBB =
                            llvm::BasicBlock::Create(xctx, "xtract_panic", fn);
                        llvm::BasicBlock* okBB =
                            llvm::BasicBlock::Create(xctx, "xtract_ok", fn);
                        b->CreateCondBr(owned, okBB, panicBB);
                        b->SetInsertPoint(panicBB);
                        // CAJETA_PANIC_TITLE_MISS = 3, integer-throw shape
                        // (< 4096 ⇒ first catch clause binds it).
                        if (llvm::Function* throwFn =
                                module->getRuntimeFunction("__cajeta_throw")) {
                            llvm::Value* code = b->CreateIntToPtr(
                                llvm::ConstantInt::get(i64Ty, 3), ptrTy);
                            b->CreateCall(throwFn, {code});
                        }
                        b->CreateUnreachable();
                        b->SetInsertPoint(okBB);
                        llvm::Value* newW = b->CreateAnd(w,
                            llvm::ConstantInt::get(i64Ty, ~(1ULL << bitIdx)));
                        b->CreateStore(newW, wordPtr);
                        return b->CreateLoad(ptrTy, slot, "xtract_title");
                    }
                }
            }
        }
        llvm::Value* value = inner ? inner->generateCode(module) : nullptr;
        ownership::TitleShape mvShape = inner
            ? ownership::moveFrom(ownership::classify(inner, module), isSharpStore())
            : ownership::TitleShape();
        // A conditional inner (`dst #= c ? a : b`) hands its per-arm title flag through,
        // so the store records the TAKEN arm's mode.
        if (isConditionalKind(inner)) {
            runtimeTitleFlag = ownership::titleFlag(mvShape, module);
        }
        // A plain (non-`#`) return is not statically a borrow — a plain-return wrapper
        // that tail-calls a `#` method rides the inner flag through. Read the TLS HERE,
        // right after the call, while it still holds this call's bit; later is stale.
        llvm::Value* innerCallReturnFlag = nullptr;
        if (inner && inner->kind() == ExprKind::MethodCall) {
            if (mvShape.source == ownership::TitleSource::ReturnFlag) {
                innerCallReturnFlag = ownership::titleFlag(mvShape, module);
            } else if (mvShape.source == ownership::TitleSource::None
                       && mvShape.answer == ownership::TitleAnswer::Borrow) {
                innerCallReturnFlag = module->getBuilder()->getInt64(0);
            }
            if (!innerCallReturnFlag && !mvShape.callee
                    && mvShape.source == ownership::TitleSource::ReturnFlag) {
                auto mceIn = std::static_pointer_cast<MethodCallExpression>(inner);
                if (!mceIn->bindingTakesTitle()) {
                    innerCallReturnFlag = module->getBuilder()->getInt64(0);
                }
            }
        }
        // Load through a variable SLOT only. A producer (interface repack, `stack Foo()`,
        // aggregate init, record upcast, slice) hands back an alloca that IS the value,
        // so gate on the AST shape as loadIfLValue's `treatAllocaAsSlot` does.
        if (value) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(value)) {
                bool allocaIsSlot = !inner || inner->kind() == ExprKind::Identifier;
                if (allocaIsSlot) {
                    value = module->getBuilder()->CreateLoad(
                        a->getAllocatedType(), a);
                }
            } else if (llvm::isa<llvm::GlobalVariable>(value)
                    && (inner->kind() == ExprKind::Identifier
                        || inner->kind() == ExprKind::Dot)) {
                // A static field's slot is a GlobalVariable, never an alloca, so the load above
                // never fires for it and the store would receive the slot ADDRESS.
                value = loadIfLValue(module, value, inner);
            }
        }
        // `#arr[i]` takes the element OUT: the runtime take hands back the value, NULLs
        // the slot (the element walk then skips it) and decays its owned bit, so the
        // slot stays resident and readable while the title rides to the new owner.
        bool slotTaken = false;
        if (inner && inner->kind() == ExprKind::ArrayIndex) {
            auto aixInner = std::static_pointer_cast<ArrayIndexExpression>(inner);
            if (value && value->getType()->isPointerTy()
                    && !aixInner->getChildren().empty()) {
                if (auto idBase = dynamic_pointer_cast<IdentifierExpression>(
                        aixInner->getChildren()[0])) {
                    if (auto scope = module->getScopeStack().peek()) {
                        if (FieldPtr baseField = scope->getField(
                                idBase->getTextValue())) {
                            if (llvm::Value* sidecar =
                                    baseField->getElemOwnSidecar()) {
                                if (!aixInner->getResolvedType()) {
                                    aixInner->resolveTypes(module);
                                }
                                if (CajetaClass::arrayElementCarriesSlotBits(
                                        aixInner->getResolvedType())) {
                                    llvm::Function* takeFn =
                                        module->getRuntimeFunction(
                                            "__cajeta_class_array_elem_take");
                                    if (takeFn) {
                                        auto* b = module->getBuilder();
                                        auto& tctx = *module->getLlvmContext();
                                        llvm::Type* ti64 =
                                            llvm::Type::getInt64Ty(tctx);
                                        llvm::PointerType* tptr =
                                            llvm::PointerType::get(tctx, 0);
                                        llvm::Value* taken = b->CreateCall(
                                            takeFn, {sidecar, value},
                                            "slot.take");
                                        llvm::Value* miss = b->CreateICmpEQ(
                                            taken,
                                            llvm::ConstantPointerNull::get(tptr),
                                            "slot.take.miss");
                                        // A MODE-CARRYING claim tolerates a miss — the slot held a borrow, which is legal
                                        // — so it selects rather than branches. Only a demanding `#R`/`#T` claim panics.
                                        if (isForwardingSlotMove()) {
                                            value = b->CreateSelect(
                                                miss, value, taken, "slot.fwd");
                                            slotTaken = true;
                                        } else {
                                        llvm::Function* fn =
                                            b->GetInsertBlock()->getParent();
                                        llvm::BasicBlock* panicBB =
                                            llvm::BasicBlock::Create(
                                                tctx, "slot_take_panic", fn);
                                        llvm::BasicBlock* okBB =
                                            llvm::BasicBlock::Create(
                                                tctx, "slot_take_ok", fn);
                                        b->CreateCondBr(miss, panicBB, okBB);
                                        b->SetInsertPoint(panicBB);
                                        if (llvm::Function* throwFn =
                                                module->getRuntimeFunction(
                                                    "__cajeta_throw")) {
                                            llvm::Value* code = b->CreateIntToPtr(
                                                llvm::ConstantInt::get(ti64, 3),
                                                tptr);
                                            b->CreateCall(throwFn, {code});
                                        }
                                        b->CreateUnreachable();
                                        b->SetInsertPoint(okBB);
                                        value = taken;
                                        slotTaken = true;
                                        }
                                    }
                                } else if (llvm::Function* takeFn =
                                        module->getRuntimeFunction(
                                            "__cajeta_string_array_elem_take")) {
                                    value = module->getBuilder()->CreateCall(
                                        takeFn, {sidecar, value}, "elem.take");
                                    slotTaken = true;
                                }
                            }
                        }
                    }
                }
                if (!slotTaken) {
                    if (!aixInner->getResolvedType()) {
                        aixInner->resolveTypes(module);
                    }
                    // Elements without slot bits (primitives, Strings, value types) have no tail take,
                    // but the Move must still hand back a LOADED element: returning the slot GEP
                    // stores the slot's ADDRESS instead of its contents.
                    auto mvElemT = aixInner->getResolvedType();
                    {
                        auto mvElemCls = dynamic_pointer_cast<CajetaClass>(
                            mvElemT);
                        if (mvElemCls && mvElemCls->getQName()
                                && mvElemCls->getQName()->getTypeName()
                                       == "String"
                                && mvElemCls->getQName()->getPackageName()
                                       == "cajeta.lang") {
                            auto* sb = module->getBuilder();
                            auto& sctx = *module->getLlvmContext();
                            llvm::PointerType* sptr =
                                llvm::PointerType::get(sctx, 0);
                            llvm::Value* sv = sb->CreateLoad(
                                sptr, value, "str.take.val");
                            sb->CreateStore(
                                llvm::ConstantPointerNull::get(sptr), value);
                            resolvedType = mvElemT;
                            return sv;
                        }
                    }
                    if (mvElemT
                            && !CajetaClass::arrayElementCarriesSlotBits(
                                   mvElemT)
                            && !CajetaClass::arrayElementCarriesArraySlotBits(
                                   mvElemT)) {
                        value = loadIfLValue(module, value, aixInner);
                        resolvedType = mvElemT;
                        return value;
                    }
                    auto recvT = dynamic_pointer_cast<Expression>(
                        aixInner->getChildren()[0]);
                    bool recvSimple = recvT
                        && (dynamic_pointer_cast<IdentifierExpression>(recvT)
                            || dynamic_pointer_cast<DotExpression>(recvT));
                    if (recvSimple
                            && (CajetaClass::arrayElementCarriesSlotBits(
                                    aixInner->getResolvedType())
                                || CajetaClass::arrayElementCarriesArraySlotBits(
                                    aixInner->getResolvedType()))) {
                        auto* b = module->getBuilder();
                        auto& tctx = *module->getLlvmContext();
                        llvm::Type* ti64 = llvm::Type::getInt64Ty(tctx);
                        llvm::PointerType* tptr =
                            llvm::PointerType::get(tctx, 0);
                        llvm::Value* rv = recvT->generateCode(module);
                        llvm::Value* hdr = loadIfLValue(module, rv, recvT);
                        auto recvArr = dynamic_pointer_cast<CajetaArray>(
                            recvT->getResolvedType());
                        const llvm::DataLayout& tdl =
                            module->getLlvmModule()->getDataLayout();
                        uint64_t ths = 8, tes = 8;
                        if (recvArr) {
                            ths = tdl.getTypeAllocSize(recvArr->getLlvmType());
                            tes = recvArr->elementStrideBytes(tdl, &tctx);
                        }
                        llvm::Value* slotInt = b->CreatePtrToInt(value, ti64);
                        llvm::Value* hdrInt = b->CreatePtrToInt(hdr, ti64);
                        llvm::Value* tIdx = b->CreateSDiv(
                            b->CreateSub(b->CreateSub(slotInt, hdrInt),
                                llvm::ConstantInt::get(ti64, ths)),
                            llvm::ConstantInt::get(ti64, tes));
                        llvm::Value* elem = b->CreateLoad(tptr, value,
                            "slot.take.val");
                        llvm::Function* takeFn = module->getRuntimeFunction(
                            "__cajeta_tail_elem_take_flag");
                        if (takeFn && isForwardingSlotMove()) {
                            runtimeTitleFlag = b->CreateCall(takeFn,
                                {hdr, llvm::ConstantInt::get(ti64, ths),
                                 llvm::ConstantInt::get(ti64, tes), tIdx},
                                "slot.fwd.flag");
                            value = elem;
                        } else if (takeFn) {
                            llvm::Value* was = b->CreateCall(takeFn,
                                {hdr, llvm::ConstantInt::get(ti64, ths),
                                 llvm::ConstantInt::get(ti64, tes), tIdx},
                                "slot.take.flag");
                            llvm::Value* miss = b->CreateICmpEQ(was,
                                llvm::ConstantInt::get(ti64, 0),
                                "slot.take.miss");
                            llvm::Function* fn =
                                b->GetInsertBlock()->getParent();
                            llvm::BasicBlock* panicBB =
                                llvm::BasicBlock::Create(
                                    tctx, "tail_take_panic", fn);
                            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(
                                tctx, "tail_take_ok", fn);
                            b->CreateCondBr(miss, panicBB, okBB);
                            b->SetInsertPoint(panicBB);
                            if (llvm::Function* throwFn =
                                    module->getRuntimeFunction(
                                        "__cajeta_throw")) {
                                llvm::Value* code = b->CreateIntToPtr(
                                    llvm::ConstantInt::get(ti64, 3), tptr);
                                b->CreateCall(throwFn, {code});
                            }
                            b->CreateUnreachable();
                            b->SetInsertPoint(okBB);
                            value = elem;
                        }
                    }
                }
            }
        }
        if (inner && inner->kind() == ExprKind::Identifier) {
            auto idExpr = std::static_pointer_cast<IdentifierExpression>(inner);
            auto scope = module->getScopeStack().peek();
            if (scope) {
                // `#x` demands a statically active owner: a borrow-shaped class local owns no
                // title, and moving out of it would mint a second active owner.
                const string& mvName = idExpr->getTextValue();
                // A `#=` records the source's mode rather than claiming a title, so the
                // rejection — which exists for `#v`, an ASSERTION that the source holds a
                // title — never applies to it. `isSharpStore()` is the reliable test: every
                // `#=` sets it, in the assignment form and the declaration form alike,
                // whereas `modeCarrying` is absent from the node that
                // HeapField::getOrCreateAllocation re-generates for a declaration's
                // initializer (LocalVariableDeclaration -> putField -> that path).
                scope->rejectTransferOfBorrow(mvName,
                                              isModeCarrying() || isSharpStore());
                // A `#=` CONSUMES its source only when the source statically holds a title: a
                // borrowed one has none to hand over, so a single lent local may feed several
                // slots. Gated on isSharpStore(), since consumption is a fact about the SOURCE.
                if (!isSharpStore() || scope->holdsStaticTitle(mvName)) {
                    scope->demoteToBorrow(mvName,
                        "moved by `#" + mvName + "` at line "
                            + std::to_string(getSourceLine()));
                }
                if (FieldPtr field = scope->getField(idExpr->getTextValue())) {
                    if (llvm::Value* entry = field->getDropEntry()) {
                        // The entry's flag IS the title's truth here — 1 for a static owner, the
                        // call- or caller-supplied bit for a runtime one. Capture it BEFORE deactivating,
                        // so the consuming store or return forwards what was actually held.
                        runtimeTitleFlag = ownership::titleFlag(mvShape, module);
                        ownership::deactivateLocalEntry(module, field);
                    } else if (auto pfMv =
                            dynamic_pointer_cast<ParameterField>(field)) {
                        // An entry-less plain formal still has runtime truth in the enclosing function's
                        // transfer word; a `#`-declared formal keeps the static 1, its contract.
                        auto fpMv = pfMv->getFormalParameter();
                        auto cmMv = module->getCurrentMethod();
                        llvm::Value* inWordMv =
                            cmMv ? cmMv->getTransferWordArg() : nullptr;
                        if (fpMv && !fpMv->isTransferred() && inWordMv) {
                            int fposMv = -1, seenMv = -1;
                            for (auto& fp : cmMv->getParameterList()) {
                                if (!fp || fp->getName() == "this") continue;
                                ++seenMv;
                                if (fp->getName() == idExpr->getTextValue()) {
                                    fposMv = seenMv;
                                    break;
                                }
                            }
                            if (fposMv >= 0 && fposMv < 64) {
                                runtimeTitleFlag = ownership::titleFlag(mvShape, module);   // (word >> index) & 1
                            }
                        }
                    }
                }
                // Every consumer reads a null flag as owned, which is right for a `#x` CLAIM and
                // wrong for `#=`. A borrow alias reaches neither branch above, so the flag must
                // be set explicitly or the slot silently claims the object.
                if (isSharpStore() && !runtimeTitleFlag
                        && mvShape.answer == ownership::TitleAnswer::Borrow) {
                    runtimeTitleFlag = ownership::titleFlag(mvShape, module);
                }
                maybeEmitSessionDisarm(module, mvName);
            }
        } else if (inner && inner->kind() == ExprKind::Dot) {
            // Path-based move (`#a.b.c`): record the path so later reads through it, or any
            // prefix of it, are rejected. A bit-capable class FIELD is exempt (its runtime
            // bit governs), decided by the field's TYPE so codegen passes agree.
            auto scope = module->getScopeStack().peek();
            if (scope) {
                bool bitCapableClassField = false;
                {
                    auto dotInner2 = std::static_pointer_cast<DotExpression>(inner);
                    if (!dotInner2->getResolvedType()) {
                        dotInner2->resolveTypes(module);
                    }
                    auto fc = dynamic_pointer_cast<CajetaClass>(
                        dotInner2->getResolvedType());
                    bool isStr = fc && fc->getQName()
                        && fc->getQName()->getTypeName() == "String"
                        && fc->getQName()->getPackageName() == "cajeta.lang";
                    bitCapableClassField = fc && !isStr
                        && !fc->isInterface() && !fc->isValueType();
                }
                if (!bitCapableClassField) {
                    string path = DotExpression::buildPath(inner);
                    if (!path.empty()) scope->demotePathToBorrow(path);
                }
            }
        }
        // A `^` (view) result has no title to take, so both spellings are refused — `#=`
        // deliberately, to keep it meaning `a title moved here`.
        if (mvShape.leaf && mvShape.leaf->kind() == ExprKind::MethodCall
                && mvShape.has(ownership::TitleShape::kView)) {
            auto mceInner = std::static_pointer_cast<MethodCallExpression>(mvShape.leaf);
            if (MethodPtr vm = mceInner->getResolvedMethod()) {
                if (vm->isReturnsView()) {
                    throw Exception(
                        "`" + vm->toCanonical(false) + "` is declared `^` — it "
                        "returns a VIEW interior to its receiver, which still "
                        "owns and frees it — and this binding claims it with "
                        "`#`. A view has no title to take: "
                          "the claim would arm a drop on memory the receiver "
                          "frees, and the second free is the caller's. Fix: "
                          "bind it with a plain `=` — a `^` result is a "
                          "borrow, and the receiver must outlive it. See "
                          "specs/stdlib-ownership-convention-spec.md §4.7.",
                        "CAJETA_ERROR_TRANSFER_OF_VIEW_RESULT");
                }
            }
        }
        if (isSharpStore() && !runtimeTitleFlag && inner
                && inner->kind() == ExprKind::MethodCall) {
            runtimeTitleFlag = innerCallReturnFlag
                ? innerCallReturnFlag : ownership::titleFlag(mvShape, module);
        }
        return value;
    }

    // Types the switch expression from the first non-default case body; there is no
    // least-upper-bound across arms, so a caller needing one shape casts at the use.
    void SwitchExpression::resolveTypes(CajetaModulePtr module) {
        if (discriminator) discriminator->resolveTypes(module);
        for (auto& c : cases) {
            for (auto& lab : c.labels) {
                if (lab) lab->resolveTypes(module);
            }
            if (c.body) c.body->resolveTypes(module);
        }
        for (auto& c : cases) {
            if (!c.labels.empty() && c.body) {
                resolvedType = c.body->getResolvedType();
                if (resolvedType) break;
            }
        }
        if (!resolvedType) {
            for (auto& c : cases) {
                if (c.body && c.body->getResolvedType()) {
                    resolvedType = c.body->getResolvedType();
                    break;
                }
            }
        }
    }

    // Lowers the arrow-form switch to an LLVM switch plus a phi over the arm values,
    // widening integer arms to a common width. A missing default gets a synthetic
    // unreachable block, so the phi needs no entry for it.
    llvm::Value* SwitchExpression::generateCode(CajetaModulePtr module) {
        runtimeTitleFlag = nullptr;   // stale-value guard, as the conditional's
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        if (!discriminator) {
            throw Exception("switch expression missing discriminator",
                "CAJETA_ERROR_NOT_IMPLEMENTED");
        }
        llvm::Value* disc = discriminator->generateCode(module);
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(disc)) {
            disc = builder->CreateLoad(a->getAllocatedType(), disc);
        }
        if (!disc->getType()->isIntegerTy()) {
            throw Exception("switch expression discriminator must be integer-typed",
                "CAJETA_ERROR_NOT_IMPLEMENTED");
        }
        llvm::Type* discTy = disc->getType();

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx, "sw_expr_merge", parentFn);
        llvm::BasicBlock* defaultBB = nullptr;

        struct LoweredCase { llvm::BasicBlock* bb; ExpressionPtr body; vector<llvm::ConstantInt*> labels; };
        vector<LoweredCase> nonDefault;
        ExpressionPtr defaultBody;
        for (auto& c : cases) {
            if (c.labels.empty()) {
                if (!defaultBB) {
                    defaultBB = llvm::BasicBlock::Create(ctx, "sw_default", parentFn);
                    defaultBody = c.body;
                }
                continue;
            }
            LoweredCase lc;
            lc.bb = llvm::BasicBlock::Create(ctx, "sw_case", parentFn);
            lc.body = c.body;
            for (auto& lab : c.labels) {
                llvm::Value* v = lab ? lab->generateCode(module) : nullptr;
                if (auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v)) {
                    if (ci->getType() != discTy) {
                        // Sign-extend or truncate the label to the discriminator's width.
                        ci = llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(discTy),
                            ci->getSExtValue(), /*isSigned=*/true);
                    }
                    lc.labels.push_back(ci);
                } else {
                    throw Exception("switch case label must be a constant integer",
                        "CAJETA_ERROR_NOT_IMPLEMENTED");
                }
            }
            nonDefault.push_back(std::move(lc));
        }
        if (!defaultBB) {
            defaultBB = llvm::BasicBlock::Create(ctx, "sw_default_unreachable", parentFn);
        }

        llvm::SwitchInst* sw = builder->CreateSwitch(disc, defaultBB,
            (unsigned) nonDefault.size());
        for (auto& lc : nonDefault) {
            for (auto* l : lc.labels) sw->addCase(l, lc.bb);
        }

        vector<pair<llvm::Value*, llvm::BasicBlock*>> incoming;
        vector<pair<llvm::Value*, llvm::BasicBlock*>> titleIncoming;
        llvm::Type* phiTy = nullptr;

        auto emitArm = [&](llvm::BasicBlock* bb, const ExpressionPtr& body) {
            builder->SetInsertPoint(bb);
            if (!body) {
                builder->CreateUnreachable();
                return;
            }
            llvm::Value* v = body->generateCode(module);
            // An arm's value must be an r-value, or the phi merges a slot ADDRESS with an
            // object pointer.
            v = loadIfLValue(module, v, body);
            if (v && v->getType()->isPointerTy()) {
                llvm::Value* tf = armTitleFlag(module, body);
                titleIncoming.push_back({tf, builder->GetInsertBlock()});
            }
            if (v) {
                if (!phiTy) phiTy = v->getType();
                else if (phiTy != v->getType() && phiTy->isIntegerTy() && v->getType()->isIntegerTy()) {
                    if (phiTy->getScalarSizeInBits() < v->getType()->getScalarSizeInBits()) {
                        phiTy = v->getType();
                    }
                }
                incoming.push_back({v, builder->GetInsertBlock()});
            }
            builder->CreateBr(mergeBB);
        };

        for (auto& lc : nonDefault) emitArm(lc.bb, lc.body);
        if (defaultBody) {
            emitArm(defaultBB, defaultBody);
        } else {
            builder->SetInsertPoint(defaultBB);
            builder->CreateUnreachable();
        }

        builder->SetInsertPoint(mergeBB);
        if (incoming.empty() || !phiTy) {
            builder->CreateUnreachable();
            return nullptr;
        }
        llvm::PHINode* phi = builder->CreatePHI(phiTy, (unsigned) incoming.size());
        for (auto& [v, bb] : incoming) {
            llvm::Value* widened = v;
            if (v->getType() != phiTy && v->getType()->isIntegerTy() && phiTy->isIntegerTy()) {
                // Need to insert the cast in the arm's predecessor block, before its branch.
                llvm::IRBuilder<> tmp(bb->getTerminator());
                widened = tmp.CreateIntCast(v, phiTy, /*isSigned=*/true);
            }
            phi->addIncoming(widened, bb);
        }
        if (!titleIncoming.empty() && titleIncoming.size() == incoming.size()) {
            auto* c0 = llvm::dyn_cast<llvm::ConstantInt>(titleIncoming[0].first);
            bool allSame = c0 != nullptr;
            for (auto& [f, bb] : titleIncoming) {
                auto* c = llvm::dyn_cast<llvm::ConstantInt>(f);
                if (!c || c->getZExtValue() != c0->getZExtValue()) { allSame = false; break; }
            }
            if (allSame) {
                runtimeTitleFlag = titleIncoming[0].first;
            } else {
                llvm::PHINode* tphi = builder->CreatePHI(llvm::Type::getInt64Ty(ctx),
                    (unsigned) titleIncoming.size(), "sw_title");
                for (auto& [f, bb] : titleIncoming) tphi->addIncoming(f, bb);
                runtimeTitleFlag = tphi;
            }
        }
        return phi;
    }

    // Always throws CAJETA_ERROR_NOT_IMPLEMENTED, naming the construct and its source
    // position: the parser accepts these shapes so the error lands at codegen.
    llvm::Value* UnsupportedExpression::generateCode(CajetaModulePtr module) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%s is not yet implemented (source line %d, column %d)",
            constructName.c_str(), sourceLine, sourceColumn);
        throw Exception(string(buf), "CAJETA_ERROR_NOT_IMPLEMENTED");
    }

    // --- LambdaExpression -------------------------------------------------

    static thread_local int64_t lambdaCounter = 0;  // per-thread (threadsafe U4)

    // Collects the names a lambda body references that its parameter list does not bind.
    // Statement subclasses and MethodCallExpression keep sub-nodes outside `children`,
    // so each needs its own descent; over-collecting is filtered by the caller.
    static void collectFreeIdentifiers(
            const AbstractSyntaxNodePtr& node,
            const std::set<std::string>& bound,
            std::set<std::string>& seen,
            std::vector<std::string>& out) {
        if (!node) return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node)) {
            const std::string& name = id->getTextValue();
            if (!name.empty()
                    && bound.find(name) == bound.end()
                    && seen.find(name) == seen.end()) {
                seen.insert(name);
                out.push_back(name);
            }
            return;
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
            for (auto& c : mc->getChildren()) {
                collectFreeIdentifiers(c, bound, seen, out);
            }
            for (auto& p : mc->getParameters()) {
                collectFreeIdentifiers(p.expression, bound, seen, out);
            }
            return;
        }
        if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
            for (auto& vd : lvd->getVariableDeclarators()) {
                if (vd && vd->getInitializer()) {
                    collectFreeIdentifiers(vd->getInitializer(), bound, seen, out);
                }
            }
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            collectFreeIdentifiers(ret->getExpression(), bound, seen, out);
            return;
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
            collectFreeIdentifiers(ifs->getCondition(), bound, seen, out);
            collectFreeIdentifiers(ifs->getThenBranch(), bound, seen, out);
            collectFreeIdentifiers(ifs->getElseBranch(), bound, seen, out);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            collectFreeIdentifiers(es->getExpression(), bound, seen, out);
            return;
        }
        // Statement subclasses whose inner blocks live outside `children` need explicit
        // handlers, or free identifiers nested inside them are never captured.
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
            collectFreeIdentifiers(ls->getBlock(), bound, seen, out);
            return;
        }
        if (auto ss = std::dynamic_pointer_cast<ScopeStatement>(node)) {
            collectFreeIdentifiers(ss->getBlock(), bound, seen, out);
            return;
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
            collectFreeIdentifiers(ws->getCondition(), bound, seen, out);
            collectFreeIdentifiers(ws->getBody(), bound, seen, out);
            return;
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(node)) {
            collectFreeIdentifiers(ds->getBody(), bound, seen, out);
            collectFreeIdentifiers(ds->getCondition(), bound, seen, out);
            return;
        }
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
            collectFreeIdentifiers(fs->getInit(), bound, seen, out);
            collectFreeIdentifiers(fs->getCondition(), bound, seen, out);
            for (auto& u : fs->getUpdate()) {
                collectFreeIdentifiers(u, bound, seen, out);
            }
            collectFreeIdentifiers(fs->getBody(), bound, seen, out);
            return;
        }
        if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            collectFreeIdentifiers(efs->getIterableExpr(), bound, seen, out);
            collectFreeIdentifiers(efs->getBody(), bound, seen, out);
            return;
        }
        for (auto& c : node->getChildren()) {
            collectFreeIdentifiers(c, bound, seen, out);
        }
    }

    // Walks to the leftmost (receiver-side) identifier of an expression. `#c.next()`
    // parses as `#(c.next())` — REFERENCE binds looser than `.` — so the transferred
    // name sits one level deeper than the move's immediate child.
    static std::string firstIdentifierIn(const AbstractSyntaxNodePtr& node) {
        if (!node) return "";
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node)) {
            return id->getTextValue();
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
            if (!mc->getChildren().empty()) {
                return firstIdentifierIn(mc->getChildren()[0]);
            }
            return "";
        }
        for (auto& c : node->getChildren()) {
            auto r = firstIdentifierIn(c);
            if (!r.empty()) return r;
        }
        return "";
    }

    // Collects the outer names a lambda body transfers in with `#name` (Lambdas.md rule
    // 3). Same statement-shape recursion as collectFreeIdentifiers; the transferred
    // name is the move subtree's receiver-side leaf.
    static void collectTransferNames(
            const AbstractSyntaxNodePtr& node,
            std::set<std::string>& out) {
        if (!node) return;
        if (auto mv = std::dynamic_pointer_cast<MoveExpression>(node)) {
            if (!mv->getChildren().empty()) {
                std::string name = firstIdentifierIn(mv->getChildren()[0]);
                if (!name.empty()) out.insert(name);
            }
            for (auto& c : mv->getChildren()) {
                collectTransferNames(c, out);
            }
            return;
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
            for (auto& c : mc->getChildren()) collectTransferNames(c, out);
            for (auto& p : mc->getParameters()) collectTransferNames(p.expression, out);
            return;
        }
        if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
            for (auto& vd : lvd->getVariableDeclarators()) {
                if (vd && vd->getInitializer()) collectTransferNames(vd->getInitializer(), out);
            }
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            collectTransferNames(ret->getExpression(), out);
            return;
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
            collectTransferNames(ifs->getCondition(), out);
            collectTransferNames(ifs->getThenBranch(), out);
            collectTransferNames(ifs->getElseBranch(), out);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            collectTransferNames(es->getExpression(), out);
            return;
        }
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
            collectTransferNames(ls->getBlock(), out);
            return;
        }
        if (auto ss = std::dynamic_pointer_cast<ScopeStatement>(node)) {
            collectTransferNames(ss->getBlock(), out);
            return;
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
            collectTransferNames(ws->getCondition(), out);
            collectTransferNames(ws->getBody(), out);
            return;
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(node)) {
            collectTransferNames(ds->getBody(), out);
            collectTransferNames(ds->getCondition(), out);
            return;
        }
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
            collectTransferNames(fs->getInit(), out);
            collectTransferNames(fs->getCondition(), out);
            for (auto& u : fs->getUpdate()) collectTransferNames(u, out);
            collectTransferNames(fs->getBody(), out);
            return;
        }
        if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            collectTransferNames(efs->getIterableExpr(), out);
            collectTransferNames(efs->getBody(), out);
            return;
        }
        for (auto& c : node->getChildren()) {
            collectTransferNames(c, out);
        }
    }

    // Rejects a write to a by-value captured primitive (Lambdas.md rule 5) — the lambda
    // would be mutating a private copy. Walks the body for assignment-form
    // BinaryOpExpressions whose LHS names a value capture, throwing on the first.
    static void enforceValueCaptureImmutability(
            const AbstractSyntaxNodePtr& node,
            const std::set<std::string>& valueCapturedNames) {
        if (!node || valueCapturedNames.empty()) return;
        if (auto bop = std::dynamic_pointer_cast<BinaryOpExpression>(node)) {
            if (bop->isAssignment() && !bop->getChildren().empty()) {
                if (auto lhsId = std::dynamic_pointer_cast<IdentifierExpression>(
                        bop->getChildren()[0])) {
                    const std::string& name = lhsId->getTextValue();
                    if (valueCapturedNames.find(name) != valueCapturedNames.end()) {
                        throw Exception(
                            "cannot assign to '" + name + "' inside lambda — "
                            "primitives are captured by value (the lambda "
                            "holds a private copy); use a mutable wrapper "
                            "if you need shared mutability",
                            "CAJETA_ERROR_TYPE");
                    }
                }
            }
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
            for (auto& c : mc->getChildren()) {
                enforceValueCaptureImmutability(c, valueCapturedNames);
            }
            for (auto& p : mc->getParameters()) {
                enforceValueCaptureImmutability(p.expression, valueCapturedNames);
            }
            return;
        }
        if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
            for (auto& vd : lvd->getVariableDeclarators()) {
                if (vd && vd->getInitializer()) {
                    enforceValueCaptureImmutability(vd->getInitializer(), valueCapturedNames);
                }
            }
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            enforceValueCaptureImmutability(ret->getExpression(), valueCapturedNames);
            return;
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
            enforceValueCaptureImmutability(ifs->getCondition(), valueCapturedNames);
            enforceValueCaptureImmutability(ifs->getThenBranch(), valueCapturedNames);
            enforceValueCaptureImmutability(ifs->getElseBranch(), valueCapturedNames);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatement>(node)) {
            enforceValueCaptureImmutability(es->getExpression(), valueCapturedNames);
            return;
        }
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
            enforceValueCaptureImmutability(ls->getBlock(), valueCapturedNames);
            return;
        }
        if (auto ss = std::dynamic_pointer_cast<ScopeStatement>(node)) {
            enforceValueCaptureImmutability(ss->getBlock(), valueCapturedNames);
            return;
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
            enforceValueCaptureImmutability(ws->getCondition(), valueCapturedNames);
            enforceValueCaptureImmutability(ws->getBody(), valueCapturedNames);
            return;
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(node)) {
            enforceValueCaptureImmutability(ds->getBody(), valueCapturedNames);
            enforceValueCaptureImmutability(ds->getCondition(), valueCapturedNames);
            return;
        }
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
            enforceValueCaptureImmutability(fs->getInit(), valueCapturedNames);
            enforceValueCaptureImmutability(fs->getCondition(), valueCapturedNames);
            for (auto& u : fs->getUpdate()) {
                enforceValueCaptureImmutability(u, valueCapturedNames);
            }
            enforceValueCaptureImmutability(fs->getBody(), valueCapturedNames);
            return;
        }
        if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            enforceValueCaptureImmutability(efs->getIterableExpr(), valueCapturedNames);
            enforceValueCaptureImmutability(efs->getBody(), valueCapturedNames);
            return;
        }
        for (auto& c : node->getChildren()) {
            enforceValueCaptureImmutability(c, valueCapturedNames);
        }
    }

    // Yields the type of the FIRST explicit return in a lambda's block body; `return;`
    // is void and no return at all is null. Nested lambdas are skipped, since their
    // returns belong to another function.
    static CajetaTypePtr inferLambdaReturnFromBody(
            const AbstractSyntaxNodePtr& node) {
        if (!node) return nullptr;
        if (std::dynamic_pointer_cast<LambdaExpression>(node)) {
            return nullptr;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            auto expr = ret->getExpression();
            if (!expr) return CajetaType::of("void");
            return expr->getResolvedType();
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) {
            if (auto t = inferLambdaReturnFromBody(ifs->getThenBranch())) return t;
            if (auto t = inferLambdaReturnFromBody(ifs->getElseBranch())) return t;
            return nullptr;
        }
        if (auto ls = std::dynamic_pointer_cast<LabelStatement>(node)) {
            return inferLambdaReturnFromBody(ls->getBlock());
        }
        if (auto ss = std::dynamic_pointer_cast<ScopeStatement>(node)) {
            return inferLambdaReturnFromBody(ss->getBlock());
        }
        if (auto ws = std::dynamic_pointer_cast<WhileStatement>(node)) {
            return inferLambdaReturnFromBody(ws->getBody());
        }
        if (auto ds = std::dynamic_pointer_cast<DoStatement>(node)) {
            return inferLambdaReturnFromBody(ds->getBody());
        }
        if (auto fs = std::dynamic_pointer_cast<ForStatement>(node)) {
            return inferLambdaReturnFromBody(fs->getBody());
        }
        if (auto efs = std::dynamic_pointer_cast<EnhancedForStatement>(node)) {
            return inferLambdaReturnFromBody(efs->getBody());
        }
        for (auto& c : node->getChildren()) {
            if (auto t = inferLambdaReturnFromBody(c)) {
                return t;
            }
        }
        return nullptr;
    }

    // Collects the name and declared type of the body's TOP-LEVEL local declarations, so
    // resolveTypes can pre-populate the lambda's scope and return-type inference finds
    // a type. Block-scoped locals are deliberately excluded.
    static void collectTopLevelBodyLocals(
            const AbstractSyntaxNodePtr& body,
            std::vector<std::pair<std::string, CajetaTypePtr>>& out) {
        if (!body) return;
        for (auto& child : body->getChildren()) {
            auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(child);
            if (!lvd) continue;
            CajetaTypePtr declaredType = lvd->getType();
            if (!declaredType) continue;
            for (auto& vd : lvd->getVariableDeclarators()) {
                out.emplace_back(vd->getIdentifier(), declaredType);
            }
        }
    }

    // Resolves the body under a temporary scope holding the declared parameters and the
    // top-level body locals, then pins the CajetaFunctionType — expectedType first,
    // else the body's type or first return, else void — and with it the return ABI.
    void LambdaExpression::resolveTypes(CajetaModulePtr module) {
        if (!module) module = CajetaModule::getActiveModule();
        if (!module) return;

        bool pushedScope = false;
        // Derive into a LOCAL vector: `paramTypes` is real state the build path owns, and
        // this resolve pass must not touch it.
        std::vector<CajetaTypePtr> effectiveParamTypes = paramTypes;
        if (module->isResolutionOnly()
                && effectiveParamTypes.size() < paramNames.size()) {
            if (auto expectedFn =
                    std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
                if (expectedFn->getParameterTypes().size() == paramNames.size())
                    effectiveParamTypes = expectedFn->getParameterTypes();
            }
        }
        if (body && effectiveParamTypes.size() == paramNames.size()
                && !paramNames.empty()) {
            auto paramScope = std::make_shared<Scope>(
                std::string("__lambda_resolve"), module);
            for (size_t i = 0; i < paramNames.size(); ++i) {
                auto fld = std::make_shared<StackField>(
                    module, paramNames[i], effectiveParamTypes[i]);
                paramScope->putField(fld);
            }
            std::vector<std::pair<std::string, CajetaTypePtr>> bodyLocals;
            collectTopLevelBodyLocals(body, bodyLocals);
            for (auto& nt : bodyLocals) {
                if (!nt.second) continue;
                auto fld = std::make_shared<StackField>(
                    module, nt.first, nt.second);
                paramScope->putField(fld);
            }
            module->getScopeStack().add(paramScope);
            pushedScope = true;
        }
        if (body) body->resolveTypes(module);
        if (pushedScope) {
            module->getScopeStack().pop();
        }

        CajetaTypePtr ret;
        bool returnsOwn = true;
        bool abiPinnedByExpected = false;
        if (auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
            ret = expectedFn->getReturnType();
            returnsOwn = expectedFn->isReturnsOwnership();
            abiPinnedByExpected = true;
        }
        if (!ret) {
            if (auto bexpr = std::dynamic_pointer_cast<Expression>(body)) {
                ret = bexpr->getResolvedType();
                if (Method::exprIsStackConstruction(bexpr)) {
                    returnsOwn = false;
                }
            }
        }
        if (!ret) {
            ret = inferLambdaReturnFromBody(body);
            if (Method::nodeHasStackReturn(body)) {
                returnsOwn = false;
            }
        }
        // Type-driven, as Method::returnsStackValue decides for methods: an INFERRED
        // lambda returning a value-shape class is sret whatever its body does. An
        // explicit function type on the LHS stays authoritative.
        if (!abiPinnedByExpected && ret && Method::isValueShapeReturnType(ret)) {
            returnsOwn = false;
        }
        if (!ret) ret = CajetaType::of("void");
        std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret, returnsOwn);
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find(canon);
        if (it != cmap.end()) {
            resolvedType = it->second;
        } else {
            auto fnType = std::make_shared<CajetaFunctionType>(
                module, paramTypes, ret, returnsOwn);
            cmap[canon] = static_pointer_cast<CajetaType>(fnType);
            resolvedType = fnType;
        }
    }

    // Emits the lambda as its own internal function plus a closure record
    // `{ fn, captures, drop_fn }`, after classifying each free identifier as a value,
    // borrow or `#`-transfer capture. Restores the outer builder before returning.
    llvm::Value* LambdaExpression::generateCode(CajetaModulePtr module) {
        // Bare-identifier params take their types from the surrounding expectedType, which
        // LocalVariableDeclaration sets just before this call — hence here, not in
        // resolveTypes, which runs before that wiring happens.
        if (paramTypes.size() < paramNames.size()) {
            auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType);
            if (expectedFn
                    && expectedFn->getParameterTypes().size() == paramNames.size()) {
                paramTypes = expectedFn->getParameterTypes();
                resolvedType.reset();
            } else {
                throw Exception(
                    "lambda parameter types could not be inferred — "
                    "annotate the parameters explicitly or assign the "
                    "lambda to a function-typed variable so the LHS pins "
                    "the parameter types",
                    "CAJETA_ERROR_TYPE_INFERENCE");
            }
        }
        if (!resolvedType) resolveTypes(module);
        auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(resolvedType);
        if (!fnType) {
            throw Exception("lambda has no function type",
                "CAJETA_ERROR_NOT_IMPLEMENTED");
        }

        if (synthesizedName.empty()) {
            synthesizedName = std::string("__cajeta_lambda_")
                + std::to_string(lambdaCounter++);
        }

        auto* lmod = module->emitTargetLlvmModule();
        if (auto* existing = lmod->getFunction(synthesizedName)) {
            return existing;
        }

        // Capture analysis runs while the OUTER scope is still on top of the stack, before
        // the lambda's own scope is pushed.
        std::set<std::string> bound(paramNames.begin(), paramNames.end());
        std::set<std::string> seen;
        std::vector<std::string> freeNames;
        collectFreeIdentifiers(body, bound, seen, freeNames);

        struct Capture {
            std::string name;
            CajetaTypePtr type;
            bool byValue;     // true: primitive copy; false: pointer slot
            bool byTransfer;  // true: `#name` transfer (only meaningful when
        };
        std::vector<Capture> captures;
        ScopePtr outerScope = module->getScopeStack().isEmpty()
            ? nullptr : module->getScopeStack().peek();
        std::set<std::string> transferNames;
        collectTransferNames(body, transferNames);
        if (outerScope) {
            for (auto& name : freeNames) {
                FieldPtr f = outerScope->getField(name);
                if (!f) continue;  // class name, namespace, or unresolved
                CajetaTypePtr t = f->getType();
                if (!t) continue;
                // A function value is one `ptr` to a closure record, so a BORROW capture of one is
                // the ordinary pointer copy. Only a `#`-transfer of a closure stays deferred:
                // the capturing closure's drop_fn would have to free the record.
                if (std::dynamic_pointer_cast<CajetaFunctionType>(t)) {
                    if (transferNames.find(name) != transferNames.end()) {
                        throw Exception(
                            "transfer-capturing a function value (`#" + name
                            + "`) inside a closure is not supported yet — "
                            "capture it by borrow (drop the `#`) or restructure "
                            "so the closure is not moved",
                            "CAJETA_ERROR_NOT_IMPLEMENTED");
                    }
                }
                // Decide value-vs-pointer capture from the outer field's existing alloca, not the
                // type flags: CajetaArray is flagged PRIMITIVE yet lives behind a pointer slot,
                // so the flag alone mis-routes the capture.
                llvm::AllocaInst* outerSlot = f->getOrCreateAllocation();
                bool slotIsPointer = outerSlot
                    && outerSlot->getAllocatedType()->isPointerTy();
                bool byValue = !slotIsPointer;
                // On a primitive `#` is a no-op marker — there is nothing to transfer — so the
                // capture stays by value.
                bool byTransfer = !byValue
                    && transferNames.find(name) != transferNames.end();
                captures.push_back({name, t, byValue, byTransfer});
                // Any borrow capture ties the closure's lifetime to the enclosing scope; the
                // escape check at ReturnStatement reads this flag.
                if (!byValue && !byTransfer) {
                    hasBorrowCaptures = true;
                }
            }
        }

        if (!captures.empty()) {
            std::set<std::string> valueCapturedNames;
            for (auto& c : captures) {
                if (c.byValue) valueCapturedNames.insert(c.name);
            }
            if (!valueCapturedNames.empty()) {
                enforceValueCaptureImmutability(body, valueCapturedNames);
            }
        }

        // The captures struct is anonymous: LLVM unifies structurally identical literal
        // structs, so the alloc site's type matches the GEPs inside the lambda body.
        auto& llvmCtx = *module->getLlvmContext();
        llvm::StructType* capturesTy = nullptr;
        if (!captures.empty()) {
            std::vector<llvm::Type*> capLlvmTypes;
            capLlvmTypes.reserve(captures.size());
            for (auto& c : captures) {
                capLlvmTypes.push_back(c.byValue
                    ? c.type->getLlvmType()
                    : (llvm::Type*) llvm::PointerType::get(llvmCtx, 0));
            }
            capturesTy = llvm::StructType::get(llvmCtx, capLlvmTypes);
        }

        // Internal linkage: a lambda's function is reached only through its closure
        // record's ADDRESS, and its name comes from a thread_local counter — external
        // linkage lets two modules mint the same name for different bodies.
        llvm::GlobalValue::LinkageTypes lambdaLinkage =
            llvm::Function::InternalLinkage;
        llvm::Function* fn = llvm::Function::Create(
            fnType->getLlvmFunctionType(),
            lambdaLinkage,
            synthesizedName,
            lmod);

        // sret form: arg 0 is the caller-allocated result slot, so captures and user
        // params shift by one; `sretOffset` is that shift for every getArg() below.
        unsigned sretOffset = 0;
        if (fnType->usesSret()) {
            auto retClass = std::dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
            llvm::Type* structTy = retClass ? retClass->getLlvmType() : nullptr;
            if (structTy) {
                fn->addParamAttr(0, llvm::Attribute::get(
                    llvmCtx, llvm::Attribute::StructRet, structTy));
            }
            sretOffset = 1;
        }

        auto* outerBuilder = module->getBuilder();
        llvm::BasicBlock* outerInsertBlock = outerBuilder->GetInsertBlock();
        MethodPtr outerMethod = module->getCurrentMethod();

        // Heap-allocate the captures struct so it can outlive the producing frame; the
        // matching free is in the synthesized drop function built below.
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        llvm::Value* capturesPtr = nullptr;
        const llvm::DataLayout& dl = lmod->getDataLayout();
        if (capturesTy) {
            llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
            if (!allocFn) {
                throw Exception(
                    "runtime helper __cajeta_alloc not linked — cannot "
                    "allocate closure captures",
                    "CAJETA_ERROR_RUNTIME");
            }
            uint64_t capBytes = dl.getTypeAllocSize(capturesTy);
            capturesPtr = outerBuilder->CreateCall(allocFn, {
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx), capBytes),
            }, "captures");
            for (size_t i = 0; i < captures.size(); ++i) {
                auto& cap = captures[i];
                FieldPtr outerField = outerScope->getField(cap.name);
                llvm::AllocaInst* outerSlot = outerField->getOrCreateAllocation();
                llvm::Type* loadTy = cap.byValue
                    ? cap.type->getLlvmType()
                    : (llvm::Type*) llvm::PointerType::get(llvmCtx, 0);
                llvm::Value* outerVal = outerBuilder->CreateLoad(
                    loadTy, outerSlot, cap.name);
                llvm::Value* slot = outerBuilder->CreateStructGEP(
                    capturesTy, capturesPtr, (unsigned) i,
                    std::string("cap.") + cap.name);
                outerBuilder->CreateStore(outerVal, slot);
                // A `#name` transfer hands ownership to the closure, whose drop_fn frees it, so
                // the outer binding's drop entry must be deactivated or both would free it.
                if (cap.byTransfer && outerScope) {
                    outerScope->demoteToBorrow(cap.name);
                    if (llvm::Value* entry = outerField->getDropEntry()) {
                        if (llvm::Function* mark = module->getRuntimeFunction(
                                "__cajeta_drop_mark_inactive")) {
                            outerBuilder->CreateCall(mark, {entry});
                        }
                    }
                }
            }
        }

        llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(
            llvmCtx, "entry", fn);
        llvm::IRBuilder<>* lambdaBuilder = new llvm::IRBuilder<>(entryBB);
        module->setBuilder(lambdaBuilder);

        prof::ProfileFrame lambdaProfFrame;
        // Push a line-info shadow frame, as Method::generateCode's prologue does: this
        // body is emitted inline rather than through that prologue, so without it a
        // lambda is absent from every profile and stack trace.
        {
            CajetaClassPtr lambdaOwner = outerMethod ? outerMethod->getParent()
                                                     : nullptr;
            if (!lambdaOwner && !module->getStructureStack().empty()) {
                lambdaOwner = module->getStructureStack().back();
            }
            std::string typeName = (lambdaOwner && lambdaOwner->getQName())
                ? lambdaOwner->getQName()->toCanonical() : std::string();
            std::string fileName = lambdaOwner ? lambdaOwner->getDeclaringFile()
                                               : std::string();
            if (fileName.empty()) fileName = module->remappedSourcePath();
            dbg::emitLineEnter(module, typeName, "<lambda>", fileName);
            // Stamp the line at the PUSH: Block is the only site that emits statement marks,
            // so an expression body emits none and the frame would keep the 0 it got.
            int lambdaLine = body ? body->getSourceLine() : 0;
            if (lambdaLine <= 0) lambdaLine = getSourceLine();
            if (lambdaLine > 0) lambdaLine = dbg::fileLineFor(module, lambdaLine);
            dbg::emitLineMark(module, lambdaLine);
            lambdaProfFrame = prof::emitProfileEnter(module, typeName,
                                                     "<lambda>", fileName);
        }

        // The lambda's scope is severed from its parent so a body lookup can never reach
        // an outer local: every cross-scope use must come through captures, and a miss
        // surfaces as a missing identifier instead of invalid cross-function IR.
        module->getScopeStack().add(make_shared<Scope>(synthesizedName, module));
        module->getScopeStack().peek()->setParent(nullptr);

        module->setCurrentMethod(nullptr);
        bool savedLambdaRet = module->isLambdaClassPtrReturn();
        bool lambdaClassPtrRet = sretOffset == 0
            && fn->getReturnType()->isPointerTy()
            && std::dynamic_pointer_cast<CajetaClass>(fnType->getReturnType())
            != nullptr;
        module->setLambdaClassPtrReturn(lambdaClassPtrRet);

        for (size_t i = 0; i < paramNames.size(); ++i) {
            // LLVM arg 0 is the implicit captures pointer, so user param i lives at arg i + 1
            // (plus sretOffset when the sret slot precedes captures).
            auto formal = std::make_shared<FormalParameter>(
                paramNames[i], paramTypes[i]);
            auto field = std::make_shared<ParameterField>(
                module, formal, fn, (int) i + 1 + (int) sretOffset);
            field->getOrCreateAllocation();
            module->getScopeStack().peek()->putField(field);
        }

        if (capturesTy) {
            llvm::Value* capArg = fn->getArg(sretOffset);
            llvm::Type* ptrLlvmTy = llvm::PointerType::get(llvmCtx, 0);
            for (size_t i = 0; i < captures.size(); ++i) {
                auto& cap = captures[i];
                llvm::Value* slot = lambdaBuilder->CreateStructGEP(
                    capturesTy, capArg, (unsigned) i,
                    std::string("cap.") + cap.name);
                llvm::Type* slotTy = cap.byValue
                    ? cap.type->getLlvmType()
                    : ptrLlvmTy;
                llvm::Value* val = lambdaBuilder->CreateLoad(
                    slotTy, slot, cap.name);
                llvm::AllocaInst* localSlot = lambdaBuilder->CreateAlloca(
                    slotTy, nullptr, cap.name);
                lambdaBuilder->CreateStore(val, localSlot);
                auto capField = std::make_shared<StackField>(
                    module, cap.name, cap.type);
                capField->setAllocation(localSlot);
                module->getScopeStack().peek()->putField(capField);
            }
        }

        // An expression body's value becomes the implicit return; a block body emits its
        // own ReturnStatements.
        bool blockBody = std::dynamic_pointer_cast<Block>(body) != nullptr;
        // NRVO: when the sret-shaped body IS a `stack X(...)`, redirect the construction
        // at the sret slot so the ctor writes into the caller's memory.
        if (sretOffset && !blockBody) {
            if (auto nx = std::dynamic_pointer_cast<NewExpression>(body)) {
                if (nx->getStackAlloc()) nx->setNrvoTarget(fn->getArg(0));
            }
        }
        // Isolate the enclosing method's try-finally stack: this body is emitted inline,
        // but its returns must unwind only the frames the lambda itself pushed.
        std::vector<std::shared_ptr<void>> savedTryFrames = module->takeTryFinally();
        llvm::Value* bodyVal = body->generateCode(module);

        if (blockBody) {
            // A block that falls through to its closing brace leaves the insertion block
            // without a terminator; emit a default return so verify is satisfied.
            llvm::BasicBlock* tail = lambdaBuilder->GetInsertBlock();
            if (tail && !tail->hasTerminator()) {
                llvm::Type* retTy = fn->getReturnType();
                if (retTy->isVoidTy()) {
                    lambdaBuilder->CreateRetVoid();
                } else {
                    lambdaBuilder->CreateRet(llvm::UndefValue::get(retTy));
                }
            }
        } else {
            // L-value to r-value coercion, mirroring ReturnStatement's conversions — keep the
            // two in sync when either grows a case.
            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(bodyVal)) {
                bodyVal = lambdaBuilder->CreateLoad(a->getAllocatedType(), a);
            } else if (auto idx = std::dynamic_pointer_cast<ArrayIndexExpression>(body)) {
                CajetaTypePtr elemType = idx->getResolvedType();
                if (elemType) {
                    llvm::Type* loadTy;
                    if (std::dynamic_pointer_cast<CajetaArray>(elemType)
                            || (elemType->getTypeFlags() & STRUCT_FLAG)) {
                        loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                    } else {
                        loadTy = elemType->getLlvmType();
                    }
                    if (loadTy && bodyVal) {
                        bodyVal = lambdaBuilder->CreateLoad(loadTy, bodyVal);
                    }
                }
            } else if (auto dot = std::dynamic_pointer_cast<DotExpression>(body)) {
                if (!dot->getChildren().empty()) {
                    auto recv = std::dynamic_pointer_cast<Expression>(dot->getChildren()[0]);
                    if (recv) {
                        if (!recv->getResolvedType()) recv->resolveTypes(module);
                        if (auto klass = std::dynamic_pointer_cast<CajetaClass>(recv->getResolvedType())) {
                            auto& props = klass->getProperties();
                            auto it = props.find(dot->getIdentifier());
                            if (it != props.end() && bodyVal) {
                                if (llvm::Type* lt = it->second->getType()->getLlvmType()) {
                                    bodyVal = lambdaBuilder->CreateLoad(lt, bodyVal);
                                }
                            }
                        }
                    }
                }
            }
            if (bodyVal) {
                if (sretOffset) {
                    llvm::Value* sretPtr = fn->getArg(0);
                    if (bodyVal != sretPtr) {
                        auto retClass = std::dynamic_pointer_cast<CajetaClass>(
                            fnType->getReturnType());
                        llvm::Type* structTy = retClass
                            ? retClass->getLlvmType() : nullptr;
                        if (structTy) {
                            if (bodyVal->getType()->isPointerTy()) {
                                const llvm::DataLayout& dl2 = lmod->getDataLayout();
                                llvm::Value* sz = llvm::ConstantInt::get(
                                    llvm::Type::getInt64Ty(llvmCtx),
                                    dl2.getTypeAllocSize(structTy));
                                lambdaBuilder->CreateMemCpy(sretPtr,
                                    llvm::MaybeAlign(8), bodyVal,
                                    llvm::MaybeAlign(8), sz);
                            } else {
                                lambdaBuilder->CreateStore(bodyVal, sretPtr);
                            }
                        }
                    }
                    lambdaBuilder->CreateRetVoid();
                } else {
                    llvm::Type* retTy = fn->getReturnType();
                    if (retTy && !retTy->isVoidTy() && bodyVal->getType() != retTy) {
                        if (retTy->isIntegerTy() && bodyVal->getType()->isIntegerTy()) {
                            bodyVal = lambdaBuilder->CreateIntCast(
                                bodyVal, retTy, /*isSigned=*/true);
                        } else if (retTy->isFloatingPointTy()
                                && bodyVal->getType()->isFloatingPointTy()) {
                            bodyVal = lambdaBuilder->CreateFPCast(bodyVal, retTy);
                        }
                    }
                    if (retTy->isVoidTy()) {
                        lambdaBuilder->CreateRetVoid();
                    } else {
                        // Return flag by shape: a fresh construction or `#x` is owned, a tail call's own
                        // flag rides through untouched, anything else is a borrow.
                        if (lambdaClassPtrRet) {
                            AbstractSyntaxNodePtr shape = body;
                            while (auto cx = std::dynamic_pointer_cast<
                                       CastExpression>(shape)) {
                                if (cx->getChildren().empty()) break;
                                shape = cx->getChildren()[0];
                            }
                                ownership::TitleShape lamSh = ownership::classify(std::dynamic_pointer_cast<Expression>(shape), module);
                                bool rides = lamSh.family == ownership::TitleFamily::CallResult
                                    || lamSh.family == ownership::TitleFamily::ClosureCall;
                            if (!rides) {
                                bool owned = (lamSh.family == ownership::TitleFamily::Fresh
                                        && lamSh.answer == ownership::TitleAnswer::Owned)
                                    || lamSh.family == ownership::TitleFamily::Move
                                    || lamSh.family == ownership::TitleFamily::Concat;
                                if (llvm::Function* fsFn = module->getRuntimeFunction(
                                        "__cajeta_return_flag_set")) {
                                    lambdaBuilder->CreateCall(fsFn,
                                        {lambdaBuilder->getInt64(owned ? 1 : 0)});
                                }
                            }
                        }
                        lambdaBuilder->CreateRet(bodyVal);
                    }
                }
            } else {
                if (fn->getReturnType()->isVoidTy()) {
                    lambdaBuilder->CreateRetVoid();
                } else {
                    lambdaBuilder->CreateRet(
                        llvm::UndefValue::get(fn->getReturnType()));
                }
            }
        }

        module->setLambdaClassPtrReturn(savedLambdaRet);
        module->restoreTryFinally(std::move(savedTryFrames));
        module->getScopeStack().pop();
        delete lambdaBuilder;
        // Pop the shadow frame at EVERY `ret`: a lambda has many exits and
        // emitScopeExitToWatermark cannot serve them (it bails on the null current
        // method a lambda body deliberately runs with). Throwing paths end unreachable.
        if (module->getFlags().lineInfo) {
            if (llvm::Function* leaveFn =
                    module->getRuntimeFunction("__cajeta_line_leave")) {
                for (llvm::BasicBlock& bb : *fn) {
                    if (auto* ri = llvm::dyn_cast_or_null<llvm::ReturnInst>(
                            bb.getTerminator())) {
                        llvm::IRBuilder<> leaveBuilder(ri);
                        leaveBuilder.CreateCall(leaveFn, {});
                    }
                }
            }
        }
        prof::emitProfileExitAtReturns(module, fn, lambdaProfFrame);

        module->setBuilder(outerBuilder);
        module->setCurrentMethod(outerMethod);
        outerBuilder->SetInsertPoint(outerInsertBlock);

        // Closure layout `{ ptr fn, ptr captures, ptr drop_fn }`; the drop_fn slot lets
        // __cajeta_closure_drop find the free routine with no static type at the site.
        // Heap when there are captures (it may outlive the frame), stack when none.
        llvm::StructType* closureTy = llvm::StructType::get(llvmCtx,
            {(llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy});
        uint64_t closureBytes = dl.getTypeAllocSize(closureTy);

        // The per-lambda drop function frees each transferred capture, then the captures
        // struct, then the record. Borrow and value captures are not owned.
        llvm::Value* dropFnValue = llvm::ConstantPointerNull::get(ptrTy);
        if (capturesTy) {
            llvm::Function* freeArrayFn = module->getRuntimeFunction("__cajeta_free_array");
            llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
            std::string dropName = synthesizedName + "_drop";
            llvm::FunctionType* dropFnTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvmCtx), {ptrTy}, /*isVarArg=*/false);
            llvm::Function* dropFn = llvm::Function::Create(dropFnTy,
                llvm::Function::InternalLinkage, dropName, lmod);
            llvm::BasicBlock* dropEntryBB = llvm::BasicBlock::Create(
                llvmCtx, "entry", dropFn);
            llvm::IRBuilder<> dropBuilder(dropEntryBB);
            llvm::Value* closureArg = dropFn->getArg(0);
            llvm::Value* capLoadSlot = dropBuilder.CreateStructGEP(
                closureTy, closureArg, 1, "closure.captures");
            llvm::Value* capLoaded = dropBuilder.CreateLoad(
                ptrTy, capLoadSlot, "captures");
            for (size_t i = 0; i < captures.size(); ++i) {
                if (!captures[i].byTransfer) continue;
                // Only CajetaArray transfer captures have a known drop function today; other
                // transferred types compile but leak.
                if (!std::dynamic_pointer_cast<CajetaArray>(captures[i].type)) continue;
                if (!freeArrayFn) continue;
                llvm::Value* slot = dropBuilder.CreateStructGEP(
                    capturesTy, capLoaded, (unsigned) i,
                    std::string("cap.") + captures[i].name);
                llvm::Value* heapPtr = dropBuilder.CreateLoad(
                    ptrTy, slot, captures[i].name);
                dropBuilder.CreateCall(freeArrayFn, {heapPtr});
            }
            if (freeFn) {
                dropBuilder.CreateCall(freeFn, {capLoaded});
                dropBuilder.CreateCall(freeFn, {closureArg});
            }
            dropBuilder.CreateRetVoid();
            dropFnValue = dropFn;
        }

        // Non-capturing closures get a private global record — program lifetime, so they
        // may escape without a heap allocation; capturing ones are heap-allocated.
        llvm::Value* closure;
        if (!capturesTy) {
            llvm::Constant* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            llvm::Constant* init = llvm::ConstantStruct::get(closureTy,
                {static_cast<llvm::Constant*>(fn), nullPtr, nullPtr});
            auto* gv = new llvm::GlobalVariable(*lmod, closureTy,
                /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                init, synthesizedName + "_record");
            return gv;
        }
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            throw Exception(
                "runtime helper __cajeta_alloc not linked — cannot "
                "allocate closure record",
                "CAJETA_ERROR_RUNTIME");
        }
        closure = outerBuilder->CreateCall(allocFn, {
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx), closureBytes),
        }, "closure");
        llvm::Value* fnSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 0, "closure.fn");
        outerBuilder->CreateStore(fn, fnSlot);
        llvm::Value* capSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 1, "closure.captures");
        outerBuilder->CreateStore(capturesPtr, capSlot);
        llvm::Value* dropSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 2, "closure.drop_fn");
        outerBuilder->CreateStore(dropFnValue, dropSlot);
        return closure;
    }

    // --- MethodReferenceExpression ---------------------------------------

    static thread_local int64_t methodRefCounter = 0;  // per-thread (threadsafe U4)

    // Resolves a method reference's LHS to a target class, reporting whether it resolved
    // as a runtime VALUE (a bindable receiver) or as a TYPE NAME. `Util::m` parses as
    // an expression, so a bare class name falls back to a registry scan.
    struct MethodRefTargetResolution {
        CajetaClassPtr targetClass;
        bool receiverIsValue;
    };
    static MethodRefTargetResolution resolveMethodRefTarget(
            CajetaTypePtr receiverType,
            ExpressionPtr receiverExpr,
            CajetaModulePtr module) {
        MethodRefTargetResolution out{nullptr, false};
        if (receiverType) {
            out.targetClass = std::dynamic_pointer_cast<CajetaClass>(receiverType);
            return out;
        }
        if (!receiverExpr) return out;
        if (!receiverExpr->getResolvedType()) {
            receiverExpr->resolveTypes(module);
        }
        if (auto k = std::dynamic_pointer_cast<CajetaClass>(receiverExpr->getResolvedType())) {
            out.targetClass = k;
            out.receiverIsValue = true;
            return out;
        }
        auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(receiverExpr);
        if (!idExpr) return out;
        const std::string& shortName = idExpr->getTextValue();
        for (auto& entry : CajetaType::getCanonicalMap()) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(entry.second);
            if (!klass) continue;
            auto qn = klass->getQName();
            if (qn && qn->getTypeName() == shortName) {
                out.targetClass = klass;
                return out;
            }
        }
        return out;
    }

    // Convenience wrapper for sites that only need the class.
    static CajetaClassPtr resolveMethodRefTargetClass(
            CajetaTypePtr receiverType,
            ExpressionPtr receiverExpr,
            CajetaModulePtr module) {
        return resolveMethodRefTarget(receiverType, receiverExpr, module).targetClass;
    }

    // First method on `klass` with this name and staticness. No overload resolution yet
    // — one method per name is assumed, and the first match wins.
    static MethodPtr findMethodByName(CajetaClassPtr klass,
                                       const std::string& name,
                                       bool wantStatic) {
        if (!klass) return nullptr;
        for (auto& entry : klass->getMethods()) {
            MethodPtr m = entry.second;
            if (!m || m->getName() != name) continue;
            bool isStatic = m->getModifiers().find(STATIC) != m->getModifiers().end();
            if (isStatic != wantStatic) continue;
            return m;
        }
        return nullptr;
    }

    // Classifies the reference (constructor, static, bound or unbound instance) and
    // derives its function-value type, including the sret-versus-ownership return ABI.
    // Rejects a `^` (view-returning) target: function types carry no view stance.
    void MethodReferenceExpression::resolveTypes(CajetaModulePtr module) {
        if (!module) module = CajetaModule::getActiveModule();
        if (!module) return;

        MethodRefTargetResolution res = resolveMethodRefTarget(
            receiverType, receiverExpr, module);
        CajetaClassPtr targetClass = res.targetClass;

        if (isCtor) {
            kind = Kind::CONSTRUCTOR;
            if (!targetClass) return;
            MethodPtr ctor;
            for (auto& entry : targetClass->getMethods()) {
                if (entry.second && entry.second->isConstructor()) {
                    ctor = entry.second;
                    break;
                }
            }
            if (!ctor) return;
            std::vector<CajetaTypePtr> paramTypes;
            auto pl = ctor->getParameterList();
            for (size_t i = 1; i < pl.size(); ++i) {
                paramTypes.push_back(pl[i]->getType());
            }
            CajetaTypePtr ret = std::static_pointer_cast<CajetaType>(targetClass);
            // A constructor reference returns ownership of a heap instance, so it is always
            // ownership-form; an sret slot would discard the owner role and leak.
            bool returnsOwn = true;
            if (auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
                if (!expectedFn->isReturnsOwnership() && expectedFn->usesSret()) {
                    throw Exception(
                        "method reference '::heap' returns a heap-owned "
                        "instance; cannot assign to an sret-form function "
                        "type (would leak the allocation)",
                        "CAJETA_ERROR_TYPE_MISMATCH");
                }
            }
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret, returnsOwn);
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canon);
            if (it != cmap.end()) {
                resolvedType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, paramTypes, ret, returnsOwn);
                cmap[canon] = std::static_pointer_cast<CajetaType>(fnType);
                resolvedType = fnType;
            }
            return;
        }
        if (!targetClass) return;  // unresolved receiver; codegen errors

        MethodPtr staticMethod = findMethodByName(targetClass, methodName, /*wantStatic=*/true);
        if (staticMethod) {
            kind = Kind::STATIC;
            std::vector<CajetaTypePtr> paramTypes;
            for (auto& p : staticMethod->getParameterList()) {
                paramTypes.push_back(p->getType());
            }
            CajetaTypePtr ret = staticMethod->getReturnType();
            // sret form iff the target returns by stack value. An LHS-pinned sret type wins,
            // and generateCode bridges a borrow method into the slot; an ownership-returning
            // method cannot adapt.
            bool returnsOwn = !staticMethod->returnsStackValue();
            if (staticMethod->isReturnsView()) {
                throw Exception(
                    "method reference '" + methodName + "' targets a `^` "
                    "(view-returning) method: function types carry no view "
                    "stance, so the reference would erase the signature fact "
                    "callers rely on (and a `#R` function type would license "
                    "a `#=` receipt over the receiver's interior). Call the "
                    "method directly, or wrap it in a lambda that returns an "
                    "owned copy. See "
                    "specs/stdlib-ownership-convention-spec.md §4.7.",
                    "CAJETA_ERROR_VIEW_REFERENCE_UNSUPPORTED");
            }
            if (auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
                bool expectedSret = !expectedFn->isReturnsOwnership()
                    && expectedFn->usesSret();
                if (expectedSret) {
                    if (staticMethod->isReturnsOwnership()) {
                        throw Exception(
                            "method reference '" + methodName + "' returns "
                            "heap ownership; cannot assign to an sret-form "
                            "function type (would leak the allocation)",
                            "CAJETA_ERROR_TYPE_MISMATCH");
                    }
                    returnsOwn = false;
                } else if (staticMethod->returnsStackValue()) {
                    throw Exception(
                        "method reference '" + methodName + "' returns by "
                        "stack value; cannot assign to a #R ownership "
                        "function type",
                        "CAJETA_ERROR_TYPE_MISMATCH");
                }
            }
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret, returnsOwn);
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canon);
            if (it != cmap.end()) {
                resolvedType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, paramTypes, ret, returnsOwn);
                cmap[canon] = std::static_pointer_cast<CajetaType>(fnType);
                resolvedType = fnType;
            }
            return;
        }

        MethodPtr instanceMethod = findMethodByName(targetClass, methodName, /*wantStatic=*/false);
        if (instanceMethod) {
            kind = res.receiverIsValue
                ? Kind::BOUND_INSTANCE
                : Kind::UNBOUND_INSTANCE;
            // An instance method's parameterList carries the synthesized `this` at index 0.
            std::vector<CajetaTypePtr> paramTypes;
            auto pl = instanceMethod->getParameterList();
            if (kind == Kind::UNBOUND_INSTANCE) {
                paramTypes.push_back(targetClass);
            }
            for (size_t i = 1; i < pl.size(); ++i) {
                paramTypes.push_back(pl[i]->getType());
            }
            CajetaTypePtr ret = instanceMethod->getReturnType();
            bool returnsOwn = !instanceMethod->returnsStackValue();
            if (instanceMethod->isReturnsView()) {
                throw Exception(
                    "method reference '" + methodName + "' targets a `^` "
                    "(view-returning) method: function types carry no view "
                    "stance, so the reference would erase the signature fact "
                    "callers rely on (and a `#R` function type would license "
                    "a `#=` receipt over the receiver's interior). Call the "
                    "method directly, or wrap it in a lambda that returns an "
                    "owned copy. See "
                    "specs/stdlib-ownership-convention-spec.md §4.7.",
                    "CAJETA_ERROR_VIEW_REFERENCE_UNSUPPORTED");
            }
            if (auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
                bool expectedSret = !expectedFn->isReturnsOwnership()
                    && expectedFn->usesSret();
                if (expectedSret) {
                    if (instanceMethod->isReturnsOwnership()) {
                        throw Exception(
                            "method reference '" + methodName + "' returns "
                            "heap ownership; cannot assign to an sret-form "
                            "function type (would leak the allocation)",
                            "CAJETA_ERROR_TYPE_MISMATCH");
                    }
                    returnsOwn = false;
                } else if (instanceMethod->returnsStackValue()) {
                    throw Exception(
                        "method reference '" + methodName + "' returns by "
                        "stack value; cannot assign to a #R ownership "
                        "function type",
                        "CAJETA_ERROR_TYPE_MISMATCH");
                }
            }
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret, returnsOwn);
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canon);
            if (it != cmap.end()) {
                resolvedType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, paramTypes, ret, returnsOwn);
                cmap[canon] = std::static_pointer_cast<CajetaType>(fnType);
                resolvedType = fnType;
            }
            if (kind == Kind::BOUND_INSTANCE) {
                _hasBorrowCaptures = true;
            }
        }
    }

    // Emits a thunk matching the closure ABI `(ptr captures, params...)` plus the closure
    // record: a private global for static, unbound and constructor references, a heap
    // record with a drop function for a bound instance whose receiver is captured.
    llvm::Value* MethodReferenceExpression::generateCode(CajetaModulePtr module) {
        if (!resolvedType) resolveTypes(module);

        if (kind == Kind::CONSTRUCTOR) {
            auto& llvmCtx = *module->getLlvmContext();
            auto* lmod = module->emitTargetLlvmModule();
            llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);

            auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(resolvedType);
            CajetaClassPtr targetClass = resolveMethodRefTargetClass(
                receiverType, receiverExpr, module);
            if (!fnType || !targetClass) {
                throw Exception(
                    "method reference: constructor reference target "
                    "did not resolve to a class type",
                    "CAJETA_ERROR_NOT_IMPLEMENTED");
            }
            MethodPtr ctor;
            for (auto& entry : targetClass->getMethods()) {
                if (entry.second && entry.second->isConstructor()) {
                    ctor = entry.second;
                    break;
                }
            }
            if (!ctor || !ctor->getLlvmFunction()) {
                throw Exception(
                    "constructor reference: target constructor has no "
                    "LLVM function at codegen time",
                    "CAJETA_ERROR_NOT_IMPLEMENTED");
            }

            // The thunk body mirrors ClassCreatorRest: malloc the struct, write the vtable,
            // call the ctor, return the instance.
            if (thunkName.empty()) {
                thunkName = std::string("__cajeta_method_ref_")
                    + std::to_string(methodRefCounter++);
            }
            llvm::Function* existing = lmod->getFunction(thunkName);
            llvm::Function* thunk = existing
                ? existing
                : llvm::Function::Create(fnType->getLlvmFunctionType(),
                    llvm::Function::InternalLinkage, thunkName, lmod);
            if (!existing) {
                llvm::BasicBlock* tbb = llvm::BasicBlock::Create(
                    llvmCtx, "entry", thunk);
                llvm::IRBuilder<> tb(tbb);

                llvm::Type* structTy = targetClass->getLlvmType();
                const llvm::DataLayout& dl = lmod->getDataLayout();
                llvm::Constant* allocSize = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(llvmCtx),
                    dl.getTypeAllocSize(structTy));
                llvm::CallInst* instance = MemoryManager::createMallocInstruction(
                    module, allocSize, tbb);

                // The vtable pointer goes in slot 0. Same cross-module fixup as CreatorRest: the
                // vtable global may live in a different llvm::Module than this thunk.
                if (llvm::GlobalVariable* vtable = targetClass->getVirtualTableGlobal()) {
                    llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                        lmod, vtable);
                    llvm::Value* vptrSlot = tb.CreateStructGEP(
                        structTy, instance, /*idx=*/0, "vtable_slot");
                    tb.CreateStore(vtableRef, vptrSlot);
                }

                std::vector<llvm::Value*> ctorArgs;
                ctorArgs.push_back(instance);
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = 1; i < thunkArgCount; ++i) {
                    ctorArgs.push_back(thunk->getArg(i));
                }
                if (ctor->needsTransferWord()) {
                    ctorArgs.push_back(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(llvmCtx), 0));
                }
                llvm::Function* ctorFn = CajetaModule::ensureFunctionInModule(
                    lmod, ctor->getLlvmFunction());
                tb.CreateCall(ctor->getLlvmFunctionType(), ctorFn, ctorArgs);
                tb.CreateRet(instance);
            }

            llvm::StructType* closureTy = llvm::StructType::get(llvmCtx,
                {(llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy});
            llvm::Constant* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            llvm::Constant* init = llvm::ConstantStruct::get(closureTy,
                {static_cast<llvm::Constant*>(thunk), nullPtr, nullPtr});
            auto* gv = new llvm::GlobalVariable(*lmod, closureTy,
                /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                init, thunkName + "_record");
            return gv;
        }

        auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(resolvedType);
        if (!fnType) {
            throw Exception(
                "method reference '::' "
                + methodName
                + "' did not resolve to a known method",
                "CAJETA_ERROR_NOT_IMPLEMENTED");
        }

        CajetaClassPtr targetClass = resolveMethodRefTargetClass(
            receiverType, receiverExpr, module);
        MethodPtr target = findMethodByName(targetClass, methodName,
            /*wantStatic=*/kind == Kind::STATIC);
        if (!target || !target->getLlvmFunction()) {
            throw Exception(
                "method reference: target method '"
                + methodName + "' has no LLVM function at codegen time",
                "CAJETA_ERROR_NOT_IMPLEMENTED");
        }

        auto& llvmCtx = *module->getLlvmContext();
        auto* lmod = module->emitTargetLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);

        // Thunk ABI `(ptr captures, user params...) -> return`. STATIC ignores captures;
        // BOUND_INSTANCE loads the receiver from captures[0] and passes it as `this`.
        if (thunkName.empty()) {
            thunkName = std::string("__cajeta_method_ref_")
                + std::to_string(methodRefCounter++);
        }
        llvm::Function* existing = lmod->getFunction(thunkName);
        llvm::Function* thunk = existing
            ? existing
            : llvm::Function::Create(fnType->getLlvmFunctionType(),
                llvm::Function::InternalLinkage, thunkName, lmod);
        // An sret-shaped thunk takes the hidden result slot at arg 0, shifting the
        // captures and user-arg indices by one.
        unsigned sretOffset = 0;
        if (!existing && fnType->usesSret()) {
            auto retClass = std::dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
            llvm::Type* structTy = retClass ? retClass->getLlvmType() : nullptr;
            if (structTy) {
                thunk->addParamAttr(0, llvm::Attribute::get(
                    llvmCtx, llvm::Attribute::StructRet, structTy));
            }
            sretOffset = 1;
        }
        // Adapter: the thunk is sret-shaped but the target returns a pointer, so capture
        // its result and memcpy into the sret slot before `ret void`.
        bool sretAdapter = sretOffset && !target->returnsStackValue();
        if (!existing) {
            llvm::BasicBlock* tbb = llvm::BasicBlock::Create(
                llvmCtx, "entry", thunk);
            llvm::IRBuilder<> tb(tbb);
            std::vector<llvm::Value*> callArgs;
            if (sretOffset && !sretAdapter) {
                callArgs.push_back(thunk->getArg(0));
            }
            if (kind == Kind::BOUND_INSTANCE) {
                // The captures struct is `{ ptr receiver }` at thunk arg `sretOffset`.
                std::vector<llvm::Type*> capFields = {(llvm::Type*) ptrTy};
                llvm::StructType* capStructTy = llvm::StructType::get(
                    llvmCtx, capFields);
                llvm::Value* capArg = thunk->getArg(sretOffset);
                llvm::Value* recvSlot = tb.CreateStructGEP(
                    capStructTy, capArg, 0, "captured.this");
                llvm::Value* recvVal = tb.CreateLoad(ptrTy, recvSlot, "this");
                callArgs.push_back(recvVal);
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = sretOffset + 1; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            } else if (kind == Kind::UNBOUND_INSTANCE) {
                callArgs.push_back(thunk->getArg(sretOffset + 1));
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = sretOffset + 2; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            } else {
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = sretOffset + 1; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            }
            // A method reference has no `#` spelling, so every call through the thunk lends:
            // the trailing transfer word is the all-borrow 0.
            if (target->needsTransferWord()) {
                callArgs.push_back(llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(llvmCtx), 0));
            }
            llvm::Function* targetFn = CajetaModule::ensureFunctionInModule(
                lmod, target->getLlvmFunction());
            llvm::CallInst* call = tb.CreateCall(
                target->getLlvmFunctionType(), targetFn, callArgs);
            if (sretOffset && !sretAdapter) {
                auto retClass = std::dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
                llvm::Type* structTy = retClass ? retClass->getLlvmType() : nullptr;
                if (structTy) {
                    call->addParamAttr(0, llvm::Attribute::get(
                        llvmCtx, llvm::Attribute::StructRet, structTy));
                }
            }
            if (sretAdapter) {
                auto retClass = std::dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
                llvm::Type* structTy = retClass ? retClass->getLlvmType() : nullptr;
                if (structTy && call) {
                    const llvm::DataLayout& dl = lmod->getDataLayout();
                    llvm::Value* sz = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(llvmCtx),
                        dl.getTypeAllocSize(structTy));
                    tb.CreateMemCpy(thunk->getArg(0), llvm::MaybeAlign(8),
                        call, llvm::MaybeAlign(8), sz);
                }
            }
            if (thunk->getReturnType()->isVoidTy()) {
                tb.CreateRetVoid();
            } else {
                tb.CreateRet(call);
            }
        }

        llvm::StructType* closureTy = llvm::StructType::get(llvmCtx,
            {(llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy});

        if (kind == Kind::STATIC || kind == Kind::UNBOUND_INSTANCE) {
            llvm::Constant* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            llvm::Constant* init = llvm::ConstantStruct::get(closureTy,
                {static_cast<llvm::Constant*>(thunk), nullPtr, nullPtr});
            auto* gv = new llvm::GlobalVariable(*lmod, closureTy,
                /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                init, thunkName + "_record");
            return gv;
        }

        auto* outerBuilder = module->getBuilder();
        const llvm::DataLayout& dl = lmod->getDataLayout();
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
        if (!allocFn || !freeFn) {
            throw Exception(
                "runtime helpers __cajeta_alloc / __cajeta_free not linked",
                "CAJETA_ERROR_RUNTIME");
        }

        std::vector<llvm::Type*> bindFields = {(llvm::Type*) ptrTy};
        llvm::StructType* capStructTy = llvm::StructType::get(
            llvmCtx, bindFields);
        uint64_t capBytes = dl.getTypeAllocSize(capStructTy);
        llvm::Value* capturesPtr = outerBuilder->CreateCall(allocFn, {
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx), capBytes),
        }, "captures");
        llvm::Value* recvValue = receiverExpr->generateCode(module);
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(recvValue)) {
            recvValue = outerBuilder->CreateLoad(
                a->getAllocatedType(), a);
        }
        llvm::Value* recvStoreSlot = outerBuilder->CreateStructGEP(
            capStructTy, capturesPtr, 0, "captures.this");
        outerBuilder->CreateStore(recvValue, recvStoreSlot);

        std::string dropName = thunkName + "_drop";
        llvm::FunctionType* dropFnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), {ptrTy}, /*isVarArg=*/false);
        llvm::Function* dropFn = llvm::Function::Create(dropFnTy,
            llvm::Function::InternalLinkage, dropName, lmod);
        llvm::BasicBlock* dropEntryBB = llvm::BasicBlock::Create(
            llvmCtx, "entry", dropFn);
        {
            llvm::IRBuilder<> db(dropEntryBB);
            llvm::Value* closureArg = dropFn->getArg(0);
            llvm::Value* capSlotInDrop = db.CreateStructGEP(
                closureTy, closureArg, 1, "closure.captures");
            llvm::Value* capLoaded = db.CreateLoad(ptrTy, capSlotInDrop, "captures");
            db.CreateCall(freeFn, {capLoaded});
            db.CreateCall(freeFn, {closureArg});
            db.CreateRetVoid();
        }

        uint64_t closureBytes = dl.getTypeAllocSize(closureTy);
        llvm::Value* closure = outerBuilder->CreateCall(allocFn, {
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx), closureBytes),
        }, "closure");
        llvm::Value* fnSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 0, "closure.fn");
        outerBuilder->CreateStore(thunk, fnSlot);
        llvm::Value* capSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 1, "closure.captures");
        outerBuilder->CreateStore(capturesPtr, capSlot);
        llvm::Value* dropSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 2, "closure.drop_fn");
        outerBuilder->CreateStore(dropFn, dropSlot);
        return closure;
    }

    // `instanceof` always types as boolean, whatever the operands.
    void InstanceOfExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        resolvedType = CajetaType::of("boolean");
    }

    // Erased base of a canonical template name: everything before the first '<'
    // (`cajeta.math.Tensor<cajeta.float32>` -> `cajeta.math.Tensor`). For a
    // non-template name it is the name itself.
    static std::string erasedBaseOf(const std::string& canonical) {
        auto lt = canonical.find('<');
        return lt == std::string::npos ? canonical : canonical.substr(0, lt);
    }

    static bool boundedWildcardTarget(const CajetaTypePtr& type,
                                      std::string& baseCanon, int& argIndex,
                                      std::string& boundCanon) {
        auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
        if (!klass) return false;
        const auto& args = klass->getTypeArguments();
        if (args.size() != 1) return false;             // single-arg only (v1)
        const auto& arg = args[0];
        if (!arg || !arg->isWildcard()
                || arg->wildcardKind() != CajetaType::WildcardKind::Extends)
            return false;
        CajetaTypePtr bound = arg->wildcardBound();
        if (!bound) return false;
        baseCanon = erasedBaseOf(type->toCanonical());
        argIndex = 0;
        boundCanon = bound->toCanonical();
        return true;
    }

    // Emits the type test. cajeta monomorphizes, so a static type that pins the answer
    // folds to a constant while a wildcard or differing instantiation asks the RTTI.
    // A pattern name binds the object on a match and null otherwise.
    llvm::Value* InstanceOfExpression::generateCode(CajetaModulePtr module) {
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);

        if (!type || children.empty()) {
            if (!children.empty()) children[0]->generateCode(module);
            return llvm::ConstantInt::getFalse(i1);
        }
        auto lhsExpr = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhsExpr) {
            children[0]->generateCode(module);
            return llvm::ConstantInt::getFalse(i1);
        }
        if (!lhsExpr->getResolvedType()) {
            lhsExpr->resolveTypes(module);
        }
        CajetaTypePtr lhsType = lhsExpr->getResolvedType();

        std::string targetCanon = type->toCanonical();
        std::string lhsCanon = lhsType ? lhsType->toCanonical() : std::string();
        bool lhsIsWildcard = lhsCanon.find('?') != std::string::npos;
        bool targetConcrete = targetCanon.find('?') == std::string::npos;
        bool sameBase = lhsType && erasedBaseOf(lhsCanon) == erasedBaseOf(targetCanon);
        // A CLASS-typed lhs whose static type differs from the target is the plain
        // downcast question, so it must also ask the RTTI rather than folding to false.
        auto targetClass = dynamic_pointer_cast<CajetaClass>(type);
        bool lhsIsClass = (bool) dynamic_pointer_cast<CajetaClass>(lhsType);
        bool wantRuntime = targetClass && targetConcrete && lhsType
            && lhsCanon != targetCanon
            && (lhsIsWildcard || sameBase || lhsIsClass);

        std::string boundBaseCanon, boundCanon;
        int boundArgIdx = -1;
        bool wantBounded = boundedWildcardTarget(type, boundBaseCanon,
                                                 boundArgIdx, boundCanon)
            && lhsType && (lhsIsWildcard || sameBase);

        auto* builder = module->getBuilder();

        bool needObj = wantRuntime || wantBounded || !pattern.empty();
        llvm::Value* objPtr = nullptr;
        if (needObj) {
            llvm::Value* raw = children[0]->generateCode(module);
            objPtr = loadIfLValue(module, raw, lhsExpr);
            // An interface-typed lhs points at the fat body `{ data, vtable, kind }`; strip to
            // the data pointer before any runtime type question or pattern binding.
            auto lhsIfaceClass = dynamic_pointer_cast<CajetaClass>(lhsType);
            if (objPtr && objPtr->getType()->isPointerTy() && lhsIfaceClass
                    && lhsIfaceClass->isInterface()) {
                // The body pointer itself can be null (`Shape s = null` stores a plain null), so
                // the data-slot load is null-guarded: branch, load on the non-null arm, phi.
                llvm::Type* bodyTy = lhsIfaceClass->getLlvmType();
                llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
                llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(
                    ctx, "instanceof.iface_load", parentFn);
                llvm::BasicBlock* contBb = llvm::BasicBlock::Create(
                    ctx, "instanceof.iface_cont", parentFn);
                llvm::Value* isNull = builder->CreateICmpEQ(
                    objPtr, llvm::ConstantPointerNull::get(ptrTy),
                    "instanceof.body_null");
                llvm::BasicBlock* fromBb = builder->GetInsertBlock();
                builder->CreateCondBr(isNull, contBb, loadBb);
                builder->SetInsertPoint(loadBb);
                llvm::Value* dataSlot = builder->CreateStructGEP(
                    bodyTy, objPtr, 0, "instanceof.iface_data");
                llvm::Value* data = builder->CreateLoad(
                    ptrTy, dataSlot, "instanceof.obj");
                builder->CreateBr(contBb);
                builder->SetInsertPoint(contBb);
                llvm::PHINode* phi = builder->CreatePHI(ptrTy, 2, "instanceof.lhs");
                phi->addIncoming(llvm::ConstantPointerNull::get(ptrTy), fromBb);
                phi->addIncoming(data, loadBb);
                objPtr = phi;
            }
        } else {
            children[0]->generateCode(module);
        }

        llvm::Value* matchBit;
        if (wantBounded && objPtr && objPtr->getType()->isPointerTy()) {
            llvm::Function* fn =
                module->getRuntimeFunction("__cajeta_instanceof_bounded");
            if (fn) {
                llvm::Value* baseStr = builder->CreateGlobalString(
                    boundBaseCanon, "instanceof.base");
                llvm::Value* boundStr = builder->CreateGlobalString(
                    boundCanon, "instanceof.bound");
                llvm::Value* idx = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), boundArgIdx);
                llvm::Value* r = builder->CreateCall(
                    fn, {objPtr, baseStr, idx, boundStr}, "instanceof.bmatch");
                matchBit = builder->CreateICmpNE(
                    r, llvm::ConstantInt::get(r->getType(), 0));
            } else {
                matchBit = llvm::ConstantInt::getFalse(i1);
            }
        } else if (wantRuntime && objPtr && objPtr->getType()->isPointerTy()) {
            llvm::Function* fn =
                module->getRuntimeFunction("__cajeta_instanceof_named");
            if (fn) {
                llvm::Value* namePtr = builder->CreateGlobalString(
                    targetCanon, "instanceof.target");
                llvm::Value* r = builder->CreateCall(
                    fn, {objPtr, namePtr}, "instanceof.match");
                matchBit = builder->CreateICmpNE(
                    r, llvm::ConstantInt::get(r->getType(), 0));
            } else {
                matchBit = (lhsCanon == targetCanon)
                    ? llvm::ConstantInt::getTrue(i1)
                    : llvm::ConstantInt::getFalse(i1);
            }
        } else {
            bool isMatch = lhsType && (lhsCanon == targetCanon);
            matchBit = isMatch ? llvm::ConstantInt::getTrue(i1)
                               : llvm::ConstantInt::getFalse(i1);
        }

        // The binding is representation-identical because cajeta monomorphizes: bind the
        // object on a match and null otherwise, so a use outside the matched region NPEs.
        // It aliases an existing object, so it is a borrow and never double-frees.
        if (!pattern.empty() && type && objPtr && objPtr->getType()->isPointerTy()) {
            llvm::PointerType* ptrTy =
                llvm::PointerType::get(*module->getLlvmContext(), 0);
            llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            llvm::Value* bound =
                builder->CreateSelect(matchBit, objPtr, nullPtr, "bind.val");
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entry(&parentFn->getEntryBlock(),
                                    parentFn->getEntryBlock().begin());
            llvm::AllocaInst* slot = entry.CreateAlloca(ptrTy, nullptr, pattern);
            builder->CreateStore(bound, slot);
            auto scope = module->getScopeStack().peek();
            if (scope) {
                auto field = make_shared<HeapField>(module, pattern, type);
                field->setAllocation(slot);
                scope->putField(field);
            }
        }

        return matchBit;
    }

    // Resolves `this<Base>` / `super<Base>` against `here`'s transitive ancestors,
    // matching canonical or short name. Throws CAJETA_ERROR_NOT_AN_ANCESTOR when the
    // name is not a reachable ancestor.
    static CajetaClassPtr resolveBracketedAncestor(
            const std::string& name, CajetaClassPtr here) {
        if (!here) return nullptr;
        std::function<CajetaClassPtr(CajetaClassPtr)> walk =
            [&](CajetaClassPtr c) -> CajetaClassPtr {
                if (!c) return nullptr;
                auto qn = c->getQName();
                if (qn) {
                    if (qn->toCanonical() == name) return c;
                    if (qn->getTypeName() == name) return c;
                }
                for (auto& sup : c->getSuperClasses()) {
                    if (auto match = walk(sup)) return match;
                }
                return nullptr;
            };
        // A class is not its own ancestor here: `this<Self>` is a no-op and a code smell.
        for (auto& sup : here->getSuperClasses()) {
            if (auto match = walk(sup)) return match;
        }
        throw Exception(
            "'" + name + "' is not an ancestor of '"
            + here->getQName()->toCanonical()
            + "'; the parent-view selector (this<" + name
            + "> / super<" + name + ">) requires a real ancestor",
            "CAJETA_ERROR_NOT_AN_ANCESTOR");
    }

    // `this` types as the class on the structure stack; `this<Base>` types as the named
    // ancestor, so DotExpression and MethodCallExpression see a Base-typed receiver.
    void ThisExpression::resolveTypes(CajetaModulePtr module) {
        if (module->getStructureStack().empty()) return;
        auto here = std::dynamic_pointer_cast<CajetaClass>(
            module->getStructureStack().back());
        if (!here) {
            resolvedType = module->getStructureStack().back();
            return;
        }
        if (chosenAncestorName.empty()) {
            resolvedType = here;
            return;
        }
        resolvedType = resolveBracketedAncestor(chosenAncestorName, here);
    }

    // Returns the `this` parameter's alloca as an l-value. `this<Base>` instead loads it
    // and adds Base's sub-object offset, returning that r-value — DotExpression detects
    // the load-through and GEPs from there.
    llvm::Value* ThisExpression::generateCode(CajetaModulePtr module) {
        // Method::generateCode registers a ParameterField named `this` for every non-static
        // method; its alloca is the l-value returned here.
        FieldPtr thisField = module->getScopeStack().peek()->getField("this");
        if (!thisField) return nullptr;
        auto alloca = thisField->getOrCreateAllocation();
        if (chosenAncestorName.empty()) {
            // In an ENUM body `this` is the i32 ordinal, not an object address, so its type
            // comes from the method's declared `this` formal.
            if (MethodPtr cur = module->getCurrentMethod()) {
                auto formals = cur->getParameterList();
                if (!formals.empty() && formals.front()
                        && formals.front()->getName() == "this"
                        && formals.front()->getType()
                        && formals.front()->getType()->getLlvmType()
                        && !formals.front()->getType()->getLlvmType()->isPointerTy()) {
                    resolvedType = formals.front()->getType();
                }
            }
            return static_cast<llvm::Value*>(alloca);
        }
        if (!resolvedType) resolveTypes(module);
        if (module->getStructureStack().empty()) return alloca;
        auto here = std::dynamic_pointer_cast<CajetaClass>(
            module->getStructureStack().back());
        if (!here) return alloca;
        auto ancestor = std::dynamic_pointer_cast<CajetaClass>(resolvedType);
        if (!ancestor) return alloca;
        auto* builder = module->getBuilder();
        llvm::Value* loaded = builder->CreateLoad(
            alloca->getAllocatedType(), alloca);
        return CajetaClass::adjustForUpcast(module, loaded, here, ancestor);
    }

    // Plain `super` resolves to the first declared parent; `super<Base>` resolves to the
    // named ancestor, validated against the transitive parent set. The instance pointer
    // is `this` either way — only the dispatch target differs.
    void SuperExpression::resolveTypes(CajetaModulePtr module) {
        if (module->getStructureStack().empty()) {
            throw Exception(
                "`super` used outside of a class context",
                "CAJETA_ERROR_SUPER_OUTSIDE_CLASS");
        }
        auto here = std::dynamic_pointer_cast<CajetaClass>(
            module->getStructureStack().back());
        if (!here) {
            throw Exception(
                "`super` used outside of a class context",
                "CAJETA_ERROR_SUPER_OUTSIDE_CLASS");
        }
        auto& supers = here->getSuperClasses();
        if (supers.empty()) {
            throw Exception(
                "`super` used in class '" + here->getQName()->toCanonical()
                + "' which has no declared superclass",
                "CAJETA_ERROR_SUPER_NO_PARENT");
        }
        if (chosenAncestorName.empty()) {
            resolvedType = supers.front();
            return;
        }
        resolvedType = resolveBracketedAncestor(chosenAncestorName, here);
    }

    // Returns the raw `this` alloca for plain `super` (invokeMethod adjusts the pointer
    // later); `super<Base>` returns a pointer already adjusted to Base's sub-object,
    // which the receiving call force-directs into Base's method.
    llvm::Value* SuperExpression::generateCode(CajetaModulePtr module) {
        auto scope = module->getScopeStack().peek();
        FieldPtr thisField = scope ? scope->getField("this") : nullptr;
        if (!thisField) {
            throw Exception(
                "`super` used in a static context with no `this`",
                "CAJETA_ERROR_SUPER_IN_STATIC");
        }
        auto alloca = thisField->getOrCreateAllocation();
        if (chosenAncestorName.empty()) {
            return static_cast<llvm::Value*>(alloca);
        }
        if (!resolvedType) resolveTypes(module);
        if (module->getStructureStack().empty()) return alloca;
        auto here = std::dynamic_pointer_cast<CajetaClass>(
            module->getStructureStack().back());
        if (!here) return alloca;
        auto ancestor = std::dynamic_pointer_cast<CajetaClass>(resolvedType);
        if (!ancestor) return alloca;
        auto* builder = module->getBuilder();
        llvm::Value* loaded = builder->CreateLoad(
            alloca->getAllocatedType(), alloca);
        return CajetaClass::adjustForUpcast(module, loaded, here, ancestor);
    }

    // `await` unwraps Task<T> to T; an inner that already resolved to T — a bare async
    // call, or a sync pass-through — makes await the identity. The strict
    // `await only on Task<T>` check waits on the async return-type rewrite.
    void AwaitExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) return;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return;
        auto innerType = inner->getResolvedType();
        if (auto task = dynamic_pointer_cast<CajetaTask>(innerType)) {
            resolvedType = task->getElementType();
        } else {
            resolvedType = innerType;
        }
    }

    // Waits on the task's done flag, then re-raises a stored exception or loads
    // `.value`. A non-Task inner is loaded through and returned unchanged.
    llvm::Value* AwaitExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return nullptr;
        llvm::Value* v = inner->generateCode(module);
        if (!v) return nullptr;
        // Re-resolve the inner at codegen time: a local Task declared later in the method
        // was not in scope when the pre-pass ran, and the cast below would miss.
        if (!inner->getResolvedType()) {
            inner->resolveTypes(module);
        }
        auto innerType = inner->getResolvedType();
        auto task = dynamic_pointer_cast<CajetaTask>(innerType);
        if (!task) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                return module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
            }
            return v;
        }
        auto* builder = module->getBuilder();
        auto* ctx = module->getLlvmContext();
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
            v = builder->CreateLoad(llvm::PointerType::get(*ctx, 0), a);
        }
        llvm::Value* doneSlot = builder->CreateStructGEP(
            task->getLlvmType(), v, CajetaTask::DONE_FIELD_INDEX, "task_done_slot");
        if (llvm::Function* waitFn = module->getRuntimeFunction("__cajeta_task_wait")) {
            builder->CreateCall(waitFn, {doneSlot});
        }
        llvm::Type* ptrTy = llvm::PointerType::get(*ctx, 0);
        llvm::Value* excSlot = builder->CreateStructGEP(
            task->getLlvmType(), v, CajetaTask::EXCEPTION_FIELD_INDEX,
            "task_exception_slot");
        llvm::Value* excPtr = builder->CreateLoad(ptrTy, excSlot, "task_exception");
        llvm::Value* hasExc = builder->CreateICmpNE(excPtr,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
            "await_threw");
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* rethrowBB = llvm::BasicBlock::Create(*ctx,
            "await_rethrow", parentFn);
        llvm::BasicBlock* normalBB = llvm::BasicBlock::Create(*ctx,
            "await_normal", parentFn);
        builder->CreateCondBr(hasExc, rethrowBB, normalBB);

        builder->SetInsertPoint(rethrowBB);
        // Clear the exception slot BEFORE throwing, or the surrounding scope walk re-raises
        // an exception the user has already caught.
        builder->CreateStore(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
            excSlot);
        if (llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw")) {
            builder->CreateCall(throwFn, {excPtr});
        }
        builder->CreateUnreachable();

        builder->SetInsertPoint(normalBB);
        llvm::Value* valueSlot = builder->CreateStructGEP(
            task->getLlvmType(), v, CajetaTask::VALUE_FIELD_INDEX, "task_value");
        return builder->CreateLoad(
            task->getLlvmType()->getStructElementType(CajetaTask::VALUE_FIELD_INDEX),
            valueSlot);
    }

    // `spawn call()` types as Task<T> over the inner call's T, keeping an existing
    // wrapper when the inner already returns a Task rather than double-boxing.
    void SpawnExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) return;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return;
        auto innerType = inner->getResolvedType();
        if (auto task = dynamic_pointer_cast<CajetaTask>(innerType)) {
            resolvedType = task;
        } else if (innerType) {
            resolvedType = CajetaTask::getOrCreate(module, innerType);
        }
    }

    // Lowers `spawn f(args)`: the arguments are evaluated HERE, so their side effects
    // stay on this thread, then packed into a heap ctx struct with the Task, and a
    // synthesized trampoline dispatches the bare call or the closure on the worker.
    llvm::Value* SpawnExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return nullptr;
        auto innerCall = dynamic_pointer_cast<MethodCallExpression>(inner);
        if (!innerCall) {
            throw Exception(
                "spawn currently only supports a method-call expression as "
                "its operand",
                "CAJETA_ERROR_ASYNC_R3A");
        }
        // `spawn obj.method()` is deferred: the receiver would have to be captured into
        // the ctx struct and dispatched through the class's vtable.
        if (!innerCall->getChildren().empty()) {
            throw Exception(
                "spawn currently doesn't support instance-method calls; use "
                "a bare class-method invocation",
                "CAJETA_ERROR_ASYNC_R3A");
        }

        // Detect spawn-of-lambda BEFORE arg evaluation, so the closure value is captured at
        // the spawn site like every other argument.
        CajetaFunctionTypePtr lambdaFnType;
        FieldPtr lambdaClosureField;
        if (!module->getScopeStack().isEmpty()) {
            auto scope = module->getScopeStack().peek();
            if (scope) {
                FieldPtr f = scope->getField(innerCall->getMethodCallName());
                if (f) {
                    if (auto ft = dynamic_pointer_cast<CajetaFunctionType>(
                            f->getType())) {
                        lambdaFnType = ft;
                        lambdaClosureField = f;
                    }
                }
            }
        }

        // Spawn-of-lambda takes only the heap-ownership and primitive-return shapes: an
        // sret result slot would have to outlive the worker's trampoline frame.
        if (lambdaFnType && lambdaFnType->usesSret()) {
            throw Exception(
                "spawn-of-lambda v1 supports only heap-ownership return "
                "shape ((P) -> #R) or primitive return; sret value-return "
                "closures would need their result slot to outlive the "
                "worker frame",
                "CAJETA_ERROR_ASYNC_SPAWN_LAMBDA_SRET");
        }

        auto* outerBuilder = module->getBuilder();
        auto& llvmCtx = *module->getLlvmContext();
        auto* lmod = module->emitTargetLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);

        vector<llvm::Value*> capturedArgs;
        vector<CajetaTypePtr> capturedArgTypes;

        if (lambdaFnType && lambdaClosureField) {
            llvm::AllocaInst* slot =
                lambdaClosureField->getOrCreateAllocation();
            llvm::Value* closurePtr = outerBuilder->CreateLoad(
                ptrTy, slot, "spawn_closure_ptr");
            capturedArgs.push_back(closurePtr);
            capturedArgTypes.push_back(lambdaFnType);
        }

        for (auto& param : innerCall->getParameters()) {
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            llvm::Value* v = param.expression->generateCode(module);
            if (!v) return nullptr;
            // Coerce args to r-values: an element or field argument yields a SLOT pointer, and
            // the worker would then mutate the slot rather than the object.
            auto exprAst = dynamic_pointer_cast<Expression>(param.expression);
            v = loadIfLValue(module, v, exprAst);
            CajetaTypePtr t = param.expression->getResolvedType();
            if (!t) t = CajetaType::of(v);
            capturedArgs.push_back(v);
            capturedArgTypes.push_back(t);
        }

        // `spawn f(#owned)` moves the value into the fiber, so the spawner must relinquish
        // it here — otherwise its scope exit frees what the running fiber now owns.
        // Mirrors the caller-side deactivation an ordinary call emits.
        {
            auto scope = module->getScopeStack().isEmpty()
                ? nullptr : module->getScopeStack().peek();
            if (scope) {
                for (auto& param : innerCall->getParameters()) {
                    if (!param.callerTransferred) continue;
                    auto idExpr = dynamic_pointer_cast<IdentifierExpression>(
                        param.expression);
                    if (!idExpr) continue;
                    FieldPtr field = scope->getField(idExpr->getTextValue());
                    if (!field) continue;
                    if (llvm::Value* entry = field->getDropEntry()) {
                        if (llvm::Function* mark = module->getRuntimeFunction(
                                "__cajeta_drop_mark_inactive")) {
                            outerBuilder->CreateCall(mark, {entry});
                        }
                    }
                }
            }
        }

        // Capture the outer block AFTER arg evaluation: an arg may have emitted its own
        // blocks (a bounds-check split), and resuming on the pre-args block would leave
        // the arg's ok-branch without a terminator.
        llvm::BasicBlock* outerInsertBlock = outerBuilder->GetInsertBlock();

        // Context struct `{ ptr task, arg0, arg1, ... }` — an anonymous literal struct, so
        // LLVM unifies the spawn-site stores with the trampoline-side loads.
        vector<llvm::Type*> ctxFieldTypes = {ptrTy};
        for (auto* v : capturedArgs) {
            ctxFieldTypes.push_back(v->getType());
        }
        llvm::StructType* ctxStructTy =
            llvm::StructType::get(llvmCtx, ctxFieldTypes);

        if (module->getStructureStack().empty()) return nullptr;
        CajetaClassPtr targetClass = module->getStructureStack().back();

        static thread_local uint64_t trampolineCounter = 0;  // per-thread (threadsafe U4)
        string trampName = string("__cajeta_spawn_trampoline_")
            + std::to_string(trampolineCounter++);
        llvm::FunctionType* trampTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), {ptrTy}, false);
        // Internal, like lambdas: the counter-numbered name collides across a cached and a
        // fresh module under incremental builds, and only its address is ever used.
        llvm::Function* trampFn = llvm::Function::Create(
            trampTy, llvm::Function::InternalLinkage, trampName, lmod);
        llvm::BasicBlock* trampEntry = llvm::BasicBlock::Create(
            llvmCtx, "entry", trampFn);
        outerBuilder->SetInsertPoint(trampEntry);

        llvm::Value* ctxParam = trampFn->arg_begin();
        llvm::Value* taskSlot = outerBuilder->CreateStructGEP(
            ctxStructTy, ctxParam, 0, "ctx_task_slot");
        llvm::Value* taskPtr = outerBuilder->CreateLoad(
            ptrTy, taskSlot, "task_ptr");

        // The inner call runs inside a try/catch so a throw lands on the Task's exception
        // slot instead of escaping the fiber and crashing the carrier.
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
        llvm::Function* excPushFn = module->getRuntimeFunction("__cajeta_exc_push");
        llvm::Function* excPopFn  = module->getRuntimeFunction("__cajeta_exc_pop");
        llvm::Function* getThrownFn = module->getRuntimeFunction("__cajeta_get_thrown");

        // 512 bytes covers jmp_buf + prev + thrown_value on every supported target.
        // 16-byte aligned because MSVCRT's _setjmp stores XMM registers into the
        // _JUMP_BUFFER with aligned stores (ExcFrameSetjmp.h).
        constexpr unsigned frameBytes = 512;
        llvm::IRBuilder<> trampEntryBuilder(trampEntry, trampEntry->begin());
        llvm::AllocaInst* trampFrame = trampEntryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, frameBytes), nullptr, "spawn_exc_frame");
        trampFrame->setAlignment(llvm::Align(16));

        llvm::BasicBlock* trampTryBB = llvm::BasicBlock::Create(
            llvmCtx, "tramp_try", trampFn);
        llvm::BasicBlock* trampCatchBB = llvm::BasicBlock::Create(
            llvmCtx, "tramp_catch", trampFn);
        llvm::BasicBlock* trampFinishBB = llvm::BasicBlock::Create(
            llvmCtx, "tramp_finish", trampFn);

        if (excPushFn) {
            outerBuilder->CreateCall(excPushFn, {trampFrame});
        }
        llvm::Value* sjResult = emitExcFrameSetjmp(*outerBuilder, trampFrame);
        llvm::Value* threwInTramp = outerBuilder->CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        outerBuilder->CreateCondBr(threwInTramp, trampCatchBB, trampTryBB);

        // --- try body: dispatch the inner call + capture result ---
        outerBuilder->SetInsertPoint(trampTryBB);
        llvm::Value* innerResult = nullptr;
        CajetaTypePtr innerType;
        if (lambdaFnType) {
            llvm::Type* closureTy = llvm::StructType::get(
                llvmCtx, {ptrTy, ptrTy, ptrTy});
            llvm::Value* closureSlot = outerBuilder->CreateStructGEP(
                ctxStructTy, ctxParam, 1, "ctx_closure_slot");
            llvm::Value* closurePtr = outerBuilder->CreateLoad(
                ptrTy, closureSlot, "closure_ptr");
            llvm::Value* fnSlot = outerBuilder->CreateStructGEP(
                closureTy, closurePtr, 0, "closure.fn");
            llvm::Value* fnPtr = outerBuilder->CreateLoad(
                ptrTy, fnSlot, "fn_ptr");
            llvm::Value* capSlot = outerBuilder->CreateStructGEP(
                closureTy, closurePtr, 1, "closure.captures");
            llvm::Value* captures = outerBuilder->CreateLoad(
                ptrTy, capSlot, "captures_ptr");
            // The closure ABI puts captures at position 0, then the user args.
            llvm::FunctionType* sig = lambdaFnType->getLlvmFunctionType();
            vector<llvm::Value*> indirectArgs;
            indirectArgs.push_back(captures);
            // User args live at ctx[2..] (ctx[0] task, ctx[1] closure).
            for (size_t i = 1; i < capturedArgs.size(); ++i) {
                llvm::Value* slot = outerBuilder->CreateStructGEP(
                    ctxStructTy, ctxParam, (unsigned)(i + 1),
                    string("ctx_arg") + std::to_string(i - 1));
                llvm::Value* loaded = outerBuilder->CreateLoad(
                    capturedArgs[i]->getType(), slot,
                    string("arg") + std::to_string(i - 1));
                size_t sigIdx = i;  // 0 is captures, then user args
                if (sig && sigIdx < sig->getNumParams() && loaded
                        && loaded->getType() != sig->getParamType(sigIdx)) {
                    llvm::Type* expected = sig->getParamType(sigIdx);
                    if (expected->isIntegerTy() && loaded->getType()->isIntegerTy()) {
                        loaded = outerBuilder->CreateIntCast(loaded, expected, true);
                    } else if (expected->isFloatingPointTy()
                            && loaded->getType()->isFloatingPointTy()) {
                        loaded = outerBuilder->CreateFPCast(loaded, expected);
                    }
                }
                indirectArgs.push_back(loaded);
            }
            innerResult = outerBuilder->CreateCall(sig, fnPtr, indirectArgs);
            innerType = lambdaFnType->getReturnType();
        } else {
            vector<ParameterEntry> entries;
            for (size_t i = 0; i < capturedArgs.size(); ++i) {
                llvm::Value* slot = outerBuilder->CreateStructGEP(
                    ctxStructTy, ctxParam, (unsigned)(i + 1),
                    string("ctx_arg") + std::to_string(i));
                llvm::Value* loaded = outerBuilder->CreateLoad(
                    capturedArgs[i]->getType(), slot,
                    string("arg") + std::to_string(i));
                entries.push_back(ParameterEntry(capturedArgTypes[i],
                    /*label=*/string(), loaded));
            }
            string methodNameCopy = innerCall->getMethodCallName();
            // Transfer word: bit i is set for each `#`-transferred parameter i (a bare static
            // call means arg index == formal index), so the worker's `#` formal is armed to
            // drop what the spawn site has already handed over.
            int64_t twBits = 0;
            {
                auto& sparams = innerCall->getParameters();
                for (size_t pi = 0; pi < sparams.size() && pi < 64; ++pi) {
                    if (sparams[pi].callerTransferred) {
                        twBits |= ((int64_t) 1 << pi);
                    }
                }
            }
            llvm::Value* spawnTransferWord =
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx),
                                       (uint64_t) twBits);
            // Pass `module` as the caller module so the call is emitted on the trampoline's
            // builder: a stdlib-template worker's own emit module points at an unrelated
            // function, and the call would land in it.
            innerResult = targetClass->invokeMethod(
                methodNameCopy, entries, /*isConstructor=*/false,
                /*thisValue=*/nullptr, /*callerModule=*/module,
                /*forceDirectCall=*/false, /*explicitMethodTypeArgs=*/{},
                /*sretTarget=*/nullptr, /*transferWord=*/spawnTransferWord);
            if (innerResult) {
                innerType = CajetaType::of(innerResult);
                // A method returning `#SomeClass` lowers to a bare `ptr`, so CajetaType::of cannot
                // recover the class — take the DECLARED return type from the receiver instead.
                if (!innerType) {
                    if (MethodPtr m = targetClass->resolveMethod(
                            methodNameCopy, entries, /*isConstructor=*/false,
                            /*floatingParams=*/false)) {
                        innerType = m->getReturnType();
                    }
                }
            }
        }
        if (!innerResult || !innerType) {
            // The trampoline shell is already emitted, so returning null would leave tramp_try
            // unterminated; throw a clear diagnostic instead.
            outerBuilder->SetInsertPoint(outerInsertBlock);
            throw Exception(
                "spawn target `" + innerCall->getMethodCallName()
                + "` could not be resolved to a callable method",
                "CAJETA_ERROR_ASYNC_SPAWN_UNRESOLVED");
        }
        auto task = CajetaTask::getOrCreate(module, innerType);
        llvm::Type* taskTy = task->getLlvmType();
        // LLVM forbids storing a void value; Task<void> leaves its value slot at zero.
        if (!innerResult->getType()->isVoidTy()) {
            llvm::Value* trampValueSlot = outerBuilder->CreateStructGEP(
                taskTy, taskPtr, CajetaTask::VALUE_FIELD_INDEX,
                "task_value_slot");
            outerBuilder->CreateStore(innerResult, trampValueSlot);
        }
        if (excPopFn) outerBuilder->CreateCall(excPopFn, {});
        outerBuilder->CreateBr(trampFinishBB);

        // --- catch: classify, then stash recoverable on task->exception ---
        outerBuilder->SetInsertPoint(trampCatchBB);
        llvm::Value* thrownPtr = getThrownFn
            ? outerBuilder->CreateCall(getThrownFn, {})
            : (llvm::Value*) llvm::ConstantPointerNull::get(ptrTy);
        if (excPopFn) outerBuilder->CreateCall(excPopFn, {});
        // An Unrecoverable throw aborts inside the runtime helper; a Recoverable one is
        // stored on the Task slot for await to re-raise.
        if (llvm::Function* fhFn = module->getRuntimeFunction(
                "__cajeta_fiber_handle_throw")) {
            outerBuilder->CreateCall(fhFn, {thrownPtr});
        }
        llvm::Value* trampExcSlot = outerBuilder->CreateStructGEP(
            taskTy, taskPtr, CajetaTask::EXCEPTION_FIELD_INDEX,
            "task_exception_slot");
        outerBuilder->CreateStore(thrownPtr, trampExcSlot);
        outerBuilder->CreateBr(trampFinishBB);

        // --- finish: signal done + free ctx (runs on both paths) ---
        outerBuilder->SetInsertPoint(trampFinishBB);
        llvm::Value* trampDoneSlot = outerBuilder->CreateStructGEP(
            taskTy, taskPtr, CajetaTask::DONE_FIELD_INDEX,
            "task_done_slot");
        if (llvm::Function* completeFn = module->getRuntimeFunction(
                "__cajeta_task_complete")) {
            outerBuilder->CreateCall(completeFn, {trampDoneSlot});
        }
        if (llvm::Function* freeFn = module->getRuntimeFunction(
                "__cajeta_free")) {
            outerBuilder->CreateCall(freeFn, {ctxParam});
        }
        outerBuilder->CreateRetVoid();

        outerBuilder->SetInsertPoint(outerInsertBlock);
        resolvedType = task;
        const llvm::DataLayout& dl = lmod->getDataLayout();
        llvm::Constant* taskAllocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(llvmCtx), dl.getTypeAllocSize(taskTy));
        llvm::CallInst* taskInstance = MemoryManager::createMallocInstruction(
            module, taskAllocSize, outerInsertBlock);
        llvm::Value* doneInit = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::DONE_FIELD_INDEX,
            "task_done_init");
        outerBuilder->CreateStore(
            llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(llvmCtx), 0), doneInit);
        // Zero the exception slot: the trampoline writes it only on the catch branch.
        llvm::Value* excInit = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::EXCEPTION_FIELD_INDEX,
            "task_exception_init");
        outerBuilder->CreateStore(
            llvm::ConstantPointerNull::get(ptrTy), excInit);
        llvm::Value* fiberSlot = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::FIBER_FIELD_INDEX,
            "task_fiber_init");
        outerBuilder->CreateStore(
            llvm::ConstantPointerNull::get(ptrTy), fiberSlot);
        // Wire the Task into the drop chain, or the heap struct leaks; its drop function
        // waits for completion before freeing. detach opts out of scope-anchored cleanup.
        if (!detachMode && !discardedMode) {
            // A statement-position spawn is scope-owned instead: a per-site drop entry cannot
            // represent N tasks live from a loop, and it joins at the innermost brace.
            bool debugTags = module->getFlags().sourceTags;
            llvm::Function* dropPush = module->getRuntimeFunction(
                debugTags ? "__cajeta_drop_push_debug" : "__cajeta_drop_push");
            if (dropPush) {
                if (llvm::Function* taskDropFn = task->getOrCreateDropFunction()) {
                    unsigned dropEntryBytes = debugTags ? 40 : 32;
                    llvm::Function* parentFnForDrop =
                        outerBuilder->GetInsertBlock()->getParent();
                    llvm::IRBuilder<> dropEntryBuilder(
                        &parentFnForDrop->getEntryBlock(),
                        parentFnForDrop->getEntryBlock().begin());
                    llvm::Value* dropEntryPtr = dropEntryBuilder.CreateAlloca(
                        llvm::ArrayType::get(i8Ty, dropEntryBytes));
                    if (debugTags) {
                        llvm::Constant* fileConst = module->getOrCreateSourceFileConstant(
                            module->getSourcePath());
                        llvm::Constant* lineConst = llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(llvmCtx), getSourceLine());
                        outerBuilder->CreateCall(dropPush,
                            {dropEntryPtr, taskInstance, taskDropFn, fileConst, lineConst});
                    } else {
                        outerBuilder->CreateCall(dropPush,
                            {dropEntryPtr, taskInstance, taskDropFn});
                    }
                    if (auto m = module->getCurrentMethod()) {
                        m->registerDropEntry(dropEntryPtr);
                    }
                    dropEntry = dropEntryPtr;
                }
            }
        }
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        if (!allocFn) {
            throw Exception(
                "runtime helper __cajeta_alloc not linked — cannot allocate "
                "spawn context",
                "CAJETA_ERROR_RUNTIME");
        }
        uint64_t ctxBytes = dl.getTypeAllocSize(ctxStructTy);
        llvm::Value* ctxInstance = outerBuilder->CreateCall(allocFn, {
            llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(llvmCtx), ctxBytes),
        }, "spawn_ctx");
        llvm::Value* ctxTaskField = outerBuilder->CreateStructGEP(
            ctxStructTy, ctxInstance, 0, "ctx_task_init");
        outerBuilder->CreateStore(taskInstance, ctxTaskField);
        for (size_t i = 0; i < capturedArgs.size(); ++i) {
            llvm::Value* field = outerBuilder->CreateStructGEP(
                ctxStructTy, ctxInstance, (unsigned)(i + 1),
                string("ctx_arg") + std::to_string(i) + "_init");
            outerBuilder->CreateStore(capturedArgs[i], field);
        }
        // Register with the innermost scope so its closing brace waits for the task, and
        // pass the exception and fiber slots so it can re-raise and cancel siblings.
        llvm::Value* fiberRegSlot = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::FIBER_FIELD_INDEX,
            "scope_register_fiber");
        if (!detachMode) {
            llvm::Value* doneRegSlot = outerBuilder->CreateStructGEP(
                taskTy, taskInstance, CajetaTask::DONE_FIELD_INDEX,
                "scope_register_done");
            llvm::Value* excRegSlot = outerBuilder->CreateStructGEP(
                taskTy, taskInstance, CajetaTask::EXCEPTION_FIELD_INDEX,
                "scope_register_exc");
            // --lazy-scope skipped the implicit body frame, so ensure one exists before a bare
            // spawn registers; a no-op when an enclosing scope already pushed one.
            if (module->getFlags().lazyScope) {
                if (auto mth = module->getCurrentMethod()) {
                    if (llvm::AllocaInst* wm = mth->getScopeWatermark()) {
                        if (llvm::Function* ensureFn = module->getRuntimeFunction(
                                "__cajeta_scope_ensure_at")) {
                            llvm::Value* wmVal = outerBuilder->CreateLoad(
                                llvm::PointerType::get(*module->getLlvmContext(), 0),
                                wm);
                            outerBuilder->CreateCall(ensureFn, {wmVal});
                        }
                    }
                }
            }
            if (discardedMode) {
                if (llvm::Function* regFn = module->getRuntimeFunction(
                        "__cajeta_scope_register_owned")) {
                    outerBuilder->CreateCall(regFn,
                        {doneRegSlot, excRegSlot, fiberRegSlot, taskInstance});
                }
            } else if (llvm::Function* regFn = module->getRuntimeFunction(
                    "__cajeta_scope_register")) {
                outerBuilder->CreateCall(regFn,
                    {doneRegSlot, excRegSlot, fiberRegSlot});
            }
        }
        if (llvm::Function* runFn = module->getRuntimeFunction(
                "__cajeta_task_run")) {
            outerBuilder->CreateCall(runFn,
                {ctxInstance, trampFn, fiberRegSlot});
        }
        if (detachMode) return nullptr;
        return taskInstance;
    }

    // `detach` is fire-and-forget, so the expression types as void; the inner is still
    // resolved so its method-call shape is validated.
    void DetachExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (!children.empty()) {
            if (auto inner = dynamic_pointer_cast<Expression>(children[0])) {
                inner->resolveTypes(module);
            }
        }
        resolvedType = CajetaType::of("void");
    }

    // Requires every argument of a detached call to be `#`-transferred, a primitive, or a
    // fresh allocation: the fiber outlives the spawning scope, so a borrow's lifetime
    // cannot be guaranteed. Throws naming the offending argument.
    static void enforceDetachMoveOnlyCaptures(const std::shared_ptr<MethodCallExpression>& innerCall,
                                              const CajetaModulePtr& module) {
        for (auto& param : innerCall->getParameters()) {
            auto expr = param.expression;
            if (!expr) continue;
            // A caller-side `#` reaches here either as the parameter's own flag or as a
            // MoveExpression-wrapped child; both spellings count as a transfer.
            if (param.callerTransferred) continue;
            ownership::TitleShape dSh = ownership::classify(expr, module);
            if (dSh.family == ownership::TitleFamily::Move
                    || (dSh.family == ownership::TitleFamily::Fresh
                        && dSh.answer == ownership::TitleAnswer::Owned)
                    || dSh.answer == ownership::TitleAnswer::Scalar) continue;
            auto t = expr->getResolvedType();
            string detail = t ? t->toCanonical() : string("<unresolved>");
            throw Exception(
                "detach argument must be #-transferred, primitive, or a fresh "
                "`heap T(...)`; got an expression of type '" + detail + "' that "
                "would be captured as a borrow",
                "CAJETA_ERROR_DETACH_BORROW_CAPTURE");
        }
    }

    // Reuses SpawnExpression's lowering in detach mode — same trampoline and fiber
    // enqueue, minus scope_register and drop_push — after checking that every captured
    // argument is move-only. Yields no value.
    llvm::Value* DetachExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return nullptr;
        auto innerCall = dynamic_pointer_cast<MethodCallExpression>(inner);
        if (!innerCall) {
            throw Exception(
                "detach currently only supports a method-call expression as "
                "its operand",
                "CAJETA_ERROR_ASYNC_R3A");
        }
        for (auto& param : innerCall->getParameters()) {
            if (param.expression && !param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
        }
        enforceDetachMoveOnlyCaptures(innerCall, module);
        auto spawn = make_shared<SpawnExpression>(nullptr);
        spawn->addChild(innerCall);
        spawn->setDetachMode(true);
        spawn->resolveTypes(module);
        spawn->generateCode(module);
        return nullptr;
    }
}
