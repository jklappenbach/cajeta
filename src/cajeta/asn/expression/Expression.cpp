//
// Created by James Klappenbach on 3/19/22.
//

#include "Expression.h"
#include "cajeta/compile/CajetaModule.h"
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

namespace cajeta {
    ExpressionPtr Expression::fromContext(CajetaParser::ExpressionContext* ctx) {
        antlr4::Token* token = ctx->getStart();
        ExpressionPtr result = nullptr;
        if (ctx->ASSIGN()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_ASSIGN, token);
        } else if (ctx->COLONCOLON()) {
            // Method reference: `expr::id`, `Type::id`, or `Type::heap`
            // (constructor reference). Check before the identifier form so we
            // don't mis-route the token-bearing ctor form. Detect the LHS:
            //   - typeType form (`Type::id` / `Type::heap`): grammar
            //     captured a typeType in front of the COLONCOLON.
            //   - expression form (`obj::id`): grammar captured an
            //     expression in front.
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
            // DOT-as-binary-op consumes all six suffix forms. Check before methodCall
            // because `obj.foo()` matches both DOT and methodCall — DOT must win.
            if (ctx->SUPER() || ctx->superSuffix()) {
                result = make_shared<UnsupportedExpression>("super call", token);
            } else if (ctx->innerCreator()) {
                result = make_shared<UnsupportedExpression>(
                    "inner-class instantiation (obj.heap Inner())", token);
            } else if (ctx->methodCall()) {
                // `obj.foo(args)` — method invocation on a receiver. The receiver is the
                // lhs expression captured by the children-add loop at the bottom of this
                // function (ctx->expression() returns [lhs] here).
                result = make_shared<MethodCallExpression>(ctx->methodCall(), token);
            } else {
                result = make_shared<DotExpression>(ctx, token);
            }
        } else if (ctx->methodCall()) {
            // Bare standalone call `foo(...)` (no DOT). With-DOT calls are routed by the
            // branch above.
            result = make_shared<MethodCallExpression>(ctx->methodCall(), token);
        } else if (ctx->HEAP()) {
            // Unified-class allocation prefix (docs/specification/lang/UnifiedClasses.md). Phase 2a:
            // both forms codegen.
            // - `heap MyClass(args)` routes through NewExpression (today's
            //   `NEW creator` path; malloc + ctor).
            // - `heap MyClass { ... }` routes through AggregateInitializer-
            //   Expression with stackAlloc=false; malloc + memset + vtable
            //   init (for classes with vtables) + per-field stores.
            if (ctx->creator()) {
                result = make_shared<NewExpression>(ctx->creator(), token);
            } else if (ctx->aggregateInitializer()) {
                auto agg = make_shared<AggregateInitializerExpression>(
                    ctx->aggregateInitializer(), token);
                agg->setStackAlloc(false);
                result = agg;
            }
        } else if (ctx->STACK()) {
            // Unified-class allocation prefix (docs/specification/lang/UnifiedClasses.md). Phase 2a:
            // both forms now codegen.
            // - `stack MyClass { ... }` routes through aggregate-init
            //   (today's bare aggregate-init path; stack-allocated body).
            // - `stack MyClass(args)` routes through NewExpression with
            //   stackAlloc=true; ClassCreatorRest emits an entry-block
            //   alloca + vtable init + ctor invocation instead of malloc.
            if (ctx->aggregateInitializer()) {
                result = make_shared<AggregateInitializerExpression>(
                    ctx->aggregateInitializer(), token);
            } else if (ctx->creator()) {
                auto newExpr = make_shared<NewExpression>(ctx->creator(), token);
                newExpr->setStackAlloc(true);
                result = newExpr;
            }
        } else if (ctx->SHARED()) {
            // `shared` placement — GPU workgroup-shared memory (NV addrspace 3),
            // a sibling of heap/stack (CajetaXPU.md §3.1.2). Device-only: the
            // NVPTX kernel lowerer turns `shared T[N]` into one per-block
            // addrspace(3) global; the host generateCode path rejects it.
            // v1 supports only the array-creation form (`shared T[N]`); the
            // aggregate / class-creator forms parse but are rejected downstream.
            if (ctx->creator()) {
                auto newExpr = make_shared<NewExpression>(ctx->creator(), token);
                newExpr->setSharedAlloc(true);
                result = newExpr;
            } else if (ctx->aggregateInitializer()) {
                auto agg = make_shared<AggregateInitializerExpression>(
                    ctx->aggregateInitializer(), token);
                agg->setStackAlloc(false);
                result = agg;  // host path rejects; v1 has no shared-aggregate
            }
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), ctx->primary() != nullptr);
        } else if (ctx->LPAREN()) {
            // Two expression forms carry a top-level LPAREN:
            //   - Cast:         '(' annotation* typeType ('&' typeType)* ')' expression
            //   - Postfix call: expression '(' parameterList? ')'   (XPU launch)
            // The cast carries a typeType; the postfix call does not. (A bare
            // standalone call `foo(...)` lives inside a MethodCallContext, so
            // its LPAREN never surfaces at this level.)
            if (!ctx->typeType().empty()) {
                // Intersection casts (multiple typeTypes) aren't supported yet; take the first.
                CajetaTypePtr destType = CajetaType::fromContext(ctx->typeType(0), nullptr);
                result = make_shared<CastExpression>(destType, token);
            } else {
                // `<callee>(args)` — the callee expression is attached as
                // children[0] by the child loop at the bottom of this function.
                result = make_shared<CallExpression>(ctx, token);
            }
        } else if (ctx->LBRACK()) {
            result = make_shared<ArrayIndexExpression>(ctx, token);
        // Grammar artifacts (annotation/creator/typeType-alone/RPAREN) are sub-rules that
        // never appear as standalone expressions; their parent expression form has
        // already been matched by a prior branch (NEW, LPAREN cast, etc.).
        } else if (!ctx->BITAND().empty()) {
            result = make_shared<BinaryOpExpression>(BINARY_OP_BITAND, token);
        } else if (ctx->ADD()) {
            // The same ADD/SUB tokens cover both binary and prefix unary forms; the
            // grammar tags unary with `ctx->prefix`.
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
            // L1 (typed params) + L1.5 (bare-identifier params with
            // target-type inference). Parameter forms recognized:
            //   `(T1 a, T2 b) -> expr`          — explicit types
            //   `() -> expr`                    — zero params
            //   `(a, b) -> expr`                — bare names, types from context
            //   `a -> expr`                     — single bare name, type from context
            // The `var`-list form parses but lands on UnsupportedExpression;
            // block bodies likewise stub out until L2.
            auto* lx = ctx->lambdaExpression();
            auto* lp = lx->lambdaParameters();
            std::vector<std::string> names;
            std::vector<CajetaTypePtr> types;
            bool acceptable = false;
            if (lp) {
                if (auto* fpl = lp->formalParameterList()) {
                    for (auto* fp : fpl->formalParameter()) {
                        if (auto p = FormalParameter::fromContext(fp, nullptr)) {
                            names.push_back(p->getName());
                            types.push_back(p->getType());
                        }
                    }
                    acceptable = !names.empty() || fpl->formalParameter().empty();
                } else if (lp->LPAREN() && lp->RPAREN() && !lp->lambdaLVTIList()) {
                    // Parens form. Either empty `()` or bare-identifier list
                    // `(a, b)`. Names captured; types filled in at resolve
                    // time from the lambda's expectedType (set by the
                    // surrounding LocalVariableDeclaration).
                    for (auto* id : lp->identifier()) {
                        names.push_back(id->getText());
                    }
                    acceptable = true;
                } else if (!lp->identifier().empty() && !lp->LPAREN()) {
                    // Single bare identifier `x -> expr`. Same inference
                    // path as the parens-list form.
                    names.push_back(lp->identifier(0)->getText());
                    acceptable = true;
                }
            }
            // Body: expression form (L1/L1.5) or block form (L2-4). The
            // var-list parameter form is still unsupported.
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
            // switch expression — arrow form with single-expression bodies. Anything
            // more elaborate (colon form, yield, guarded patterns) is rejected at
            // codegen with NOT_IMPLEMENTED.
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
                    // Arrow form: the rule body is `blockStatement* statementExpression ;`.
                    // We accept exactly the shape `expression ;` — i.e. a single
                    // ExpressionStatement — and pull out its expression.
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
            // `cond ? then : else`. The grammar matches QUESTION and COLON together; we
            // pick QUESTION as the discriminator. Children populated later by the loop
            // at the bottom of this function as [cond, then, else].
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
            // `#expr` — transfer ownership of expr at the surrounding consumption
            // site (assignment LHS, method-call argument, or return). Wrapped in
            // MoveExpression so consumers can detect it via dynamic_pointer_cast.
            result = make_shared<MoveExpression>(token);
        } else if (ctx->AWAIT()) {
            // `await expr` — unwrap Task<T> to T (sync MVP just reads .value).
            result = make_shared<AwaitExpression>(token);
        } else if (ctx->SPAWN()) {
            // `spawn expr` — run inner call now, return a completed Task<T>.
            result = make_shared<SpawnExpression>(token);
        } else if (ctx->DETACH()) {
            // `detach expr` — run inner call now, discard the resulting Task.
            result = make_shared<DetachExpression>(token);
        } else if (ctx->INSTANCEOF()) {
            // `expr instanceof Type` (plain) or `expr instanceof Type id` (the
            // pattern form, which binds `id : Type` in the matched region —
            // reified-capture §4). In the pattern form the type + identifier
            // live inside `pattern` (the top-level typeType list is empty).
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

        if (result) {
            if (!ctx->expression().empty()) {
                for (auto childContext: ctx->expression()) {
                    result->addChild(Expression::fromContext(childContext));
                }
            }
        }
        return result;
    }

    ArrayIndexExpression::ArrayIndexExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(
        token) {

    }

    ArrayLiteralExpression::ArrayLiteralExpression(
        CajetaParser::ArrayLiteralContext* ctx, antlr4::Token* token)
        : Expression(token) {
        if (auto* list = ctx->expressionList()) {
            for (auto* e : list->expression()) {
                auto elem = Expression::fromContext(e);
                elements.push_back(elem);
                // Mirror into children so generic AST walks (free-variable
                // scans, type resolution) reach the elements too.
                addChild(elem);
            }
        }
    }

    llvm::Value* ArrayLiteralExpression::generateCode(CajetaModulePtr module) {
        // A list literal has no standalone value lowering yet — it exists to
        // carry XPU launch dimensions, which the launch path reads element-by-
        // element off the AST (see CallExpression / the launch-site lowering).
        // Used anywhere else, reject clearly rather than emit null IR.
        throw Exception(
            "array literal expression (only supported today as XPU launch "
            "grid/block dimensions)",
            "CAJETA_ERROR_NOT_IMPLEMENTED");
    }

    // Resolve a value-of-slot for sites that consumed an l-value (alloca or ArrayIndex
    // GEP). Returns the loaded value when `v` is such an address; otherwise returns
    // `v` unchanged (constants, intermediates). `valueType` is the Cajeta type of the
    // element (used to pick the load size — reference types load as `ptr`, primitives
    // load as their own LLVM type).
    static llvm::Value* readSlot(CajetaModulePtr module, llvm::Value* v,
                                  CajetaTypePtr valueType) {
        if (!v) return v;
        auto* builder = module->getBuilder();
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
            return builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (!v->getType()->isPointerTy() || !valueType) return v;
        llvm::Type* loadTy;
        if (dynamic_pointer_cast<CajetaArray>(valueType)) {
            // Slot stores a `ptr` to the inner header (or to any reference).
            loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
        } else if (valueType->getTypeFlags() & STRUCT_FLAG) {
            loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
        } else {
            loadTy = valueType->getLlvmType();
        }
        if (!loadTy) return v;
        return builder->CreateLoad(loadTy, v);
    }

    void ArrayIndexExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        // One level of indexing unwraps one CajetaArray layer. `int[][]` indexed once
        // yields `int[]`; indexed again yields `int`. For a non-array receiver
        // (a class with operator[] overload), the resolved type is the
        // operator's return type — look up the method to find out.
        if (!children.empty()) {
            if (auto exprChild = dynamic_pointer_cast<Expression>(children[0])) {
                CajetaTypePtr lhsType = exprChild->getResolvedType();
                if (auto arr = dynamic_pointer_cast<CajetaArray>(lhsType)) {
                    resolvedType = arr->getElementType();
                } else if (auto vecT = dynamic_pointer_cast<CajetaVector>(lhsType)) {
                    resolvedType = vecT->getElementType();
                } else if (auto matT = dynamic_pointer_cast<CajetaMatrix>(lhsType)) {
                    // Matrix m[r] selects a row -> Vector<T, C> (B1). m[r][c] then
                    // resolves via the CajetaVector branch above on this row type.
                    resolvedType = CajetaVector::getOrCreate(
                        module, matT->getElementType(), matT->getCols());
                } else if (auto klass = dynamic_pointer_cast<CajetaClass>(lhsType)) {
                    // Try to find operator[] on the class. parameterList
                    // computation needs the index expression's resolved
                    // type; resolve children[1] first so the lookup has
                    // a real parameter type.
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

    // Evaluate a compile-time-constant integer index from the AST: an integer
    // literal, or a unary +/- applied to one. Returns true + the value when
    // constant-foldable. Used for the static inline-array bounds check, which
    // must catch `f[-1]` — under overflow checking the negation lowers to a
    // checked-sub intrinsic, so the post-codegen LLVM value is not a folded
    // ConstantInt.
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

    llvm::Value* ArrayIndexExpression::generateCode(CajetaModulePtr module) {
        // Each ArrayIndexExpression handles exactly one index level. children[0] is the
        // array expression (a ptr to a header `{ i64 size, [0 x T] data }`); children[1]
        // is the index expression. For `arr[i][j]` the AST is nested:
        //   (ArrayIndex (ArrayIndex arr i) j)
        // matching the Java grammar `expression '[' expression ']'` recursively.
        if (children.size() < 2) {
            return nullptr;
        }
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);

        // Vector index read: v[i] -> extractelement (dynamic index ok). The
        // assignment form v[i] = x is handled by BinaryOpExpression's
        // assignment path (it needs the vector's slot, not this value).
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
                // Wrap in a slot so consumers (which loadIfLValue an
                // ArrayIndex result as a pointer) read the element correctly —
                // mirrors the operator[] GET path below.
                llvm::AllocaInst* slot = builder->CreateAlloca(
                    elt->getType(), nullptr, "vec.idx.slot");
                builder->CreateStore(elt, slot);
                return slot;
            }
        }

        // Matrix single-index read: m[r] -> the row Vector<T,C> (flat lanes
        // [r*C, r*C+C)). m[r][c] composes — the outer index sees this row's
        // CajetaVector type and the vector branch above extracts lane c. The
        // assignment form m[r][c] = x is handled in BinaryOpExpression's
        // assignment path (it writes into the matrix's slot, not a row temp).
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
                llvm::AllocaInst* slot = builder->CreateAlloca(
                    rowVal->getType(), nullptr, "mat.row.slot");
                builder->CreateStore(rowVal, slot);
                return slot;
            }
        }

        // Operator overload dispatch: if the LHS resolves to a class with
        // an `operator[]` method, route through it instead of the native-
        // array path. Mirrors BinaryOpExpression's operator-method
        // dispatch shape (operator+, operator==, ...). v1 covers GET
        // only — `arr[i]` reading. The assignment form `arr[i] = v` is
        // handled by BinaryOpExpression's assignment path, which still
        // assumes a native-array LHS; supporting operator[]-typed
        // assignment targets is a separate cut.
        auto lhsExprForOp = dynamic_pointer_cast<Expression>(children[0]);
        if (lhsExprForOp) {
            if (!lhsExprForOp->getResolvedType()) {
                lhsExprForOp->resolveTypes(module);
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
                llvm::Value* lhsForOp = children[0]->generateCode(module);
                llvm::Value* idxForOp = children[1]->generateCode(module);
                if (idxForOp && !idxType) {
                    idxType = CajetaType::of(idxForOp);
                }
                if (!idxType) {
                    // Without a usable index type the canonical lookup
                    // crashes; fall through to native-array path.
                    goto fall_through_to_native_array;
                }
                // l-value coercion mirrors BinaryOpExpression: identifier
                // expressions evaluate to allocas holding the heap
                // pointer; we want the loaded value as `this`. EXCEPT a
                // @ValueType receiver, whose slot holds the aggregate INLINE
                // (alloca %ValueType, not alloca ptr) — its `this` is the alloca
                // ADDRESS (instance methods take the receiver by reference even
                // for value types), so loading would pass the aggregate VALUE.
                // Mirrors the DotExpression value-type guard (S2).
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhsForOp)) {
                    bool valueTypeReceiver = lhsClass->isValueType()
                        && !a->getAllocatedType()->isPointerTy();
                    if (!valueTypeReceiver) {
                        lhsForOp = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                }
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(idxForOp)) {
                    idxForOp = builder->CreateLoad(a->getAllocatedType(), a);
                }
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
                    // Wrap the call result in an alloca so consumers
                    // (ReturnStatement, assignment LHS, nested
                    // expressions) see the same "load from this
                    // address" shape they get from the native-array
                    // GEP path. Native ArrayIndex returns a GEP
                    // pointer; the caller loads to read. Without the
                    // wrapper they'd issue a `load i32, i32 <value>`
                    // on the raw value and the verifier rejects it.
                    llvm::AllocaInst* slot = builder->CreateAlloca(
                        callResult->getType(), nullptr, "opidx.slot");
                    builder->CreateStore(callResult, slot);
                    return slot;
                }
            }
        }
        fall_through_to_native_array:;

        // Resolve the array value (the header pointer).
        //   - Local-variable arrays: an alloca holding a `ptr` to the header. Load to get
        //     the header pointer.
        //   - Nested ArrayIndex (`arr[i][j]`): the parent ArrayIndex gave us a slot whose
        //     element is a `ptr` to the inner header — load `ptr` to get that.
        //   - Class-field array (`obj.field`): DotExpression hands back a GEP to the
        //     field slot, which holds a `ptr` to the heap header (per
        //     CajetaClass::generatePrototype's `fieldLayoutType` rule that stores
        //     array fields as pointers, not inline). Load the slot to get the header
        //     pointer, same way the local-variable path does for an alloca.
        //   - Anything else (e.g. method-call returning an array): the value already IS
        //     the header pointer.
        llvm::Value* arrayVal = children[0]->generateCode(module);
        auto lhsExpr = dynamic_pointer_cast<Expression>(children[0]);

        // Fixed-size inline array field (`obj.f[i]` where f is `T[N]`): the
        // field slot GEP from DotExpression ALREADY points at the inline
        // `[N x T]` storage embedded in the object — there is no header
        // pointer to load through. Loading it (as the heap-`T[]` path does)
        // would read the first inline element as a pointer and SIGSEGV.
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

        // Resolve element type from the CajetaArray annotation on the lhs. With opaque
        // pointers the LLVM type alone tells us nothing; we lean on the type resolver
        // having pinned children[0]->resolvedType to the CajetaArray.
        //
        // The pre-pass resolver runs before LocalVariableDeclaration populates the
        // scope, so identifier-based lhses are commonly null at resolveTypes time. We
        // re-run resolveTypes here (now that the scope is populated by codegen) and
        // also publish *our* element type so consumers like ReturnStatement see it.
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

        // TBAA access kind for this array's elements: i8 (byte buffer) is
        // reinterpretation-prone -> alias-all Char; any other element type gets
        // the disjoint array-element tag so the optimizer can hoist enclosing
        // object-field loads across element stores.
        auto tbaaElemKind = [&]() -> CajetaModule::TbaaKind {
            llvm::Type* et = resolvedType ? resolvedType->getLlvmType() : nullptr;
            if (et && et->isIntegerTy(8)) {
                return CajetaModule::TbaaKind::Char;
            }
            return CajetaModule::TbaaKind::ArrayElem;
        };

        // Resolve the index expression. Same l-value-to-r-value coercion
        // needed for class-field indices (`this.data[this.idx]`) — the
        // DotExpression for `this.idx` returns a GEP, not an AllocaInst,
        // and without loadIfLValue the GEP stays a ptr and CreateIntCast
        // sext'd it as a pointer (LLVM verify error: "SExt only operates
        // on integer").
        llvm::Value* idx = children[1]->generateCode(module);
        auto idxAst = dynamic_pointer_cast<Expression>(children[1]);
        idx = loadIfLValue(module, idx, idxAst);
        if (idx->getType() != i64Ty) {
            idx = builder->CreateIntCast(idx, i64Ty, /*isSigned=*/true);
        }

        // Inline fixed-size array field (`T[N]`): address the element directly
        // in the embedded `[N x T]` storage — `&inline[idx]` via GEP {0, idx},
        // no header, no DATA_FIELD indirection. Bounds are against the
        // compile-time constant N.
        if (arrayType->isInlineArray()) {
            int64_t n = arrayType->getFixedLength();
            // Static bounds check: the length N is known at compile time, so a
            // constant index proven out of [0, N) is a compile error (spec
            // 2.1.7) rather than a runtime trap. Evaluate the constant from the
            // AST (catches `f[-1]`, whose checked negation isn't a folded
            // ConstantInt); fall back to a folded LLVM constant for constant
            // expressions the AST walk doesn't cover.
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
            // TBAA: this address feeds an array-element load/store. Byte (i8)
            // buffers are reinterpretation-prone (String/SWAR/memcpy) so they get
            // the alias-all Char tag; other element types get the disjoint
            // array-element tag. See CajetaModule TBAA section.
            module->recordTbaaProvenance(inlineElemPtr, tbaaElemKind());
            return inlineElemPtr;
        }

        // Bounds check (when enabled by the compiler flag and the runtime helper is
        // linked): load the size field and branch to fail if `idx >= size` under
        // unsigned comparison (catches negatives). Disabled via `cajeta --bounds=off`.
        // `--bounds=trap` swaps the abort-helper call for @llvm.trap so the failure
        // surfaces as SIGILL with no message (matches release builds that don't
        // want the helper-symbol footprint).
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

        // Element address: &header->data[idx]. GEP walks ptr -> struct -> data array -> element.
        vector<llvm::Value*> gepIndices = {
            llvm::ConstantInt::get(i64Ty, 0),
            llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
            idx,
        };
        llvm::Value* elemPtr = builder->CreateGEP(headerTy, arrayVal, gepIndices);
        // TBAA: array-element access (i8 buffers -> alias-all Char; else the
        // disjoint array-element tag). See CajetaModule TBAA section.
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
        // P3+ Optional fix — child may be a DotExpression returning a
        // GEP into a class field (e.g. `!this.flag`). loadIfLValue
        // recognizes the field-load shape via the AST's resolvedType
        // and loads the field at the right element type. Without this,
        // PrefixExpression LOGNOT/BITNOT/etc. saw a ptr and produced
        // an ICmpEQ(ptr, i?-zero) → LLVM type mismatch.
        auto exprAst = std::dynamic_pointer_cast<Expression>(child);
        if (exprAst) {
            llvm::Value* loaded = loadIfLValue(module, raw, exprAst);
            // l-value address is `raw` only when it's an alloca slot;
            // for GEP-into-field, the slot isn't writable through the
            // same primitive (the existing increment/decrement uses
            // `addr` which is alloca-only — that's fine; increment
            // through a field is a DotExpression-write concern).
            return {nullptr, loaded};
        }
        return {nullptr, raw};
    }

    // Dispatch a unary operator to a user-defined overload on a class
    // operand. For `++ --` (mutating), the lookup is for an INSTANCE
    // method taking 0 params; for `- + ! ~` (non-mutating), the lookup
    // is for a STATIC method taking 1 param. Returns the call's result
    // when dispatched, nullptr to fall through to the primitive path
    // (docs/OperatorOverloading.md §§3-4 + §8 dispatch table).
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
            // x++ / x-- → x.operator++() (no explicit params; the
            // receiver IS the target). Borrow checker enforces a
            // mutable borrow at the call site.
            if (!opClass->resolveMethod(opName, entries,
                    /*isConstructor=*/false, /*floatingParams=*/false)) {
                return nullptr;
            }
            return opClass->invokeMethod(opName, entries,
                /*isConstructor=*/false,
                /*thisInstance=*/operandVal,
                /*callerModule=*/module);
        }
        // -x / +x / !x / ~x → T.operator-(x) (static, one param).
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

    void PrefixExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) return;
        auto operand = dynamic_pointer_cast<Expression>(children[0]);
        if (!operand) return;
        CajetaTypePtr operandType = operand->getResolvedType();
        if (!operandType) return;
        // !x → boolean; +/-/~/++/-- preserve the operand's primitive type.
        if (op == PREFIX_OP_LOGNOT) {
            resolvedType = CajetaType::of("boolean");
        } else {
            resolvedType = operandType;
        }
    }

    llvm::Value* PrefixExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto* builder = module->getBuilder();
        auto [addr, val] = loadOperand(module, children[0]);
        if (!val) return nullptr;
        llvm::Type* ty = val->getType();

        // Class-type operator dispatch — runs BEFORE the primitive
        // switch so a class with `static T operator-(T)` or instance
        // `void operator++()` takes priority over the (nonsensical)
        // primitive lowering on a class pointer. Primitive operands
        // fall through with opSym == nullptr or the class-resolve miss.
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
                // Signed-overflow check: -INT_MIN can't fit in a signed
                // int of the same width. Equivalent to ssub(0, x), so
                // route through llvm.ssub.with.overflow when the
                // operand's AST type is signed and overflowChecks=On.
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
                // !x ≡ (x == 0). Produces i1.
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
                // Same signed-overflow gate as unary -: trap on
                // INT_MAX++ / INT_MIN--. Unsigned wraps modularly.
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

    // Erased base of a canonical template name (defined later in this TU);
    // forward-declared so the capture-cast guard below can use it.
    static std::string erasedBaseOf(const std::string& canonical);

    // Detect a class-bounded-wildcard capture target `Base<? extends Bound>`
    // (reified-capture-spec.md §5). Returns true and fills baseCanon (the erased
    // container base, e.g. "test.Box"), argIndex (the bounded arg's position),
    // and boundCanon (the bound's canonical name, e.g. "test.Animal") iff `type`
    // is a class instantiation whose sole type argument is a bounded-EXTENDS
    // wildcard. Single-arg only for now — multi-arg / mixed-arg targets fall
    // through to the existing concrete-target handling. Defined later in this TU.
    static bool boundedWildcardTarget(const CajetaTypePtr& type,
                                      std::string& baseCanon, int& argIndex,
                                      std::string& boundCanon);

    // Materialize a compile-time std::string as a static cajeta.lang.String
    // (view-mode) instance, returning the instance global. Mirrors the
    // LITERAL_TYPE_TEXT_BLOCK path in LiteralExpression so synthesized codegen
    // sites (the throwing capture cast) can build a String message without
    // re-running the parser. Falls back to a bare C-string global before class
    // String is registered (bootstrap).
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
        auto* dataArrTy = llvm::ArrayType::get(i8Ty, (uint64_t) len + 1);
        auto* dataInit = llvm::ConstantDataArray::getString(ctx, text, true);
        auto* arrStructTy = llvm::StructType::get(ctx,
            llvm::ArrayRef<llvm::Type*>{i64Ty, dataArrTy});
        auto* arrInit = llvm::ConstantStruct::get(arrStructTy,
            llvm::ArrayRef<llvm::Constant*>{
                llvm::ConstantInt::get(i64Ty,
                    llvm::APInt(64, (uint64_t) len, false)),
                dataInit});
        auto* bytesGv = new llvm::GlobalVariable(*mod, arrStructTy,
            /*isConst=*/true, llvm::GlobalValue::PrivateLinkage, arrInit,
            ".str.bytes");
        llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(ptrTy);
        if (auto* vt = klass->getVirtualTableGlobal()) {
            vtableRef = CajetaModule::ensureGlobalInModule(mod, vt);
        }
        std::vector<llvm::Constant*> instFields = {
            vtableRef, bytesGv,
            llvm::ConstantInt::get(i32Ty,
                llvm::APInt(32, (uint64_t) len, true)),
            llvm::ConstantInt::get(i32Ty, llvm::APInt(32, 1, true)),
            llvm::ConstantInt::get(i32Ty,
                llvm::APInt(32, (uint64_t) -1, true))};
        // Zero-init any trailing fields (inline SSO region: ssoCount, ssoData) —
        // a literal is view-mode with bytes pointing at the static buffer.
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

    // Shared tail of a reified capture-cast guard: branch on `matchBit`, throw
    // cajeta.error.ClassCastException("... <msgDetail>") on the false edge, and
    // leave the builder positioned in the match-true block (so the caller returns
    // the reused pointer there). Thrown via the same __cajeta_throw path as a
    // user `throw`, so owned-local unwinding behaves identically.
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

    // Reified guard for a class-bounded-wildcard capture downcast
    // `(Base<? extends Bound>) w` (reified-capture-spec.md §5): throw
    // ClassCastException unless w is a `baseCanon<...>` whose reified element type
    // at `argIndex` conforms to `boundCanon`.
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

    llvm::Value* CastExpression::generateCode(CajetaModulePtr module) {
        if (children.empty() || !destType) return nullptr;
        auto* builder = module->getBuilder();
        // loadOperand only unwraps AllocaInst, which leaves field-access
        // GEPs (DotExpression, implicit-this) as raw pointers — `(int64)
        // obj.field` would then ptrtoint the GEP address instead of
        // loading + extending the field value. loadIfLValue uses the
        // ast's resolvedType to load through GEPs at the right element
        // type.
        llvm::Value* raw = children[0]->generateCode(module);
        if (!raw) return nullptr;
        auto childAst = dynamic_pointer_cast<Expression>(children[0]);
        llvm::Value* val = loadIfLValue(module, raw, childAst);
        if (!val) return nullptr;
        // iface→class downcast. When the operand is a value of an
        // interface type and the destination is a concrete (non-iface)
        // class, the operand is a pointer to the 24-byte fat-pointer
        // body { data, vtable, kind }. Strip the body by loading slot 0
        // (the data pointer) — that's the heap class instance pointer
        // we want. The destination's LLVM type is the body struct (per
        // CajetaClass::getLlvmType), but storage for class references
        // is `ptr` so returning the loaded data ptr matches what other
        // class-typed value sites expect.
        if (childAst) {
            if (!childAst->getResolvedType()) {
                childAst->resolveTypes(module);
            }
            CajetaTypePtr srcCajetaType = childAst->getResolvedType();
            auto srcClass = dynamic_pointer_cast<CajetaClass>(srcCajetaType);
            auto dstClass = dynamic_pointer_cast<CajetaClass>(destType);
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
        // Class-typed destinations store as `ptr` at runtime even though
        // getLlvmType() returns the class body struct. If the source is
        // already a `ptr`, the cast is a no-op at the LLVM level — both
        // sides are heap class pointers, just with different declared
        // Cajeta types. Without this, the fallback bitcast tries to
        // convert `ptr` to a struct type and JIT-verify rejects it.
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
                // Reified capture-downcast guard. When the source is a wildcard
                // (`Foo<?>`) or a different instantiation of the same generic
                // base, and the destination is a concrete instantiation, this
                // is a genuine reified downcast: verify w's runtime
                // instantiation matches and throw ClassCastException on
                // mismatch (the guarded `instanceof` / pattern-binding forms
                // skip the throw by branching first). Plain upcasts and
                // same-type casts don't qualify, so they're untouched.
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
                // Class-bounded-wildcard downcast `(Base<? extends Bound>) w`
                // (§5): the dst isn't concrete, so it's a bounded reified check —
                // the captured element must conform to Bound or the cast throws.
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
        // Integer WIDENING and int→fp conversion must follow the SOURCE
        // operand's signedness, not the destination's: a `uint8` zero-extends to
        // `int32` (sign-extending it — the old `destSigned` behavior — turned
        // 200 into -56), and a `uint64` converts to fp via UIToFP. Truncation
        // ignores the flag, and fp→int (below) correctly keys off the
        // destination integer's signedness, so only these two cases change.
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
        // Fallback to bitcast for anything that's bit-compatible.
        return builder->CreateBitCast(val, dstTy);
    }

    llvm::Value* PostfixExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto* builder = module->getBuilder();
        auto [addr, val] = loadOperand(module, children[0]);
        if (!val) return nullptr;
        // Class-type ++/-- dispatch. For primitives, the existing
        // pre/post distinction matters because the expression value is
        // a scalar. For classes, the operator mutates `this` and the
        // expression value is the receiver pointer in either case —
        // postfix vs prefix collapse semantically. Try dispatch
        // BEFORE the alloca-only addr requirement that the primitive
        // path needs (a class operand often lacks a writable alloca
        // when fed by a method call).
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
        // Same signed-overflow gate as prefix INC/DEC.
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
        // Postfix yields the *original* value, not the updated one.
        return val;
    }


//    antlr4::tree::TerminalNode *LPAREN();
//    ExpressionContext *expression();
//    antlr4::tree::TerminalNode *RPAREN();
//    antlr4::tree::TerminalNode *THIS();
//    antlr4::tree::TerminalNode *SUPER();
//    LiteralContext *literal();
//    IdentifierContext *identifier();
//    TypeTypeOrVoidContext *typeTypeOrVoid();
//    antlr4::tree::TerminalNode *DOT();
//    antlr4::tree::TerminalNode *CLASS();
//    ArgumentsContext *arguments();
    ExpressionPtr PrimaryExpression::fromContext(CajetaParser::PrimaryContext* ctx) {
        ExpressionPtr result = nullptr;
        if (ctx->LPAREN()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->literal()) {
            result = LiteralExpression::fromContext(ctx->literal());
        } else if (ctx->aggregateInitializer()) {
            // S6.2 — `Foo { field: expr, ... }`. Matched before the
            // identifier alternative so the parser picks the longer form;
            // a bare `Foo` still routes through the identifier branch
            // below because aggregateInitializer requires the trailing
            // `{ ... }`.
            result = make_shared<AggregateInitializerExpression>(
                ctx->aggregateInitializer(), ctx->getStart());
        } else if (ctx->arrayLiteral()) {
            // `[e1, e2, ...]` list literal (XPU launch dims; general-purpose).
            result = make_shared<ArrayLiteralExpression>(
                ctx->arrayLiteral(), ctx->getStart());
        } else if (ctx->typeTypeOrVoid() && ctx->CLASS()) {
            // REFL-1.5: `T.class` — the statically-known type's reflective Class.
            // Capture the type's text now (the ANTLR context is freed before
            // codegen); the class is resolved by name at resolveTypes time.
            result = make_shared<ClassLiteralExpression>(
                ctx->typeTypeOrVoid()->getText(), ctx->getStart());
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), true);
        } else if (ctx->THIS()) {
            // Pass the Token directly — ctx is a PrimaryContext here, so
            // ctx->expression() is null for the THIS form and the old
            // ThisExpression(ctx->expression()) overload would null-deref
            // on getStart(). The token-taking overload sidesteps that.
            auto thisExpr = make_shared<ThisExpression>(ctx->getStart());
            if (ctx->typeType()) {
                // MultiClassing Phase 2: `this<Base>` — record the
                // bracketed ancestor name. resolveTypes validates it's
                // a real ancestor and pins resolvedType so DotExpression
                // routes through the chosen sub-object.
                thisExpr->setChosenAncestorName(ctx->typeType()->getText());
            }
            result = thisExpr;
        } else if (ctx->SUPER()) {
            // `super` as a primary expression (e.g. `super.foo()`). The
            // call dispatch lives in MethodCallExpression — when its
            // receiver is a SuperExpression, the call bypasses vtable
            // lookup and direct-calls the parent's method.
            auto superExpr = make_shared<SuperExpression>(ctx->getStart());
            if (ctx->typeType()) {
                // MultiClassing Phase 2: `super<Base>` — record the
                // bracketed ancestor name. Same validation + adjustment
                // as `this<Base>`; method calls additionally force-direct
                // (MethodCallExpression keys off the SuperExpression
                // receiver type unchanged from the unbracketed form).
                superExpr->setChosenAncestorName(ctx->typeType()->getText());
            }
            result = superExpr;
        }
        return result;
    }

    llvm::Value* PrimaryExpression::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    // REFL-1.5: `T.class`.
    void ClassLiteralExpression::resolveTypes(CajetaModulePtr module) {
        if (resolvedType) return;
        // Resolve the named type by name from canonicalMap (keyed by both short
        // typeName and full canonical), now that every class is registered.
        auto& cmap = CajetaType::getCanonicalMap();
        auto nit = cmap.find(namedTypeName);
        if (nit != cmap.end()) {
            namedType = nit->second;
        }
        if (!namedType) return;   // unresolved type — generateCode reports it
        // resolvedType = Class<T> (the wildcard-phantom template instantiated
        // with the named type). Requires cajeta.reflect.Class on the path.
        auto it = cmap.find("cajeta.reflect.Class");
        auto classTmpl = (it != cmap.end())
            ? dynamic_pointer_cast<CajetaClass>(it->second) : nullptr;
        if (classTmpl && classTmpl->isTemplate()) {
            resolvedType = classTmpl->instantiate({namedType});
        }
    }

    llvm::Value* ClassLiteralExpression::generateCode(CajetaModulePtr module) {
        if (!resolvedType) resolveTypes(module);
        auto klass = dynamic_pointer_cast<CajetaClass>(namedType);
        if (!klass) {
            throw Exception(
                "`.class` requires a class type — a primitive has no runtime "
                "Class object; use a reference type before `.class`",
                "CAJETA_ERROR_CLASS_LITERAL");
        }
        // The cached #ClassObject IS the Class<T> instance: a process-lifetime
        // constant { Class<?>#VTable, rtti }. Its ADDRESS is the borrow we hand
        // back (never freed). Mirror obj.getClass()/Class.of, but statically.
        llvm::GlobalVariable* co = klass->getClassObjectGlobal();
        if (!co) {
            throw Exception(
                "no #ClassObject for '" + klass->toCanonical()
                + "' — its reflection metadata was not emitted",
                "CAJETA_ERROR_CLASS_LITERAL");
        }
        return CajetaModule::ensureGlobalInModule(module->getLlvmModule(), co);
    }

    void BooleanSwitchExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        // Result type = the `then` branch's resolved type. A full implementation would
        // unify with the `else` branch (least common ancestor); we leave that to
        // codegen-time coercion since the type system doesn't yet have promotion.
        if (children.size() >= 2) {
            if (auto thenExpr = dynamic_pointer_cast<Expression>(children[1])) {
                resolvedType = thenExpr->getResolvedType();
            }
        }
    }

    llvm::Value* BooleanSwitchExpression::generateCode(CajetaModulePtr module) {
        if (children.size() < 3) return nullptr;
        auto* builder = module->getBuilder();
        llvm::LLVMContext& ctx = *module->getLlvmContext();

        // Evaluate condition; coerce non-i1 (e.g. i32 0/non-0) to i1 via != 0.
        // Use loadIfLValue (not loadIfAllocaShared) so an l-value condition that
        // isn't a plain alloca — a boolean class FIELD (`this.flag` → a GEP),
        // an array element, a static field — is loaded to its r-value. Loading
        // only allocas left a field GEP as a pointer, and the i1 coercion below
        // then built an ICmp with a pointer operand (malformed IR / verifier
        // assertion).
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
        llvm::BasicBlock* thenEnd = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(elseBB);
        auto elseAst = dynamic_pointer_cast<Expression>(children[2]);
        llvm::Value* elseVal = loadIfLValue(module, children[2]->generateCode(module), elseAst);
        // If types differ, narrow/extend the else side to match the then side. Mirrors
        // BinaryOpExpression's coerceArithPair logic at a single point of variance.
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
        return phi;
    }

    void MoveExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        // Our resolvedType mirrors the wrapped expression's — `#x` carries the
        // same type as `x` for typing purposes; the `#` is purely an ownership
        // operation, not a coercion.
        if (!children.empty()) {
            if (auto inner = dynamic_pointer_cast<Expression>(children[0])) {
                resolvedType = inner->getResolvedType();
            }
        }
    }

    llvm::Value* MoveExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        // Evaluate the wrapped expression FIRST (while the source is still
        // readable), then mark the source as moved. Marking before evaluating
        // would trip the use-after-move check on the very read that performs
        // the transfer.
        //
        // Only direct identifier sources are tracked today; chain forms
        // (`#person.name`) require path-based borrow tracking, which lands in
        // a later step of Session 3.
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        llvm::Value* value = inner ? inner->generateCode(module) : nullptr;
        // The wrapped expression typically yields an l-value (an alloca). The
        // consumer of a moved value wants the r-value — the pointer to the
        // owned heap block, not the address of the local slot that holds it.
        // Load through the alloca so the destination receives the actual heap
        // pointer.
        if (value) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(value)) {
                value = module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
            }
        }
        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(inner)) {
            auto scope = module->getScopeStack().peek();
            if (scope) {
                scope->markMoved(idExpr->getTextValue());
                // If the moved-out identifier has a drop entry, flag it inactive
                // so scope-exit doesn't re-free the value the new owner holds.
                if (FieldPtr field = scope->getField(idExpr->getTextValue())) {
                    if (llvm::Value* entry = field->getDropEntry()) {
                        if (llvm::Function* mark = module->getRuntimeFunction(
                                "__cajeta_drop_mark_inactive")) {
                            module->getBuilder()->CreateCall(mark, {entry});
                        }
                    }
                }
            }
        } else if (dynamic_pointer_cast<DotExpression>(inner)) {
            // Path-based move (`#person.address.city`). Build the dotted path
            // and record it on the scope so future reads through that path —
            // or any prefix — are rejected. See MemoryModel.md § Path-based
            // borrow tracking.
            auto scope = module->getScopeStack().peek();
            if (scope) {
                string path = DotExpression::buildPath(inner);
                if (!path.empty()) scope->markMovedPath(path);
            }
        }
        return value;
    }

    void SwitchExpression::resolveTypes(CajetaModulePtr module) {
        if (discriminator) discriminator->resolveTypes(module);
        for (auto& c : cases) {
            for (auto& lab : c.labels) {
                if (lab) lab->resolveTypes(module);
            }
            if (c.body) c.body->resolveTypes(module);
        }
        // Result type tracks the first non-default case body's resolvedType. Cajeta
        // doesn't yet do a least-upper-bound across arms — callers that need a
        // specific shape can cast at the use site.
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

    llvm::Value* SwitchExpression::generateCode(CajetaModulePtr module) {
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

        // Separate cases into non-default and default for ordering.
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
                        // Sign-extend or truncate the label constant to match the
                        // discriminator's width — needed for cases like a `byte`
                        // discriminator with an `int` literal label.
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
            // Java requires a default in switch expressions for exhaustiveness; emit a
            // synthetic unreachable so we don't have to insert a phi entry for nothing.
            defaultBB = llvm::BasicBlock::Create(ctx, "sw_default_unreachable", parentFn);
        }

        llvm::SwitchInst* sw = builder->CreateSwitch(disc, defaultBB,
            (unsigned) nonDefault.size());
        for (auto& lc : nonDefault) {
            for (auto* l : lc.labels) sw->addCase(l, lc.bb);
        }

        // Collect each arm's result value and the BB it ends in for the phi node.
        vector<pair<llvm::Value*, llvm::BasicBlock*>> incoming;
        llvm::Type* phiTy = nullptr;

        auto emitArm = [&](llvm::BasicBlock* bb, const ExpressionPtr& body) {
            builder->SetInsertPoint(bb);
            if (!body) {
                builder->CreateUnreachable();
                return;
            }
            llvm::Value* v = body->generateCode(module);
            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                v = builder->CreateLoad(a->getAllocatedType(), v);
            }
            if (v) {
                if (!phiTy) phiTy = v->getType();
                else if (phiTy != v->getType() && phiTy->isIntegerTy() && v->getType()->isIntegerTy()) {
                    // Widen integer arms to a common width — pick the larger.
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
            // All arms unreachable — nothing to return.
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
        return phi;
    }

    llvm::Value* UnsupportedExpression::generateCode(CajetaModulePtr module) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%s is not yet implemented (source line %d, column %d)",
            constructName.c_str(), sourceLine, sourceColumn);
        throw Exception(string(buf), "CAJETA_ERROR_NOT_IMPLEMENTED");
    }

    // --- LambdaExpression -------------------------------------------------

    static thread_local int64_t lambdaCounter = 0;  // per-thread (threadsafe U4)

    // Walk the lambda body's AST collecting names of free identifiers — names
    // referenced inside the body that aren't bound by the lambda's parameter
    // list. The walker has to know about a few AST shapes where sub-nodes
    // live outside `children`:
    //   - MethodCallExpression: args in `parameters` (children[0] is the
    //     receiver).
    //   - IdentifierExpression: terminate; the identifier name is the target.
    //   - LocalVariableDeclaration / ReturnStatement / IfStatement /
    //     ExpressionStatement: sub-expressions are in private fields
    //     reached via dedicated getters. Block-body lambdas would otherwise
    //     skip every identifier on a statement's RHS.
    // Identifiers that don't resolve to a local Field (class names, namespace
    // tokens like `Math` / `System`) get filtered later when the caller looks
    // up the outer scope, so it's fine to over-collect here.
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
        // Statement types whose inner block / sub-statements live as
        // private members rather than in `children` need explicit
        // handlers, or the generic getChildren() fallback below skips
        // them — leaving free identifiers inside the nested body
        // uncaptured. The visible bug: a captured-class identifier
        // inside an if-branch block (LabelStatement wrapper) is never
        // collected, the lambda's captures-struct doesn't include it,
        // and at codegen the body's IdentifierExpression lookup
        // returns null — the resulting instance-method call drops
        // `this`.
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

    // Find the leftmost (receiver-side) identifier in an expression's
    // subtree. The grammar groups `#c.next()` as `#(c.next())` — REFERENCE
    // binds looser than `.` — so the name being transferred isn't the
    // MoveExpression's immediate child but lives one level deeper, as the
    // method call's receiver. Walking to the leftmost leaf via known
    // expression shapes (method call → receiver, dot/array-index → lhs,
    // identifier → that identifier) gives the receiver-side name.
    static std::string firstIdentifierIn(const AbstractSyntaxNodePtr& node) {
        if (!node) return "";
        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(node)) {
            return id->getTextValue();
        }
        if (auto mc = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
            // children[0] is the receiver, when there is one. A bare
            // `foo(x)` has no receiver and no transfer candidate at this
            // level — `#foo(x)` would mean "transfer the call's return",
            // which isn't meaningful for capture-by-transfer.
            if (!mc->getChildren().empty()) {
                return firstIdentifierIn(mc->getChildren()[0]);
            }
            return "";
        }
        // Generic fallback: first non-empty leftmost identifier. Handles
        // DotExpression, ArrayIndexExpression, parens, etc., which all
        // place the receiver/lhs in children[0].
        for (auto& c : node->getChildren()) {
            auto r = firstIdentifierIn(c);
            if (!r.empty()) return r;
        }
        return "";
    }

    // Collect the names of outer-scope identifiers transferred into the
    // closure via `#name` in the lambda body. Rule 3 from
    // docs/specification/lang/Lambdas.md: ownership moves into the closure and the
    // outer name becomes unreadable afterwards. Uses the same statement-
    // shape recursion as collectFreeIdentifiers so block bodies pick up
    // `#name` anywhere it appears (return value, initializer, condition,
    // arg). The transferred name is the receiver-side leaf of the move's
    // subtree (see firstIdentifierIn).
    static void collectTransferNames(
            const AbstractSyntaxNodePtr& node,
            std::set<std::string>& out) {
        if (!node) return;
        if (auto mv = std::dynamic_pointer_cast<MoveExpression>(node)) {
            if (!mv->getChildren().empty()) {
                std::string name = firstIdentifierIn(mv->getChildren()[0]);
                if (!name.empty()) out.insert(name);
            }
            // Still recurse — nested `#` (rare but possible).
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
        // Same gap as collectFreeIdentifiers — Statement subclasses
        // whose inner blocks aren't in `children` need explicit
        // descents so `#name` transfers nested inside their bodies
        // are not silently demoted to borrows.
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

    // Rule 5 from docs/specification/lang/Lambdas.md: writing to a primitive that was
    // captured by value is a compile error — the lambda is mutating a
    // private copy, so the write would silently fail to propagate. The
    // user must use a mutable wrapper (Cell-style) to opt into shared
    // mutability. Walks the body looking for assignment-form
    // BinaryOpExpressions whose LHS resolves to a value-captured name.
    // Throws with a precise message naming the offending identifier.
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
        // Same gap as collectFreeIdentifiers — without these descents
        // a value-captured primitive assigned inside an if-branch
        // block / loop body / scope block would slip past the check
        // (the assignment is the LHS of a BinaryOpExpression hidden
        // inside the LabelStatement / WhileStatement / etc.).
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

    // Walk a lambda's block body for ReturnStatement nodes (skipping
    // nested lambdas — their returns belong to a different function)
    // and yield the first explicit return's resolved expression type.
    // A bare `return;` (no expression) yields `void`. If no return is
    // found, returns nullptr — the lambda is "falls through to end" and
    // the caller treats that as void.
    //
    // Only the FIRST return's type is consulted. Multi-arm lambdas that
    // return different types from different branches are already a
    // type error (the user would need to annotate the lambda's return
    // type explicitly via the LHS function type); this helper exists to
    // recover the common case where there's a single explicit return
    // and no contextual expectedType.
    static CajetaTypePtr inferLambdaReturnFromBody(
            const AbstractSyntaxNodePtr& node) {
        if (!node) return nullptr;
        // Don't descend into nested lambdas — their returns are routed
        // to a separate synthesized function, not this lambda.
        if (std::dynamic_pointer_cast<LambdaExpression>(node)) {
            return nullptr;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) {
            auto expr = ret->getExpression();
            if (!expr) return CajetaType::of("void");
            return expr->getResolvedType();
        }
        // Same Statement-subtype gap the capture walkers have: nested
        // block / sub-statement members live as private fields rather
        // than in `children`, so the generic getChildren() fallback
        // silently skips them. Explicit handlers descend into the
        // bodies so returns inside if-branches, loops, scope blocks,
        // etc. participate in lambda return-type inference.
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

    // Walk the body for top-level LocalVariableDeclarations and
    // collect their (name, declared-type) pairs. Used by
    // LambdaExpression::resolveTypes to pre-populate the body's
    // resolve-time scope so IdentifierExpression lookups for body
    // locals (`return t;` where `int32 t = ...;` declares `t`) find
    // a type instead of returning null. Without this the lambda's
    // return-type inference falls through to void for any lambda
    // whose return references a body-local rather than a parameter.
    //
    // Top-level only on purpose: locals declared inside nested blocks
    // (if/for/while bodies) have block scope and shouldn't leak into
    // the lambda's outer name space at resolve time. Returns from
    // inside those nested blocks already reference identifiers the
    // standard scope chain won't find at resolve time either; the
    // typed-local workaround on the caller side remains the
    // intended idiom for that case.
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

    void LambdaExpression::resolveTypes(CajetaModulePtr module) {
        // L1: bodies can't reference outer-scope locals (captures are L2),
        // so the lambda's body resolution doesn't need access to the
        // surrounding scope's fields beyond what resolveTypes naturally
        // walks. Identifier resolution to lambda-local params happens at
        // generateCode time when the parameter scope is pushed.
        if (!module) module = CajetaModule::getActiveModule();
        if (!module) return;

        // Push a temporary scope holding the lambda's declared
        // parameters so the body's resolveTypes can resolve bare
        // identifiers (`acc`, `c`) to their declared types. Without
        // this, expressions like `acc + c.v` resolve `acc` to null
        // and `c` to null, and the BinaryOpExpression's fallback
        // path picks the wrong return type — surfaced by templated-
        // method calls that infer R from the lambda's return type
        // (Stream<T>.fold<R> with class-typed T, or Collector ctors
        // with function-typed second arg). Skip when paramTypes
        // aren't yet pinned (bare-identifier lambdas waiting for
        // target-type inference at generateCode time).
        bool pushedScope = false;
        if (body && paramTypes.size() == paramNames.size()
                && !paramNames.empty()) {
            auto paramScope = std::make_shared<Scope>(
                std::string("__lambda_resolve"), module);
            for (size_t i = 0; i < paramNames.size(); ++i) {
                // StackField is a concrete leaf with the simple
                // (module, name, type) ctor. Allocation never runs
                // (this scope is dropped after resolveTypes), so the
                // codegen-only branches of StackField stay unused.
                auto fld = std::make_shared<StackField>(
                    module, paramNames[i], paramTypes[i]);
                paramScope->putField(fld);
            }
            // Pre-register top-level body locals so body resolveTypes
            // can resolve `return t;` (where `int32 t = ...;` declares
            // `t`) to t's declared type instead of leaving the
            // identifier unresolved. Without this, inferLambdaReturnFromBody
            // gets a null type back from `t.getResolvedType()` and the
            // lambda's return type falls through to void — surfaces as
            // "JIT verify: Function of void return type" at codegen.
            // Body-locals are normally registered at generateCode time;
            // this is a resolve-time pre-registration that mirrors what
            // codegen would do, just for type inference.
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

        // Determine the lambda's CajetaFunctionType. Preferred order:
        //   1. expectedType (a CajetaFunctionType from the surrounding
        //      context, e.g. the LHS of a function-typed variable
        //      declaration that called setExpectedType before resolveTypes).
        //   2. Expression body: the body's resolvedType (works for
        //      identifiers, literals, method calls — arithmetic
        //      expressions sometimes leave it null).
        //   3. Block body: walk the body for explicit return statements
        //      and take the first one's resolved expression type. This
        //      covers the common case where a typed-param block-body
        //      lambda is passed as a constructor / method argument
        //      without the call-site propagating an expectedType (e.g.
        //      `new Holder(seed, (T acc, U x) -> { ...; return acc; })`).
        //   4. Fallback: void. A nonsensical lambda surfaces at codegen.
        CajetaTypePtr ret;
        // M5(b) — function-type return ABI is sret form iff the body yields a
        // stack value (an expression body that IS `stack X(...)`, or a block
        // body whose returns are `return stack X(...)`). An expectedType from
        // the LHS pins the ABI; otherwise infer it the same way Method does.
        bool returnsOwn = true;
        if (auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
            ret = expectedFn->getReturnType();
            returnsOwn = expectedFn->isReturnsOwnership();
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
            // Block body — walk for the first explicit return statement.
            // Returns void if the body has no `return` at all.
            ret = inferLambdaReturnFromBody(body);
            if (Method::nodeHasStackReturn(body)) {
                returnsOwn = false;
            }
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

    llvm::Value* LambdaExpression::generateCode(CajetaModulePtr module) {
        // L1.5 target-type inference: bare-identifier lambda params (parsed
        // with empty `paramTypes`) borrow their types from the surrounding
        // context's expectedType. The expectedType is set by
        // LocalVariableDeclaration::generateCode before this generateCode
        // runs, so we infer here rather than in resolveTypes (which runs
        // earlier, before the context wiring has happened).
        if (paramTypes.size() < paramNames.size()) {
            auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType);
            if (expectedFn
                    && expectedFn->getParameterTypes().size() == paramNames.size()) {
                paramTypes = expectedFn->getParameterTypes();
                // Stale resolvedType (which was computed without the now-
                // known param types) — recompute so the synthesized
                // function uses the right signature.
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
        // Ensure resolvedType (the CajetaFunctionType) is computed — body
        // may not have been pre-resolved if this lambda is itself a
        // sub-expression of an unresolved context.
        if (!resolvedType) resolveTypes(module);
        auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(resolvedType);
        if (!fnType) {
            throw Exception("lambda has no function type",
                "CAJETA_ERROR_NOT_IMPLEMENTED");
        }

        // Synthesize a unique name for the LLVM function. The lambda body
        // becomes the function body; the function is registered in the
        // current module's IR module and returned as a `ptr` value.
        if (synthesizedName.empty()) {
            synthesizedName = std::string("__cajeta_lambda_")
                + std::to_string(lambdaCounter++);
        }

        auto* lmod = module->emitTargetLlvmModule();
        // Already emitted? (Re-entering codegen for the same lambda — rare,
        // but harmless to return the existing function pointer.)
        if (auto* existing = lmod->getFunction(synthesizedName)) {
            return existing;
        }

        // ---- L2 capture analysis (run while outer scope is still on top
        //      of the stack — before we push the lambda's own scope). Walk
        //      the body for free identifiers, then for each candidate look
        //      up the outer scope. Primitive Fields → capture-by-value
        //      (L2-2). Non-primitive Fields → capture-by-borrow (L2-3): we
        //      copy the outer slot's pointer value into the captures
        //      struct, so the lambda sees the same heap object. `#name`
        //      transfers + lifetime/escape checks remain L3 territory.
        //      Identifiers that don't resolve to a Field (class names,
        //      namespaces) are left for the body's normal lookup; they
        //      don't go through captures.
        std::set<std::string> bound(paramNames.begin(), paramNames.end());
        std::set<std::string> seen;
        std::vector<std::string> freeNames;
        collectFreeIdentifiers(body, bound, seen, freeNames);

        struct Capture {
            std::string name;
            CajetaTypePtr type;
            bool byValue;     // true: primitive copy; false: pointer slot
            bool byTransfer;  // true: `#name` transfer (only meaningful when
                              //       !byValue); false: borrow (default)
        };
        std::vector<Capture> captures;
        ScopePtr outerScope = module->getScopeStack().isEmpty()
            ? nullptr : module->getScopeStack().peek();
        // Names that appeared inside `#name` somewhere in the body — these
        // are transfer captures (Rule 3), not the default borrow. Computed
        // up-front so the categorization loop below can consult it.
        std::set<std::string> transferNames;
        collectTransferNames(body, transferNames);
        if (outerScope) {
            for (auto& name : freeNames) {
                FieldPtr f = outerScope->getField(name);
                if (!f) continue;  // class name, namespace, or unresolved
                CajetaTypePtr t = f->getType();
                if (!t) continue;
                // Function-typed captures. A function value is a single
                // `ptr` to a closure record `{ fn, captures }`, so a *borrow*
                // capture is just the usual pointer copy (the borrow path
                // below): the closure record outlives the capturing closure
                // exactly as any borrowed heap object does, and the escape
                // check (hasBorrowCaptures) ties the lifetimes the same way.
                // Only a `#`-*transfer* capture of a closure stays deferred —
                // surrendering the record means the capturing closure's
                // synthesized drop_fn must free it, which L2 drop synthesis
                // doesn't model yet.
                if (std::dynamic_pointer_cast<CajetaFunctionType>(t)) {
                    if (transferNames.find(name) != transferNames.end()) {
                        throw Exception(
                            "transfer-capturing a function value (`#" + name
                            + "`) inside a closure is not supported yet — "
                            "capture it by borrow (drop the `#`) or restructure "
                            "so the closure is not moved",
                            "CAJETA_ERROR_NOT_IMPLEMENTED");
                    }
                    // else: fall through to the pointer-slot borrow path.
                }
                // Source-of-truth for "is this a value or a pointer slot"
                // is the outer field's existing alloca, not the type
                // flags. A few language types (CajetaArray in particular)
                // are flagged PRIMITIVE but live behind a pointer slot
                // because they're heap-allocated; relying on the flag
                // alone would mis-route the capture. Pointer slot →
                // borrow capture (copy the ptr); value slot → primitive
                // capture (copy the value).
                llvm::AllocaInst* outerSlot = f->getOrCreateAllocation();
                bool slotIsPointer = outerSlot
                    && outerSlot->getAllocatedType()->isPointerTy();
                bool byValue = !slotIsPointer;
                // `#name` only makes sense for heap values (pointer slot).
                // On a primitive, `#` is just a no-op marker and the
                // capture stays by-value — Rule 1 wins because there's
                // nothing to transfer.
                bool byTransfer = !byValue
                    && transferNames.find(name) != transferNames.end();
                captures.push_back({name, t, byValue, byTransfer});
                // L3-2: any borrow capture (heap reference held by the
                // closure without an explicit transfer) ties the
                // closure's lifetime to the enclosing scope. The escape
                // check at ReturnStatement consults this flag to reject
                // returning such a closure.
                if (!byValue && !byTransfer) {
                    hasBorrowCaptures = true;
                }
            }
        }

        // L2-5: writes to value-captured primitives are a compile error
        // (the lambda would be mutating a private copy invisibly). Build
        // the set of value-capture names from `captures` and walk the
        // body checking assignment LHSes against it. Throws on the first
        // offender so the error message stays precise.
        if (!captures.empty()) {
            std::set<std::string> valueCapturedNames;
            for (auto& c : captures) {
                if (c.byValue) valueCapturedNames.insert(c.name);
            }
            if (!valueCapturedNames.empty()) {
                enforceValueCaptureImmutability(body, valueCapturedNames);
            }
        }

        // Build the captures struct LLVM type. Anonymous; LLVM unifies
        // structurally identical literal struct types so the type used at
        // the alloc site (outer code) matches the type used by GEPs inside
        // the lambda body. Primitive captures occupy their natural LLVM
        // type slot; borrow captures occupy a `ptr` slot (matches the
        // outer StackField's alloca shape for non-primitives).
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

        // A non-capturing lambda's synthesized function is referenced ONLY
        // through its (private constant) closure record — never by external
        // symbol name — so it can have INTERNAL linkage. That lets the
        // optimizer treat it as the sole definition: when the constant closure
        // record flows into a generic callee (e.g. `Sort.sort`'s `cmp`),
        // IPSCCP / function-specialization can fold the loaded `fn` to this
        // constant and devirtualize the indirect closure call to a direct,
        // inlinable call (the monomorphization C++/Rust get for free). Indirect
        // calls go through the fn's ADDRESS (held in the record), which stays
        // valid across modules in the final binary regardless of linkage.
        // Capturing closures keep external linkage (the heap record + drop_fn
        // path is unchanged).
        llvm::GlobalValue::LinkageTypes lambdaLinkage =
            captures.empty() ? llvm::Function::InternalLinkage
                             : llvm::Function::ExternalLinkage;
        llvm::Function* fn = llvm::Function::Create(
            fnType->getLlvmFunctionType(),
            lambdaLinkage,
            synthesizedName,
            lmod);

        // M5(b) — sret form: arg 0 is the caller-allocated result slot,
        // captures and user params shift by one. The sret attribute carries
        // the struct type so the backend / optimizer can reason about the
        // pointer's role (and CallInst sites use it too). The shift offset
        // `sretOffset` is reused for every fn->getArg() index that follows.
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

        // Save current insertion state — we're going to emit the lambda's
        // body into its own function, then restore the outer cursor so the
        // surrounding expression continues building.
        auto* outerBuilder = module->getBuilder();
        llvm::BasicBlock* outerInsertBlock = outerBuilder->GetInsertBlock();
        MethodPtr outerMethod = module->getCurrentMethod();

        // ---- Allocate + populate the captures struct. Heap-allocate when
        //      there are captures so the struct can outlive the producing
        //      method's stack frame; the matching free is in the lambda's
        //      synthesized drop function (built below). Each primitive
        //      capture loads its current value from the outer field's slot
        //      and stores it into the captures struct.
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
                // Primitive capture: load the value itself.
                // Borrow capture: load the pointer the outer slot holds
                // (StackField allocs `ptr` for non-primitives, so the load
                // type is `ptr` — same as the captures-struct field).
                llvm::Type* loadTy = cap.byValue
                    ? cap.type->getLlvmType()
                    : (llvm::Type*) llvm::PointerType::get(llvmCtx, 0);
                llvm::Value* outerVal = outerBuilder->CreateLoad(
                    loadTy, outerSlot, cap.name);
                llvm::Value* slot = outerBuilder->CreateStructGEP(
                    capturesTy, capturesPtr, (unsigned) i,
                    std::string("cap.") + cap.name);
                outerBuilder->CreateStore(outerVal, slot);
                // Rule 3 (`#name` transfer): mark the outer binding as
                // moved and deactivate its drop entry. The closure now
                // owns the transferred value — its synthesized drop_fn
                // (below) will free the heap object when the closure
                // itself drops. Without the deactivation, both the
                // outer and the closure would attempt to free the same
                // memory.
                if (cap.byTransfer && outerScope) {
                    outerScope->markMoved(cap.name);
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

        // Open a fresh scope for the lambda's parameters + captures. After
        // adding to the stack, sever the parent link so identifier lookups
        // inside the body never walk up into the outer method's scope —
        // every cross-scope use must come through the explicit captures
        // mechanism. Missed captures surface as missing-identifier errors
        // at body codegen rather than emitting invalid cross-function IR.
        module->getScopeStack().add(make_shared<Scope>(synthesizedName, module));
        module->getScopeStack().peek()->setParent(nullptr);

        // L1 dummy method context — we need *some* MethodPtr on the module
        // because some helpers (e.g. drop-chain bookkeeping) read it, even
        // though a lambda body in L1 has no drop chain of its own.
        module->setCurrentMethod(nullptr);

        for (size_t i = 0; i < paramNames.size(); ++i) {
            // Bind each LLVM arg into an alloca-backed ParameterField so
            // identifier lookups inside the body load through it (same
            // pattern as method params). LLVM arg 0 is the implicit
            // `captures` pointer (L2 ABI), so user param i lives at LLVM
            // arg i + 1. Under sret form the sret slot precedes captures,
            // adding `sretOffset` to every LLVM arg index.
            auto formal = std::make_shared<FormalParameter>(
                paramNames[i], paramTypes[i]);
            auto field = std::make_shared<ParameterField>(
                module, formal, fn, (int) i + 1 + (int) sretOffset);
            // Force the alloca + store now so getOrCreateAllocation later
            // returns the populated slot.
            field->getOrCreateAllocation();
            module->getScopeStack().peek()->putField(field);
        }

        // ---- Unpack captures inside the lambda function. LLVM arg 0 is
        //      the captures pointer; for each captured primitive, GEP into
        //      the captures struct, load the value, and stash it in a
        //      local alloca. Register a StackField pointing at that alloca
        //      so the body's identifier lookups find the captured value as
        //      if it were a normal local.
        if (capturesTy) {
            llvm::Value* capArg = fn->getArg(sretOffset);
            llvm::Type* ptrLlvmTy = llvm::PointerType::get(llvmCtx, 0);
            for (size_t i = 0; i < captures.size(); ++i) {
                auto& cap = captures[i];
                llvm::Value* slot = lambdaBuilder->CreateStructGEP(
                    capturesTy, capArg, (unsigned) i,
                    std::string("cap.") + cap.name);
                // Primitive: load the value (e.g. i32). Borrow: load the
                // captured `ptr`. Local alloca shape mirrors StackField's
                // own shape for the corresponding type so the body's
                // identifier-lookup-and-load matches.
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

        // Generate the body. Expression bodies produce a single value that
        // becomes the implicit return; block bodies produce no value and
        // contain their own ReturnStatement(s).
        bool blockBody = std::dynamic_pointer_cast<Block>(body) != nullptr;
        // M5(b) NRVO — when the lambda is sret-shaped and the expression
        // body IS a `stack X(...)` construction, redirect the construction
        // target at the sret slot so the ctor writes its fields straight
        // into the caller's memory (zero copy). For block bodies the same
        // redirection happens inside ReturnStatement (Statement.cpp).
        if (sretOffset && !blockBody) {
            if (auto nx = std::dynamic_pointer_cast<NewExpression>(body)) {
                if (nx->getStackAlloc()) nx->setNrvoTarget(fn->getArg(0));
            }
        }
        // Isolate the enclosing method's try-finally stack: this lambda body is
        // codegen'd inline, but its `return`s must only unwind try frames the
        // lambda itself pushed, not the enclosing method's open try frames.
        std::vector<std::shared_ptr<void>> savedTryFrames = module->takeTryFinally();
        llvm::Value* bodyVal = body->generateCode(module);

        if (blockBody) {
            // The block's own ReturnStatement emits the terminator. If the
            // user wrote a block that falls through to the closing brace
            // without returning, the current insertion block has no
            // terminator yet — emit a default return so LLVM verify is
            // satisfied (RetVoid for void lambdas, undef of the return
            // type otherwise; matches how a malformed regular method
            // would surface).
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
            // L-value-to-r-value coercion. The expression body's value
            // flows directly into the return instruction below, so an
            // l-value (alloca, array-slot GEP, struct/class field GEP)
            // needs to be loaded first. Mirrors the same conversions
            // ReturnStatement does — keep them in sync when one grows a
            // new case.
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
                // M5(b) sret form: body already wrote into the caller-
                // owned slot (NRVO when body was a `stack` construction;
                // otherwise bodyVal is a pointer to copy in). RetVoid.
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
                    // Coerce to the function return type if width differs
                    // (literal i64 going to an i32 return slot, etc.) —
                    // mirrors what invokeMethod does at the call site.
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

        // Restore outer state.
        module->restoreTryFinally(std::move(savedTryFrames));
        module->getScopeStack().pop();
        delete lambdaBuilder;
        module->setBuilder(outerBuilder);
        module->setCurrentMethod(outerMethod);
        outerBuilder->SetInsertPoint(outerInsertBlock);

        // L3-3 closure layout: `{ ptr fn, ptr captures, ptr drop_fn }`.
        // The drop_fn slot lets the runtime's __cajeta_closure_drop find
        // the per-lambda free routine without needing static type info at
        // the call site. Heap-alloc when there are captures so the
        // closure can outlive the producing method's stack frame
        // (escape); stack-alloc for non-capturing closures (drop_fn=null,
        // safe because nothing to free).
        llvm::StructType* closureTy = llvm::StructType::get(llvmCtx,
            {(llvm::Type*) ptrTy, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy});
        uint64_t closureBytes = dl.getTypeAllocSize(closureTy);

        // Synthesize the per-lambda drop function for capturing closures.
        // It frees each transferred capture's heap object, then the
        // captures struct, then the closure record itself. Borrow and
        // value (primitive) captures need nothing — the closure didn't
        // own them. Only transfer captures (`#name`) require freeing.
        llvm::Value* dropFnValue = llvm::ConstantPointerNull::get(ptrTy);
        if (capturesTy) {
            llvm::Function* freeArrayFn = module->getRuntimeFunction("__cajeta_free_array");
            llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
            std::string dropName = synthesizedName + "_drop";
            llvm::FunctionType* dropFnTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvmCtx), {ptrTy}, /*isVarArg=*/false);
            llvm::Function* dropFn = llvm::Function::Create(dropFnTy,
                llvm::Function::ExternalLinkage, dropName, lmod);
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
                // Today only CajetaArray transfer captures have a known
                // drop function. Non-array transfers compile but leak —
                // tracked alongside the existing emitDropEntryFor coverage.
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

        // Materialize the closure record. Two paths:
        //   - Non-capturing: emit a private global constant
        //     `{ fn, null, null }`. Globals live for the program's
        //     lifetime, so the closure can safely escape its declaring
        //     scope without a heap allocation — most non-capturing
        //     lambdas are tiny constants and never freed anyway.
        //   - Capturing: heap-allocate the record so the closure can
        //     outlive the producing method's frame. The synthesized
        //     drop_fn frees the closure when ownership runs out.
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

    // Resolve a MethodReferenceExpression's LHS to a target class.
    // Returns the class plus whether the LHS resolved as a runtime VALUE
    // (a local of class type, suitable for binding) or as a TYPE-NAME
    // (the class itself, suitable for unbound / static refs).
    //
    // The grammar parses `Util::method` (where Util is a bare class name)
    // as `expression '::' identifier` — IdentifierExpression's scope
    // lookup leaves resolvedType null. When that happens we fall back to
    // scanning the type registry for a class whose short name matches,
    // and flag the result as a type-name resolution so the caller can
    // pick UNBOUND_INSTANCE over BOUND_INSTANCE.
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
            // typeType form is always type-name; receiverIsValue stays false.
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
        // Class-name fallback for bare identifiers naming a class type.
        auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(receiverExpr);
        if (!idExpr) return out;
        const std::string& shortName = idExpr->getTextValue();
        for (auto& entry : CajetaType::getCanonicalMap()) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(entry.second);
            if (!klass) continue;
            auto qn = klass->getQName();
            if (qn && qn->getTypeName() == shortName) {
                out.targetClass = klass;
                // receiverIsValue stays false — the LHS named a type, not an instance.
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

    // Find a method on `klass` by name. The lookup currently doesn't
    // overload-resolve — L4-1 assumes one method per name on the
    // referenced class. When multiple methods share the name, the first
    // one found wins; overload-aware method references will follow once
    // we have a target-type-driven resolution pass.
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

    void MethodReferenceExpression::resolveTypes(CajetaModulePtr module) {
        if (!module) module = CajetaModule::getActiveModule();
        if (!module) return;

        // Decide the kind. L4-1 only implements the STATIC form; the
        // others land on a NOT_IMPLEMENTED at codegen so the parser
        // still accepts every shape the grammar covers.
        MethodRefTargetResolution res = resolveMethodRefTarget(
            receiverType, receiverExpr, module);
        CajetaClassPtr targetClass = res.targetClass;

        if (isCtor) {
            kind = Kind::CONSTRUCTOR;
            if (!targetClass) return;
            // Find any constructor on the class (L4-4 doesn't yet
            // overload-resolve — first ctor wins). Build the function-
            // value type from the ctor's user-facing params (skip the
            // synthesized `this` at index 0) and the class instance
            // type as the return.
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
            // Constructor returns ownership of a heap instance — always
            // ownership-form function type. Assigning a constructor ref to
            // an sret-form slot would leak the heap instance after the
            // adapter copy, so reject upfront per the matrix.
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
            // Derive the function-value type from the method's
            // parameter and return types. Static methods don't have an
            // implicit `this`, so the parameter list IS the user-
            // declared one.
            std::vector<CajetaTypePtr> paramTypes;
            for (auto& p : staticMethod->getParameterList()) {
                paramTypes.push_back(p->getType());
            }
            CajetaTypePtr ret = staticMethod->getReturnType();
            // Function-type ABI: sret form iff the target method returns
            // by stack value (M5(b)). Borrow and `#R` returns both produce
            // the ownership-form pointer-return signature today, so they
            // share the `returnsOwn=true` shape. When the LHS pins an
            // sret-form callback type, override to sret — the adapter in
            // generateCode bridges a borrow method into the sret slot.
            // Ownership-returning methods can't adapt: an sret target
            // would discard the heap pointer's owner role and leak.
            bool returnsOwn = !staticMethod->returnsStackValue();
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

        // Instance method — BOUND_INSTANCE if the LHS resolved as a
        // runtime value (a local of class type), UNBOUND_INSTANCE if it
        // named a type (typeType form or class-name fallback from a bare
        // identifier).
        MethodPtr instanceMethod = findMethodByName(targetClass, methodName, /*wantStatic=*/false);
        if (instanceMethod) {
            kind = res.receiverIsValue
                ? Kind::BOUND_INSTANCE
                : Kind::UNBOUND_INSTANCE;
            // Compute the function-value type. instanceMethod's
            // parameterList includes the synthesized `this` at index 0
            // (per Method::generatePrototype).
            std::vector<CajetaTypePtr> paramTypes;
            auto pl = instanceMethod->getParameterList();
            if (kind == Kind::UNBOUND_INSTANCE) {
                // Receiver becomes the function-value's first param;
                // user-facing args follow.
                paramTypes.push_back(targetClass);
            }
            for (size_t i = 1; i < pl.size(); ++i) {
                paramTypes.push_back(pl[i]->getType());
            }
            CajetaTypePtr ret = instanceMethod->getReturnType();
            // M5(b) — sret form iff the target method returns by stack
            // value. Same shape rule + LHS-pinned override + matrix
            // checks as the static case above.
            bool returnsOwn = !instanceMethod->returnsStackValue();
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

    llvm::Value* MethodReferenceExpression::generateCode(CajetaModulePtr module) {
        if (!resolvedType) resolveTypes(module);

        if (kind == Kind::CONSTRUCTOR) {
            // Resolve the class + its constructor again at codegen time
            // — we need both the LLVM struct type (for sizing the
            // allocation and the vtable slot) and the ctor's LLVM
            // function pointer for the thunk's call.
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

            // Synthesize the thunk:
            //   (ptr captures, ctor_args...) -> ptr instance
            // Body mirrors what CreatorRest::generateCode does for
            // `new Foo(args)`: malloc the struct, write the vtable,
            // call the ctor, return the instance pointer.
            if (thunkName.empty()) {
                thunkName = std::string("__cajeta_method_ref_")
                    + std::to_string(methodRefCounter++);
            }
            llvm::Function* existing = lmod->getFunction(thunkName);
            llvm::Function* thunk = existing
                ? existing
                : llvm::Function::Create(fnType->getLlvmFunctionType(),
                    llvm::Function::ExternalLinkage, thunkName, lmod);
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

                // Write the vtable pointer at slot 0 (required for any
                // virtual dispatch on the new instance). Same cross-
                // module fixup as CreatorRest — targetClass's vtable
                // global may live in a different llvm::Module than
                // where this thunk is being emitted.
                if (llvm::GlobalVariable* vtable = targetClass->getVirtualTableGlobal()) {
                    llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                        lmod, vtable);
                    llvm::Value* vptrSlot = tb.CreateStructGEP(
                        structTy, instance, /*idx=*/0, "vtable_slot");
                    tb.CreateStore(vtableRef, vptrSlot);
                }

                // Pass the new instance as `this`, then the thunk's
                // explicit args (1..) as the user-facing ctor params.
                std::vector<llvm::Value*> ctorArgs;
                ctorArgs.push_back(instance);
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = 1; i < thunkArgCount; ++i) {
                    ctorArgs.push_back(thunk->getArg(i));
                }
                llvm::Function* ctorFn = CajetaModule::ensureFunctionInModule(
                    lmod, ctor->getLlvmFunction());
                tb.CreateCall(ctor->getLlvmFunctionType(), ctorFn, ctorArgs);
                tb.CreateRet(instance);
            }

            // Non-capturing — private global closure record.
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

        // Look up the target method again at codegen time — we need its
        // LLVM function for the thunk's call instruction. resolveTypes
        // confirmed it exists.
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

        // Synthesize a thunk function matching the closure ABI:
        //   (ptr captures, <user-facing method params>) -> <return>
        // For STATIC: captures is ignored; args forward straight to the
        //   underlying static method.
        // For BOUND_INSTANCE: load the captured receiver from captures[0]
        //   and pass it as `this` (the underlying instance method's first
        //   arg) along with the rest of the user args.
        if (thunkName.empty()) {
            thunkName = std::string("__cajeta_method_ref_")
                + std::to_string(methodRefCounter++);
        }
        llvm::Function* existing = lmod->getFunction(thunkName);
        llvm::Function* thunk = existing
            ? existing
            : llvm::Function::Create(fnType->getLlvmFunctionType(),
                llvm::Function::ExternalLinkage, thunkName, lmod);
        // M5(b) — when the function-type is sret-shaped the thunk also takes
        // a hidden sret slot at arg 0; mirror the attribute on its parameter
        // and shift captures/user-arg indices by 1.
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
        // M5(b) adapter — the thunk is sret-shaped but the target method
        // returns by pointer (borrow). The call doesn't take an sret slot;
        // instead capture its returned R* and memcpy into the thunk's sret
        // slot before ret void. Matrix-rejected combinations (ownership
        // method → sret target, sret method → ownership target) are
        // already filtered in resolveTypes above.
        bool sretAdapter = sretOffset && !target->returnsStackValue();
        if (!existing) {
            llvm::BasicBlock* tbb = llvm::BasicBlock::Create(
                llvmCtx, "entry", thunk);
            llvm::IRBuilder<> tb(tbb);
            std::vector<llvm::Value*> callArgs;
            // When the target method itself is sret-shaped, its arg 0 IS the
            // sret slot — forward the thunk's sret arg straight through. In
            // the adapter case the target has no sret arg; skip the prepend
            // and copy the call result into the sret slot after the call.
            if (sretOffset && !sretAdapter) {
                callArgs.push_back(thunk->getArg(0));
            }
            if (kind == Kind::BOUND_INSTANCE) {
                // Captures struct is `{ ptr receiver }` at thunk arg
                // `sretOffset`. Load the receiver and pass it as `this`,
                // then user args at thunk arg `sretOffset + 1..`.
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
                // Thunk arg `sretOffset + 1` IS the receiver. Pass it as
                // `this`, then args from `sretOffset + 2..`.
                callArgs.push_back(thunk->getArg(sretOffset + 1));
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = sretOffset + 2; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            } else {
                // STATIC — captures is unused; forward args from
                // `sretOffset + 1..`.
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = sretOffset + 1; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            }
            llvm::Function* targetFn = CajetaModule::ensureFunctionInModule(
                lmod, target->getLlvmFunction());
            llvm::CallInst* call = tb.CreateCall(
                target->getLlvmFunctionType(), targetFn, callArgs);
            if (sretOffset && !sretAdapter) {
                // Matched sret-to-sret — mirror the sret call-site
                // attribute (LLVM verify wants it on indirect calls; for
                // direct calls it's redundant but harmless).
                auto retClass = std::dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
                llvm::Type* structTy = retClass ? retClass->getLlvmType() : nullptr;
                if (structTy) {
                    call->addParamAttr(0, llvm::Attribute::get(
                        llvmCtx, llvm::Attribute::StructRet, structTy));
                }
            }
            if (sretAdapter) {
                // Adapter: borrow method returned a `ptr` (R*). Copy the
                // bytes into the thunk's sret slot — caller-owned memory —
                // so it acts like a value-return at the closure boundary.
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
            // Non-capturing — closure record can be a private global
            // constant `{ thunk, null, null }`. Globals live for the
            // whole program, so a static or unbound-instance method
            // reference can be returned / stored without escape worries.
            llvm::Constant* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            llvm::Constant* init = llvm::ConstantStruct::get(closureTy,
                {static_cast<llvm::Constant*>(thunk), nullPtr, nullPtr});
            auto* gv = new llvm::GlobalVariable(*lmod, closureTy,
                /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                init, thunkName + "_record");
            return gv;
        }

        // BOUND_INSTANCE — heap-allocate the captures struct + closure
        // record, populate with the evaluated receiver, and synthesize
        // a drop function that frees both at scope exit. Mirrors the
        // L3-3 lambda heap-closure path; the captured receiver is a
        // borrow (we don't free the receiver object itself).
        auto* outerBuilder = module->getBuilder();
        const llvm::DataLayout& dl = lmod->getDataLayout();
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc");
        llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
        if (!allocFn || !freeFn) {
            throw Exception(
                "runtime helpers __cajeta_alloc / __cajeta_free not linked",
                "CAJETA_ERROR_RUNTIME");
        }

        // 1. Allocate captures struct and store the evaluated receiver.
        std::vector<llvm::Type*> bindFields = {(llvm::Type*) ptrTy};
        llvm::StructType* capStructTy = llvm::StructType::get(
            llvmCtx, bindFields);
        uint64_t capBytes = dl.getTypeAllocSize(capStructTy);
        llvm::Value* capturesPtr = outerBuilder->CreateCall(allocFn, {
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx), capBytes),
        }, "captures");
        llvm::Value* recvValue = receiverExpr->generateCode(module);
        // L-value coerce the receiver: an IdentifierExpression yields its
        // alloca, which holds the heap pointer — load through to get the
        // actual ptr.
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(recvValue)) {
            recvValue = outerBuilder->CreateLoad(
                a->getAllocatedType(), a);
        }
        llvm::Value* recvStoreSlot = outerBuilder->CreateStructGEP(
            capStructTy, capturesPtr, 0, "captures.this");
        outerBuilder->CreateStore(recvValue, recvStoreSlot);

        // 2. Synthesize the per-ref drop function. Frees the captures
        //    struct and the closure record; the receiver itself isn't
        //    freed (borrow capture, original owner still has the entry).
        std::string dropName = thunkName + "_drop";
        llvm::FunctionType* dropFnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), {ptrTy}, /*isVarArg=*/false);
        llvm::Function* dropFn = llvm::Function::Create(dropFnTy,
            llvm::Function::ExternalLinkage, dropName, lmod);
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

        // 3. Allocate the closure record and populate fn / captures / drop_fn.
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

    llvm::Value* InstanceOfExpression::generateCode(CajetaModulePtr module) {
        // cajeta monomorphizes, so a value widened to a template wildcard
        // (`Tensor<?>`) is, at runtime, still its concrete instantiation. When the
        // static lhs type already pins the answer we fold it at compile time; when
        // the lhs is a wildcard (or a different instantiation of the same base —
        // i.e. a genuine reified downcast question) we emit a runtime RTTI check
        // (`__cajeta_instanceof_named`, reified-capture-spec.md §1–2).
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
        // The pre-pass resolver runs before LocalVariableDeclaration populates the scope,
        // so the lhs's resolvedType may be null. Re-run resolveTypes now — at codegen
        // time the scope is fully populated.
        if (!lhsExpr->getResolvedType()) {
            lhsExpr->resolveTypes(module);
        }
        CajetaTypePtr lhsType = lhsExpr->getResolvedType();

        std::string targetCanon = type->toCanonical();
        std::string lhsCanon = lhsType ? lhsType->toCanonical() : std::string();
        bool lhsIsWildcard = lhsCanon.find('?') != std::string::npos;
        bool targetConcrete = targetCanon.find('?') == std::string::npos;
        bool sameBase = lhsType && erasedBaseOf(lhsCanon) == erasedBaseOf(targetCanon);
        // A reified runtime check is warranted (and sound) when the lhs is a
        // wildcard, or a different instantiation of the same generic base, and the
        // target is a concrete class instantiation we can name.
        auto targetClass = dynamic_pointer_cast<CajetaClass>(type);
        bool wantRuntime = targetClass && targetConcrete && lhsType
            && lhsCanon != targetCanon && (lhsIsWildcard || sameBase);

        // Class-bounded-wildcard target `Base<? extends Bound>` (§5): not a
        // concrete instantiation (so `wantRuntime` is false), but still a real
        // reified question — does the container's reified element type conform to
        // the bound? Resolved at runtime by __cajeta_instanceof_bounded.
        std::string boundBaseCanon, boundCanon;
        int boundArgIdx = -1;
        bool wantBounded = boundedWildcardTarget(type, boundBaseCanon,
                                                 boundArgIdx, boundCanon)
            && lhsType && (lhsIsWildcard || sameBase);

        auto* builder = module->getBuilder();

        // We need the lhs object pointer for the runtime match and/or the
        // pattern binding; otherwise we only evaluate it for side-effects.
        bool needObj = wantRuntime || wantBounded || !pattern.empty();
        llvm::Value* objPtr = nullptr;
        if (needObj) {
            llvm::Value* raw = children[0]->generateCode(module);
            objPtr = loadIfLValue(module, raw, lhsExpr);
        } else {
            children[0]->generateCode(module);
        }

        // Compute the match bit. Runtime reified check when the static type
        // doesn't pin the answer; otherwise the folded compile-time constant.
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

        // Pattern binding (`w instanceof Foo<int32> f`): introduce `f : Foo<int32>`
        // bound to the captured pointer — representation-identical because cajeta
        // monomorphizes. Sound by construction: bind the object on a match, null
        // otherwise, so a use outside the matched region NPEs rather than reading a
        // mis-typed object. The binding aliases an existing object (no creator /
        // drop_push), so it is a borrow and never double-frees. The `if` evaluates
        // its condition before the then-block, so registering the field in the
        // current scope makes it visible to the guarded body.
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

    // Resolve a bracketed ancestor name (`this<Base>` / `super<Base>`)
    // against the current class's transitive ancestor closure. Returns
    // the resolved class pointer, or throws CAJETA_ERROR_NOT_AN_ANCESTOR
    // if the name doesn't match any reachable ancestor of `here`.
    // Comparison uses both canonical and short type names because the
    // bracket contents may be written either way at the call site.
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
        // Self is not its own ancestor for bracket purposes — `this<Self>`
        // is a no-op that's also a code smell; only allow real ancestors.
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

    void ThisExpression::resolveTypes(CajetaModulePtr module) {
        // `this` resolves to the current class type on the structure stack.
        // `this<Base>` resolves to the chosen ancestor instead — DotExpression
        // / MethodCallExpression then treat the receiver as Base-typed.
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

    llvm::Value* ThisExpression::generateCode(CajetaModulePtr module) {
        // Method::generateCode registers a ParameterField named "this" in the active scope
        // for non-static methods. We return its alloca (l-value style); consumers can
        // loadIfLValue if they need the pointer itself.
        //
        // Plain `this` returns the raw alloca (l-value). `this<Base>` must
        // return a Base-typed pointer adjusted to the sub-object — loaded
        // from the alloca and shifted by getSubObjectByteOffset(Base).
        // Returning the adjusted r-value (not an alloca) is fine because
        // DotExpression's class-receiver path detects the load-through and
        // GEPs from there.
        FieldPtr thisField = module->getScopeStack().peek()->getField("this");
        if (!thisField) return nullptr;
        auto alloca = thisField->getOrCreateAllocation();
        if (chosenAncestorName.empty()) {
            return static_cast<llvm::Value*>(alloca);
        }
        // Self-resolve when the pre-pass didn't (e.g., for expressions
        // inside LocalVariableDeclaration initializers — those aren't
        // visited by the pre-pass, so the first generateCode call
        // observes resolvedType == null). Without this, we'd fall
        // through to "return alloca" and the adjustment would be
        // silently skipped — `this<C>` would behave like plain `this`.
        if (!resolvedType) resolveTypes(module);
        // Load the current `this` pointer, then adjust to the chosen
        // ancestor sub-object. The adjustment is a no-op for the first-
        // parent chain (offset = 0) and for self.
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

    void SuperExpression::resolveTypes(CajetaModulePtr module) {
        // Plain `super` resolves to the current class's first declared
        // parent (matches Java's single-parent rule). `super<Base>`
        // resolves to the named ancestor, validated against the current
        // class's transitive parent set. In both forms the instance
        // pointer is `this` (possibly adjusted at codegen for non-first
        // parents); only the dispatch target differs.
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

    llvm::Value* SuperExpression::generateCode(CajetaModulePtr module) {
        // Plain `super` returns the raw `this` alloca (Phase 1 single-
        // parent case; pointer adjustment happens later in invokeMethod
        // when the declaring class differs from the receiver class).
        //
        // `super<Base>` returns a pointer adjusted to Base's sub-object
        // — same machinery as `this<Base>`. The downstream MCE detects
        // a SuperExpression receiver and force-direct-calls, so the
        // adjusted pointer flows straight into Base's method.
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
        // Same self-resolve guard as ThisExpression — initializer
        // sites bypass the pre-pass, so the bracketed form's
        // resolvedType may still be null on the first generateCode.
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

    // Sync-lowering MVP for the three concurrency expressions. They all wrap a
    // single inner expression in children[0]. resolveTypes mirrors the inner
    // expression's type so the outer context picks up the right type without
    // a Task<T> wrapper. generateCode just forwards to the inner expression
    // (await/spawn) or evaluates-and-discards (detach). The async runtime
    // (scheduler, state machines, Task<T> struct) layers on top later; the
    // AST surface stays stable.

    // R1 contract: `await` unwraps Task<T>* to T. If the inner expression
    // already resolved to T (a bare call to an async fn that hasn't been
    // wrapped, or a sync-compat pass-through), await acts as the identity.
    // The strict "await only on Task<T>" check lands in R3 when the type
    // rewrite for async fn returns is in place.
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

    llvm::Value* AwaitExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return nullptr;
        llvm::Value* v = inner->generateCode(module);
        if (!v) return nullptr;
        // Re-resolve the inner at codegen time. For an identifier referring
        // to a local Task<T> declared later in the same method, the pre-
        // pass resolveTypes ran before the local was in scope and left
        // resolvedType null — the dynamic_pointer_cast below would then
        // miss and we'd fall into the sync-compat branch, returning the
        // raw Task ptr instead of unwrapping. Same pattern DotExpression
        // and BinaryOpExpression ASSIGN use for their receiver lookups.
        if (!inner->getResolvedType()) {
            inner->resolveTypes(module);
        }
        auto innerType = inner->getResolvedType();
        auto task = dynamic_pointer_cast<CajetaTask>(innerType);
        if (!task) {
            // Sync-compat: inner is a plain value (not a Task wrapper).
            // Identity-load through allocas so the caller sees the value.
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                return module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
            }
            return v;
        }
        // Task<T>* in hand. R2: block until the worker has flipped the
        // done flag, then load .value. The Task ptr may be in an alloca
        // slot (when the spawn result was bound to a local) — load
        // through to get the heap ptr first.
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
        // R5/Error-model #205: post-wait, check the task's exception slot.
        // If non-null, re-raise into the awaiter's frame — the thrown
        // value flows up via the existing setjmp/longjmp machinery, same
        // as a direct throw. If NULL (success path), load .value and
        // return the unwrapped result.
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
        // Clear the exception slot BEFORE throwing so the surrounding
        // scope_exit_to doesn't re-raise the same exception when the
        // user has already caught it via try/catch around the await.
        // Without this, function-return-after-handled-throw triggers a
        // ghost re-raise from the scope frame's exception walk.
        builder->CreateStore(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
            excSlot);
        if (llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw")) {
            builder->CreateCall(throwFn, {excPtr});
        }
        // Throw is noreturn from the user's POV; the runtime longjmps. Emit
        // unreachable so LLVM knows this path doesn't continue. The
        // normal-path block has the actual return value.
        builder->CreateUnreachable();

        builder->SetInsertPoint(normalBB);
        llvm::Value* valueSlot = builder->CreateStructGEP(
            task->getLlvmType(), v, CajetaTask::VALUE_FIELD_INDEX, "task_value");
        return builder->CreateLoad(
            task->getLlvmType()->getStructElementType(CajetaTask::VALUE_FIELD_INDEX),
            valueSlot);
    }

    void SpawnExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) return;
        auto inner = dynamic_pointer_cast<Expression>(children[0]);
        if (!inner) return;
        auto innerType = inner->getResolvedType();
        // spawn `T-returning-call` materializes a Task<T>. If T is itself
        // already Task<T'> (i.e., calling a hypothetically-typed async fn
        // that returned a Task), keep the existing wrapper — don't double-
        // box. For sync MVP backward-compat (the inner fn returns plain T),
        // we synthesize the Task<T> here.
        if (auto task = dynamic_pointer_cast<CajetaTask>(innerType)) {
            resolvedType = task;
        } else if (innerType) {
            resolvedType = CajetaTask::getOrCreate(module, innerType);
        }
    }

    // R3-A spawn lowering: lift R2's zero-argument restriction. Each arg is
    // evaluated at the spawn site (outer/main thread) so any side effects
    // happen there, not on the worker. The values are packed into a heap-
    // allocated context struct alongside the Task<T> pointer; the trampoline
    // loads from this context and routes the call through the target class's
    // invokeMethod path (same dispatch as a regular method call would emit,
    // minus MethodCallExpression's outer-thread arg evaluation).
    //
    // Two inner-call shapes are supported:
    //   - Bare class-method (`spawn compute(args)`): trampoline routes
    //     through the enclosing class's invokeMethod.
    //   - Spawn-of-lambda (`spawn body(args)` where `body` is a function-
    //     typed scope field — local, parameter, or capture): trampoline
    //     loads the closure record at fn+captures (L3-3 ABI) and
    //     indirect-dispatches. Lets `withTimeout(d, () -> compute())`
    //     work — see docs/specification/concurrent/Concurrency.md § withTimeout, and the
    //     #-transfer lifetime invariant in docs/specification/concurrent/AsyncStatus.md
    //     § Plan: Task<T>.
    //   - Instance-method dispatch (`spawn obj.method()`) is still
    //     deferred — it needs the receiver captured into the ctx struct
    //     and a dispatch through the class's vtable; not in scope here.
    //
    // Pure intrinsics inside spawn are also out of scope (no useful
    // semantics).
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
        // Instance-method dispatch (`spawn obj.method()`) needs the receiver
        // captured into the context struct too — deferred.
        if (!innerCall->getChildren().empty()) {
            throw Exception(
                "spawn currently doesn't support instance-method calls; use "
                "a bare class-method invocation",
                "CAJETA_ERROR_ASYNC_R3A");
        }

        // Detect spawn-of-lambda: methodCallName resolves to a function-
        // typed scope field (local, parameter, or capture). The body's
        // trampoline will indirect-call through the closure instead of
        // invokeMethod. The detection has to run before arg evaluation so
        // the closure value gets captured at the spawn site (outer thread),
        // matching the same-thread-side-effects rule the regular spawn
        // observes for its method-args.
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

        // v1 spawn-of-lambda restriction: only the heap-ownership ABI
        // (`(P) -> #R`) and primitive-return shapes. The sret form would
        // need the result slot to outlive the worker's trampoline frame,
        // which today's Task<T>::value slot layout (which holds either a
        // primitive or a heap pointer, not a struct) doesn't carry.
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

        // Step 1: Evaluate every arg at the spawn site. Any side effects
        // (mutation through `#`, calls with effects, etc.) happen on the
        // calling thread; the worker only sees the resulting values via
        // the context struct. Each capture records both the LLVM value
        // and the CajetaType, since invokeMethod's overload resolution
        // walks the latter.
        vector<llvm::Value*> capturedArgs;
        vector<CajetaTypePtr> capturedArgTypes;

        // Spawn-of-lambda: also capture the closure record ptr from the
        // lambdaClosureField's slot. This becomes capturedArgs[0]; the
        // trampoline reads it from ctx[1] (ctx[0] is the task ptr) and
        // GEPs fn/captures off the closure. CajetaType for the closure
        // is the function type itself — capturedArgTypes positionally
        // tracks types but the lambda path doesn't use invokeMethod's
        // overload resolution, so the stored type is decorative beyond
        // round-tripping the LLVM ptr.
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
            // l-value → r-value coercion. The narrow AllocaInst-only load
            // covers the IdentifierExpression case, but argument expressions
            // can also be ArrayIndexExpression (a GEP into the array's data
            // region) or DotExpression (a GEP into a struct field). In both
            // cases what's returned is a slot pointer and the call site
            // wants the stored value. Without this, e.g. `spawn f(arr[0])`
            // for a Counter[] would pass the SLOT ADDRESS to the worker
            // (not the heap Counter pointer), and the worker's c.inc()
            // would mutate the slot bytes rather than the Counter.
            auto exprAst = dynamic_pointer_cast<Expression>(param.expression);
            v = loadIfLValue(module, v, exprAst);
            CajetaTypePtr t = param.expression->getResolvedType();
            if (!t) t = CajetaType::of(v);
            capturedArgs.push_back(v);
            capturedArgTypes.push_back(t);
        }
        // Capture the outer block AFTER arg evaluation. An arg expression
        // may have emitted its own basic blocks (e.g. ArrayIndexExpression's
        // bounds-check ok/fail split), leaving the builder positioned on a
        // fresh BB. The spawn-site setup that runs after the trampoline
        // body must continue from THAT block, not the BB that was current
        // before arg eval (which is already terminated by the args' own
        // control flow). Without this, the post-trampoline restore lands
        // on the pre-args BB whose terminator we just emitted, and the
        // ok-branch from the bounds check is left without a terminator —
        // JIT verify fails with "Basic Block ... does not have terminator".
        llvm::BasicBlock* outerInsertBlock = outerBuilder->GetInsertBlock();

        // Step 2: Build the context struct {ptr task, arg0, arg1, ...}.
        // Anonymous literal struct — LLVM unifies structurally identical
        // ones so the spawn-site stores and trampoline-side loads agree.
        vector<llvm::Type*> ctxFieldTypes = {ptrTy};
        for (auto* v : capturedArgs) {
            ctxFieldTypes.push_back(v->getType());
        }
        llvm::StructType* ctxStructTy =
            llvm::StructType::get(llvmCtx, ctxFieldTypes);

        // Step 3: Resolve the target class (bare-call form): the enclosing
        // class from the structure stack, same heuristic MethodCallExpression
        // uses for receiver-less invocations.
        if (module->getStructureStack().empty()) return nullptr;
        CajetaClassPtr targetClass = module->getStructureStack().back();

        // Step 4: Synthesize the trampoline. Signature: void (ptr ctx).
        // The runtime is type-agnostic — it just calls trampoline(ctx).
        static thread_local uint64_t trampolineCounter = 0;  // per-thread (threadsafe U4)
        string trampName = string("__cajeta_spawn_trampoline_")
            + std::to_string(trampolineCounter++);
        llvm::FunctionType* trampTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvmCtx), {ptrTy}, false);
        llvm::Function* trampFn = llvm::Function::Create(
            trampTy, llvm::Function::ExternalLinkage, trampName, lmod);
        llvm::BasicBlock* trampEntry = llvm::BasicBlock::Create(
            llvmCtx, "entry", trampFn);
        outerBuilder->SetInsertPoint(trampEntry);

        llvm::Value* ctxParam = trampFn->arg_begin();
        // Load task ptr from ctx[0].
        llvm::Value* taskSlot = outerBuilder->CreateStructGEP(
            ctxStructTy, ctxParam, 0, "ctx_task_slot");
        llvm::Value* taskPtr = outerBuilder->CreateLoad(
            ptrTy, taskSlot, "task_ptr");

        // R5/Error-model #205: wrap the inner call in a try/catch inside
        // the trampoline so a thrown RecoverableException is captured onto
        // the Task's exception slot rather than escaping the fiber's
        // stack and crashing the carrier. Matches the TryStatement
        // setjmp/__cajeta_exc_push pattern, but with a synthetic body:
        // the try-body is the invokeMethod + result store; the catch
        // reads the thrown ptr and stashes it on task->exception.
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
        llvm::Function* excPushFn = module->getRuntimeFunction("__cajeta_exc_push");
        llvm::Function* excPopFn  = module->getRuntimeFunction("__cajeta_exc_pop");
        llvm::Function* getThrownFn = module->getRuntimeFunction("__cajeta_get_thrown");
        llvm::Function* setjmpFn = lmod->getFunction("setjmp");
        if (!setjmpFn) {
            llvm::FunctionType* sjt = llvm::FunctionType::get(
                i32Ty, {ptrTy}, false);
            setjmpFn = llvm::Function::Create(sjt,
                llvm::Function::ExternalLinkage, "setjmp", lmod);
            setjmpFn->addFnAttr(llvm::Attribute::ReturnsTwice);
        }

        // Exception frame at trampoline entry — 512-byte blob covers
        // jmp_buf + prev + thrown_value on the targets we support; same
        // sizing TryStatement uses.
        constexpr unsigned frameBytes = 512;
        llvm::IRBuilder<> trampEntryBuilder(trampEntry, trampEntry->begin());
        llvm::Value* trampFrame = trampEntryBuilder.CreateAlloca(
            llvm::ArrayType::get(i8Ty, frameBytes), nullptr, "spawn_exc_frame");

        llvm::BasicBlock* trampTryBB = llvm::BasicBlock::Create(
            llvmCtx, "tramp_try", trampFn);
        llvm::BasicBlock* trampCatchBB = llvm::BasicBlock::Create(
            llvmCtx, "tramp_catch", trampFn);
        llvm::BasicBlock* trampFinishBB = llvm::BasicBlock::Create(
            llvmCtx, "tramp_finish", trampFn);

        if (excPushFn) {
            outerBuilder->CreateCall(excPushFn, {trampFrame});
        }
        llvm::Value* sjResult = outerBuilder->CreateCall(setjmpFn, {trampFrame});
        llvm::Value* threwInTramp = outerBuilder->CreateICmpNE(sjResult,
            llvm::ConstantInt::get(i32Ty, 0));
        outerBuilder->CreateCondBr(threwInTramp, trampCatchBB, trampTryBB);

        // --- try body: dispatch the inner call + capture result ---
        outerBuilder->SetInsertPoint(trampTryBB);
        llvm::Value* innerResult = nullptr;
        CajetaTypePtr innerType;
        if (lambdaFnType) {
            // Spawn-of-lambda: load the closure ptr from ctx[1], GEP
            // fn+captures (L3-3 closure layout: { ptr fn, ptr captures,
            // ptr drop_fn }), build the indirect-call arg list, dispatch.
            // Mirrors MethodCallExpression's function-typed-local
            // invocation lowering (the `add(3, 4)` case) but inside the
            // worker frame and reading the closure from ctx instead of
            // the caller's scope field.
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
            // Build args: [captures, user_arg0, user_arg1, ...]. The
            // L3-3 ABI puts captures at position 0 in the closure's
            // function signature.
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
                // Width-coerce to match the signature, mirroring the
                // MCE direct-closure-call path.
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
            // Bare class-method dispatch — the original lowering path.
            // Load each arg from ctx[i+1] and build the entries
            // invokeMethod expects.
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
            // For static methods, thisValue is nullptr. Pass `module` as the
            // caller module so the worker call is emitted with THIS module's
            // builder/insert point (the trampoline we just built) — not the
            // receiver class's. For a stdlib-template worker resolved via the
            // uninstantiated `ParallelDriver` template (structureStack.back()),
            // the class's own emit module is the cached stdlib, whose builder
            // points at some unrelated stdlib function; using it would emit the
            // call into the wrong function ("instruction in another function").
            innerResult = targetClass->invokeMethod(
                methodNameCopy, entries, /*isConstructor=*/false,
                /*thisValue=*/nullptr, /*callerModule=*/module);
            if (innerResult) {
                innerType = CajetaType::of(innerResult);
                // CajetaType::of can't recover a class type from an opaque-
                // pointer call result: a method returning `#SomeClass` lowers
                // to a bare `ptr`, so of() yields null. Ask the receiver class
                // for the method's DECLARED return type instead. Without this,
                // spawn of ANY object-returning method tripped the `!innerType`
                // bail below, which abandoned tramp_try unterminated and JIT
                // verify rejected the module ("Basic Block %tramp_try does not
                // have terminator").
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
            // Resolution failed after the trampoline shell (entry + try/catch/
            // finish blocks) was already emitted. Returning nullptr here would
            // leave tramp_try unterminated and fail JIT verify with a confusing
            // message; throw a clear diagnostic instead (the partial module is
            // discarded with the failed compile).
            outerBuilder->SetInsertPoint(outerInsertBlock);
            throw Exception(
                "spawn target `" + innerCall->getMethodCallName()
                + "` could not be resolved to a callable method",
                "CAJETA_ERROR_ASYNC_SPAWN_UNRESOLVED");
        }
        auto task = CajetaTask::getOrCreate(module, innerType);
        llvm::Type* taskTy = task->getLlvmType();
        // For void-returning inner functions there's nothing to store
        // (LLVM forbids store of a void value). The Task<void> wrapper
        // still tracks done/exception/fiber; the value slot just stays
        // at its zero placeholder. await on a void Task already does
        // the right thing — the value GEP is unused on that path
        // because the result type has no LLVM representation.
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
        // Error-model #210: if the thrown value is Unrecoverable, the
        // runtime helper aborts the process (alarm semantics — runtime
        // invariant violations must not propagate through await). If
        // Recoverable, the helper returns and we store on the Task slot
        // for await to re-raise.
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

        // Trampoline complete — restore outer cursor and emit the spawn-
        // site setup (task + ctx allocations, store captured args, enqueue).
        outerBuilder->SetInsertPoint(outerInsertBlock);
        resolvedType = task;
        const llvm::DataLayout& dl = lmod->getDataLayout();
        // Allocate the Task<T> on the heap.
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
        // R5/Error-model #205: zero the exception slot so the success path
        // (no throw) leaves NULL there. The trampoline only writes this
        // slot on the catch branch.
        llvm::Value* excInit = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::EXCEPTION_FIELD_INDEX,
            "task_exception_init");
        outerBuilder->CreateStore(
            llvm::ConstantPointerNull::get(ptrTy), excInit);
        // R5-C: zero the fiber slot too. __cajeta_task_run writes the
        // freshly-allocated fiber here so scope's cancellation walk can
        // find it.
        llvm::Value* fiberSlot = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::FIBER_FIELD_INDEX,
            "task_fiber_init");
        outerBuilder->CreateStore(
            llvm::ConstantPointerNull::get(ptrTy), fiberSlot);
        // Wire the Task into the drop chain. Without this, the heap
        // struct malloced above leaks until process exit (documented in
        // AsyncStatus.md § Known gaps before this commit). The drop fn
        // CajetaTask::getOrCreateDropFunction synthesizes waits for the
        // task to complete before freeing — safe under both the normal
        // fall-through path (where __cajeta_scope_exit_to has already
        // joined the task) and the exception-unwind path (where it
        // hasn't). Drop-entry alloca lives in the function entry block
        // so its address is stable across the function's lifetime;
        // re-execution of the spawn site (e.g. a loop body) pushes and
        // pops the same entry per iteration, which is the same pattern
        // array-local drops already use.
        //
        // detachMode skips this — `detach` deliberately opts out of
        // scope-anchored cleanup (docs/specification/concurrent/Concurrency.md § detach semantics).
        // The Task wrapper leaks for the process lifetime; the body's
        // own locals still drop normally on the carrier-side chain.
        if (!detachMode) {
            // Pick the push variant + entry size based on the CompilerFlags
            // (docs/CompilerModes.md). Mirrors the LVD path's choice
            // so the chain has uniformly-shaped entries within a build.
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
                    // Publish for ownership-transfer call sites — assignment
                    // to a named local marks this entry inactive so the
                    // local's class-instance drop becomes the canonical owner.
                    dropEntry = dropEntryPtr;
                }
            }
        }
        // Allocate the ctx struct on the heap and populate it.
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
        // R5-A: register the task with the innermost enclosing scope so
        // its closing `}` will wait for this task before returning.
        // R5-D: also pass the exception slot so scope_exit can walk it
        // post-wait and re-raise any caught throw via the doc's first-
        // throw-wins escalation. R5-C: also the fiber slot so scope can
        // cancel remaining siblings when one throws.
        //
        // detachMode skips scope_register (the whole point of detach is
        // to escape the enclosing scope). The fiber-slot GEP stays —
        // __cajeta_task_run needs the slot address whether or not the
        // scope is waiting on it. Cancellation via scope's first-throw
        // walk is silently inapplicable for detached tasks: nothing
        // registered them, so scope's iteration never finds them.
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
            // --lazy-scope: the prologue skipped the implicit function-body
            // frame, so ensure one exists before this (possibly bare) spawn
            // registers. No-op when an enclosing `scope { }` or an earlier bare
            // spawn already pushed a frame (the runtime compares *top to the
            // method's entry watermark). Off by default → identical codegen.
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
            if (llvm::Function* regFn = module->getRuntimeFunction(
                    "__cajeta_scope_register")) {
                outerBuilder->CreateCall(regFn,
                    {doneRegSlot, excRegSlot, fiberRegSlot});
            }
        }
        // R5-C: __cajeta_task_run writes the freshly-allocated fiber's
        // pointer into the task's fiber slot before enqueueing — so
        // scope's later cancellation walk can find the fiber. Pass the
        // slot address.
        if (llvm::Function* runFn = module->getRuntimeFunction(
                "__cajeta_task_run")) {
            outerBuilder->CreateCall(runFn,
                {ctxInstance, trampFn, fiberRegSlot});
        }
        // detach is fire-and-forget — no Task handle escapes back to
        // user code (DetachExpression::resolveTypes already typed the
        // surrounding expression as void).
        if (detachMode) return nullptr;
        return taskInstance;
    }

    void DetachExpression::resolveTypes(CajetaModulePtr module) {
        AbstractSyntaxNode::resolveTypes(module);
        // `detach` is fire-and-forget — the surrounding expression context
        // gets no value (void). The inner is still type-resolved so its
        // method-call shape is valid.
        if (!children.empty()) {
            if (auto inner = dynamic_pointer_cast<Expression>(children[0])) {
                inner->resolveTypes(module);
            }
        }
        resolvedType = CajetaType::of("void");
    }

    // docs/specification/concurrent/Concurrency.md § detach semantics requires every argument to the
    // detached call to be either a #-transferred value, a primitive,
    // or a fresh allocator (NewExpression — auto-promoted in transfer
    // position per MemoryModel.md § Borrow / transfer rules). A bare
    // class-typed identifier or any other expression that yields a
    // heap pointer without an explicit transfer marker is rejected:
    // the detached fiber outlives the spawning scope, so a borrow's
    // lifetime can't be guaranteed.
    static void enforceDetachMoveOnlyCaptures(const std::shared_ptr<MethodCallExpression>& innerCall) {
        for (auto& param : innerCall->getParameters()) {
            auto expr = param.expression;
            if (!expr) continue;
            // (a) Explicit caller-side #-transfer at the argument slot
            // (Phase 1 of #68 — the canonical shape). Pre-Phase-1 the
            // REFERENCE was parsed as a MoveExpression-wrapped child;
            // post-Phase-1 it's captured on the MethodCallParameter
            // itself via callerTransferred. Accept both: the new flag
            // is authoritative going forward, and the legacy wrapping
            // still appears in non-argument contexts (assignment RHS,
            // return expressions).
            if (param.callerTransferred) continue;
            if (dynamic_pointer_cast<MoveExpression>(expr)) continue;
            // (b) Fresh allocator — anonymous new, auto-promoted.
            if (dynamic_pointer_cast<NewExpression>(expr)) continue;
            // (c) Primitive value — pass-by-value, no aliasing.
            auto t = expr->getResolvedType();
            if (t && (t->getTypeFlags() & PRIMITIVE_FLAG)) continue;
            // Anything else: heap class without an explicit transfer.
            // Surface a clean error with the offending argument's
            // expression text where we can extract it.
            string detail = t ? t->toCanonical() : string("<unresolved>");
            throw Exception(
                "detach argument must be #-transferred, primitive, or a fresh "
                "`heap T(...)`; got an expression of type '" + detail + "' that "
                "would be captured as a borrow",
                "CAJETA_ERROR_DETACH_BORROW_CAPTURE");
        }
    }

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
        // Resolve types on the inner first so the capture check sees
        // each argument's resolved type. resolveTypes is idempotent.
        for (auto& param : innerCall->getParameters()) {
            if (param.expression && !param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
        }
        enforceDetachMoveOnlyCaptures(innerCall);
        // Reuse SpawnExpression's full lowering — same trampoline, same
        // fiber enqueue, same Task struct. Detach-mode flag skips
        // scope_register and drop_push so the task escapes scope-anchored
        // cleanup. See docs/specification/concurrent/Concurrency.md § detach semantics for the
        // implementation-vs-spawn delta. Passing nullptr for the token
        // is fine: SpawnExpression doesn't retain it beyond extracting
        // source-position metadata for diagnostics, which here would
        // duplicate this DetachExpression's metadata anyway.
        auto spawn = make_shared<SpawnExpression>(nullptr);
        spawn->addChild(innerCall);
        spawn->setDetachMode(true);
        spawn->resolveTypes(module);
        spawn->generateCode(module);
        return nullptr;
    }
}
