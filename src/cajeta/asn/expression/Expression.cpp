//
// Created by James Klappenbach on 3/19/22.
//

#include "Expression.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/field/StackField.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/util/LiteralUtils.h"
#include "cajeta/util/MemoryManager.h"
#include "cajeta/asn/expression/Identifier.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaTask.h"
#include "cajeta/error/ExplicitCastRequiredException.h"
#include "cajeta/error/InvalidOperandException.h"
#include "BinaryOpExpression.h"
#include "DotExpression.h"
#include "LiteralExpression.h"
#include "MethodCallExpression.h"
#include "NewExpression.h"
#include "../Block.h"
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
            // Method reference: `expr::id`, `Type::id`, or `Type::new`.
            // Check before NEW and identifier so we don't mis-route
            // those token-bearing forms. Detect the LHS shape:
            //   - typeType form (`Type::id` / `Type::new`): grammar
            //     captured a typeType in front of the COLONCOLON.
            //   - expression form (`obj::id`): grammar captured an
            //     expression in front.
            CajetaTypePtr methodRefRecvType;
            ExpressionPtr methodRefRecvExpr;
            std::string methodRefName;
            bool methodRefIsCtor = ctx->NEW() != nullptr;
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
            } else if (ctx->explicitTemplateInvocation()) {
                result = make_shared<UnsupportedExpression>("explicit template invocation", token);
            } else if (ctx->innerCreator()) {
                result = make_shared<UnsupportedExpression>(
                    "inner-class instantiation (obj.new Inner())", token);
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
        } else if (ctx->NEW()) {
            result = make_shared<NewExpression>(ctx->creator(), token);
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), ctx->primary() != nullptr);
        } else if (ctx->LPAREN()) {
            // Cast: '(' annotation* typeType ('&' typeType)* ')' expression
            // We don't yet support intersection casts (multiple typeTypes); take the first.
            if (!ctx->typeType().empty()) {
                CajetaTypePtr destType = CajetaType::fromContext(ctx->typeType(0), nullptr);
                result = make_shared<CastExpression>(destType, token);
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
            // `expr instanceof Type` — target type comes from typeType (the pattern form
            // of instanceof, which binds a name, is treated as the same shape for now).
            CajetaTypePtr targetType = ctx->typeType().empty()
                ? CajetaTypePtr()
                : CajetaType::fromContext(ctx->typeType(0), nullptr);
            string patternName = ctx->pattern() ? ctx->pattern()->getText() : string();
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
        // yields `int[]`; indexed again yields `int`.
        if (!children.empty()) {
            if (auto exprChild = dynamic_pointer_cast<Expression>(children[0])) {
                if (auto arr = dynamic_pointer_cast<CajetaArray>(exprChild->getResolvedType())) {
                    resolvedType = arr->getElementType();
                }
            }
        }
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

        // Resolve the array value (the header pointer).
        //   - Local-variable arrays: an alloca holding a `ptr` to the header. Load to get
        //     the header pointer.
        //   - Nested ArrayIndex (`arr[i][j]`): the parent ArrayIndex gave us a slot whose
        //     element is a `ptr` to the inner header — load `ptr` to get that.
        //   - Anything else (e.g. method-call returning an array): the value already IS
        //     the header pointer.
        llvm::Value* arrayVal = children[0]->generateCode(module);
        auto lhsExpr = dynamic_pointer_cast<Expression>(children[0]);
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(arrayVal)) {
            arrayVal = builder->CreateLoad(a->getAllocatedType(), a);
        } else if (lhsExpr && dynamic_pointer_cast<ArrayIndexExpression>(lhsExpr)) {
            arrayVal = builder->CreateLoad(
                llvm::PointerType::get(ctx, 0), arrayVal);
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

        // Resolve the index expression.
        llvm::Value* idx = children[1]->generateCode(module);
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(idx)) {
            idx = builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (idx->getType() != i64Ty) {
            idx = builder->CreateIntCast(idx, i64Ty, /*isSigned=*/true);
        }

        // Bounds check (when enabled by the compiler flag and the runtime helper is
        // linked): load the size field and branch to fail if `idx >= size` under
        // unsigned comparison (catches negatives). Disabled via `cajeta --bounds=off`.
        llvm::Function* boundsFail = module->isBoundsCheckEnabled()
            ? module->getRuntimeFunction("__cajeta_array_bounds_fail")
            : nullptr;
        if (boundsFail) {
            llvm::Value* sizePtr = builder->CreateStructGEP(headerTy, arrayVal,
                CajetaArray::SIZE_FIELD_INDEX);
            llvm::Value* size = builder->CreateLoad(i64Ty, sizePtr);
            llvm::Value* outOfBounds = builder->CreateICmpUGE(idx, size);
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::BasicBlock* failBB = llvm::BasicBlock::Create(ctx, "bounds_fail", parentFn);
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx, "bounds_ok", parentFn);
            builder->CreateCondBr(outOfBounds, failBB, okBB);
            builder->SetInsertPoint(failBB);
            builder->CreateCall(boundsFail, {idx, size});
            builder->CreateUnreachable();
            builder->SetInsertPoint(okBB);
        }

        // Element address: &header->data[idx]. GEP walks ptr -> struct -> data array -> element.
        vector<llvm::Value*> gepIndices = {
            llvm::ConstantInt::get(i64Ty, 0),
            llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
            idx,
        };
        return builder->CreateGEP(headerTy, arrayVal, gepIndices);
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
        return {nullptr, raw};
    }

    llvm::Value* PrefixExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        auto* builder = module->getBuilder();
        auto [addr, val] = loadOperand(module, children[0]);
        if (!val) return nullptr;
        llvm::Type* ty = val->getType();

        switch (op) {
            case PREFIX_OP_POSITIVE:
                return val;
            case PREFIX_OP_NEGATIVE:
                return ty->isFloatingPointTy() ? builder->CreateFNeg(val) : builder->CreateNeg(val);
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
                llvm::Value* newVal;
                if (op == PREFIX_OP_INC) {
                    newVal = ty->isFloatingPointTy() ? builder->CreateFAdd(val, one)
                                                     : builder->CreateAdd(val, one);
                } else {
                    newVal = ty->isFloatingPointTy() ? builder->CreateFSub(val, one)
                                                     : builder->CreateSub(val, one);
                }
                builder->CreateStore(newVal, addr);
                return newVal;
            }
        }
        return nullptr;
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
        llvm::Type* srcTy = val->getType();
        llvm::Type* dstTy = destType->getLlvmType();
        if (srcTy == dstTy) return val;

        bool srcInt = srcTy->isIntegerTy();
        bool dstInt = dstTy->isIntegerTy();
        bool srcFp  = srcTy->isFloatingPointTy();
        bool dstFp  = dstTy->isFloatingPointTy();
        bool srcPtr = srcTy->isPointerTy();
        bool dstPtr = dstTy->isPointerTy();
        unsigned long destFlags = destType->getTypeFlags();
        bool destSigned = (destFlags & SIGNED_FLAG) != 0;

        if (srcInt && dstInt) {
            return builder->CreateIntCast(val, dstTy, destSigned);
        }
        if (srcFp && dstFp) {
            return builder->CreateFPCast(val, dstTy);
        }
        if (srcInt && dstFp) {
            return destSigned ? builder->CreateSIToFP(val, dstTy)
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
        if (!val || !addr) return val;
        llvm::Type* ty = val->getType();
        llvm::Value* one = ty->isFloatingPointTy()
            ? (llvm::Value*) llvm::ConstantFP::get(ty, 1.0)
            : (llvm::Value*) llvm::ConstantInt::get(ty, 1);
        llvm::Value* newVal;
        if (op == POSTFIX_OP_INC) {
            newVal = ty->isFloatingPointTy() ? builder->CreateFAdd(val, one)
                                             : builder->CreateAdd(val, one);
        } else {
            newVal = ty->isFloatingPointTy() ? builder->CreateFSub(val, one)
                                             : builder->CreateSub(val, one);
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
//    NonWildcardTypeArgumentsContext *nonWildcardTypeArguments();
//    ExplicitTemplateInvocationSuffixContext *explicitTemplateInvocationSuffix();
//    ArgumentsContext *arguments();
    ExpressionPtr PrimaryExpression::fromContext(CajetaParser::PrimaryContext* ctx) {
        ExpressionPtr result = nullptr;
        if (ctx->LPAREN()) {
            result = Expression::fromContext(ctx->expression());
        } else if (ctx->literal()) {
            result = LiteralExpression::fromContext(ctx->literal());
        } else if (ctx->identifier()) {
            result = make_shared<IdentifierExpression>(ctx->identifier(), true);
        } else if (ctx->THIS()) {
            // Pass the Token directly — ctx is a PrimaryContext here, so
            // ctx->expression() is null for the THIS form and the old
            // ThisExpression(ctx->expression()) overload would null-deref
            // on getStart(). The token-taking overload sidesteps that.
            result = make_shared<ThisExpression>(ctx->getStart());
        } else if (ctx->SUPER()) {
            // `super` as a primary expression (e.g. `super.foo()`); the actual call
            // dispatch is the bigger feature we haven't built yet.
            result = make_shared<UnsupportedExpression>("super call", ctx->getStart());
        }
        return result;
    }

    llvm::Value* PrimaryExpression::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    // Helper used by ternary/instanceof: load value from an alloca-style l-value.
    static llvm::Value* loadIfAllocaShared(CajetaModulePtr module, llvm::Value* v) {
        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
            return module->getBuilder()->CreateLoad(a->getAllocatedType(), a);
        }
        return v;
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
        llvm::Value* cond = loadIfAllocaShared(module, children[0]->generateCode(module));
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
        llvm::Value* thenVal = loadIfAllocaShared(module, children[1]->generateCode(module));
        llvm::BasicBlock* thenEnd = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(elseBB);
        llvm::Value* elseVal = loadIfAllocaShared(module, children[2]->generateCode(module));
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

    static int64_t lambdaCounter = 0;

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
    // cajeta-docs/Lambdas.md: ownership moves into the closure and the
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
        for (auto& c : node->getChildren()) {
            collectTransferNames(c, out);
        }
    }

    // Rule 5 from cajeta-docs/Lambdas.md: writing to a primitive that was
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
        for (auto& c : node->getChildren()) {
            enforceValueCaptureImmutability(c, valueCapturedNames);
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
        if (body) body->resolveTypes(module);

        // Determine the lambda's CajetaFunctionType. Preferred order:
        //   1. expectedType (a CajetaFunctionType from the surrounding
        //      context, e.g. the LHS of a function-typed variable
        //      declaration that called setExpectedType before resolveTypes).
        //   2. The body's resolvedType, when populated (not all
        //      Expression subclasses pin their resolvedType — works for
        //      identifiers and literals, less for arithmetic expressions).
        //   3. Fallback: void. A nonsensical lambda surfaces at codegen.
        CajetaTypePtr ret;
        if (auto expectedFn = std::dynamic_pointer_cast<CajetaFunctionType>(expectedType)) {
            ret = expectedFn->getReturnType();
        }
        if (!ret) {
            // Only Expression bodies expose a resolvedType — block bodies
            // require an explicit LHS expectedType (or void) since their
            // shape doesn't map to a single expression's resolved type.
            if (auto bexpr = std::dynamic_pointer_cast<Expression>(body)) {
                ret = bexpr->getResolvedType();
            }
        }
        if (!ret) ret = CajetaType::of("void");
        std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret);
        auto& cmap = CajetaType::getCanonicalMap();
        auto it = cmap.find(canon);
        if (it != cmap.end()) {
            resolvedType = it->second;
        } else {
            auto fnType = std::make_shared<CajetaFunctionType>(
                module, paramTypes, ret);
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

        auto* lmod = module->getLlvmModule();
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
                // Skip function-typed captures for now — capturing a
                // closure inside another closure needs careful thinking
                // about closure-record lifetime and isn't part of L2-3.
                if (std::dynamic_pointer_cast<CajetaFunctionType>(t)) continue;
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

        llvm::Function* fn = llvm::Function::Create(
            fnType->getLlvmFunctionType(),
            llvm::Function::ExternalLinkage,
            synthesizedName,
            lmod);

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
            // arg i + 1.
            auto formal = std::make_shared<FormalParameter>(
                paramNames[i], paramTypes[i]);
            auto field = std::make_shared<ParameterField>(
                module, formal, fn, (int) i + 1);
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
            llvm::Value* capArg = fn->getArg(0);
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
            if (tail && !tail->getTerminator()) {
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

    static int64_t methodRefCounter = 0;

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
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret);
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canon);
            if (it != cmap.end()) {
                resolvedType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, paramTypes, ret);
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
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret);
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canon);
            if (it != cmap.end()) {
                resolvedType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, paramTypes, ret);
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
            std::string canon = CajetaFunctionType::buildCanonical(paramTypes, ret);
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canon);
            if (it != cmap.end()) {
                resolvedType = it->second;
            } else {
                auto fnType = std::make_shared<CajetaFunctionType>(
                    module, paramTypes, ret);
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
            auto* lmod = module->getLlvmModule();
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
                // virtual dispatch on the new instance).
                if (llvm::GlobalVariable* vtable = targetClass->getVirtualTableGlobal()) {
                    llvm::Value* vptrSlot = tb.CreateStructGEP(
                        structTy, instance, /*idx=*/0, "vtable_slot");
                    tb.CreateStore(vtable, vptrSlot);
                }

                // Pass the new instance as `this`, then the thunk's
                // explicit args (1..) as the user-facing ctor params.
                std::vector<llvm::Value*> ctorArgs;
                ctorArgs.push_back(instance);
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = 1; i < thunkArgCount; ++i) {
                    ctorArgs.push_back(thunk->getArg(i));
                }
                tb.CreateCall(ctor->getLlvmFunctionType(),
                    ctor->getLlvmFunction(), ctorArgs);
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
        auto* lmod = module->getLlvmModule();
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
        if (!existing) {
            llvm::BasicBlock* tbb = llvm::BasicBlock::Create(
                llvmCtx, "entry", thunk);
            llvm::IRBuilder<> tb(tbb);
            std::vector<llvm::Value*> callArgs;
            if (kind == Kind::BOUND_INSTANCE) {
                // Captures struct is `{ ptr receiver }`. Load the
                // receiver and pass it as `this`. User args follow at
                // thunk arg 1..
                std::vector<llvm::Type*> capFields = {(llvm::Type*) ptrTy};
                llvm::StructType* capStructTy = llvm::StructType::get(
                    llvmCtx, capFields);
                llvm::Value* capArg = thunk->getArg(0);
                llvm::Value* recvSlot = tb.CreateStructGEP(
                    capStructTy, capArg, 0, "captured.this");
                llvm::Value* recvVal = tb.CreateLoad(ptrTy, recvSlot, "this");
                callArgs.push_back(recvVal);
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = 1; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            } else if (kind == Kind::UNBOUND_INSTANCE) {
                // Thunk arg 1 IS the receiver (passed by the caller as
                // the function value's first explicit arg). The method
                // expects `this` first; pass arg 1 there, then args 2..
                callArgs.push_back(thunk->getArg(1));
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = 2; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            } else {
                // STATIC — captures is unused; forward args verbatim.
                unsigned thunkArgCount = thunk->arg_size();
                for (unsigned i = 1; i < thunkArgCount; ++i) {
                    callArgs.push_back(thunk->getArg(i));
                }
            }
            llvm::Value* callResult = tb.CreateCall(
                target->getLlvmFunctionType(), target->getLlvmFunction(),
                callArgs);
            if (thunk->getReturnType()->isVoidTy()) {
                tb.CreateRetVoid();
            } else {
                tb.CreateRet(callResult);
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

    llvm::Value* InstanceOfExpression::generateCode(CajetaModulePtr module) {
        // Static dispatch only: take the lhs's compile-time resolvedType and ask whether
        // it matches `type` (or one of its bases, when we can walk the chain). When the
        // lhs type is unknown at compile time, we fall back to `false` — wrong for the
        // dynamic-dispatch case but safe (never falsely reports a match).
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvm::Type* i1 = llvm::Type::getInt1Ty(ctx);

        // Always evaluate the lhs so side-effects fire even in the static case.
        if (!children.empty()) {
            children[0]->generateCode(module);
        }

        if (!type || children.empty()) {
            return llvm::ConstantInt::getFalse(i1);
        }
        auto lhsExpr = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhsExpr) {
            return llvm::ConstantInt::getFalse(i1);
        }
        // The pre-pass resolver runs before LocalVariableDeclaration populates the scope,
        // so the lhs's resolvedType may be null. Re-run resolveTypes now — at codegen
        // time the scope is fully populated.
        if (!lhsExpr->getResolvedType()) {
            lhsExpr->resolveTypes(module);
        }
        CajetaTypePtr lhsType = lhsExpr->getResolvedType();
        if (!lhsType) {
            return llvm::ConstantInt::getFalse(i1);
        }
        // Exact match. A full implementation would walk lhsType's parent chain; for
        // primitive/non-class types the compile-time check is sufficient.
        bool isMatch = (lhsType->toCanonical() == type->toCanonical());
        return isMatch ? llvm::ConstantInt::getTrue(i1) : llvm::ConstantInt::getFalse(i1);
    }

    void ThisExpression::resolveTypes(CajetaModulePtr module) {
        // `this` resolves to the current class type on the structure stack.
        if (!module->getStructureStack().empty()) {
            resolvedType = module->getStructureStack().back();
        }
    }

    llvm::Value* ThisExpression::generateCode(CajetaModulePtr module) {
        // Method::generateCode registers a ParameterField named "this" in the active scope
        // for non-static methods. We return its alloca (l-value style); consumers can
        // loadIfLValue if they need the pointer itself.
        FieldPtr thisField = module->getScopeStack().peek()->getField("this");
        return thisField ? static_cast<llvm::Value*>(thisField->getOrCreateAllocation()) : nullptr;
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
    // R3-A still restricts to bare class-method calls — instance-method
    // calls inside spawn (`spawn obj.method(args)`) are deferred since they
    // require capturing the receiver too. Pure intrinsics inside spawn are
    // also out of scope (no useful semantics).
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

        auto* outerBuilder = module->getBuilder();
        auto& llvmCtx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::BasicBlock* outerInsertBlock = outerBuilder->GetInsertBlock();
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);

        // Step 1: Evaluate every arg at the spawn site. Any side effects
        // (mutation through `#`, calls with effects, etc.) happen on the
        // calling thread; the worker only sees the resulting values via
        // the context struct. Each capture records both the LLVM value
        // and the CajetaType, since invokeMethod's overload resolution
        // walks the latter.
        vector<llvm::Value*> capturedArgs;
        vector<CajetaTypePtr> capturedArgTypes;
        for (auto& param : innerCall->getParameters()) {
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            llvm::Value* v = param.expression->generateCode(module);
            if (!v) return nullptr;
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                v = outerBuilder->CreateLoad(a->getAllocatedType(), a);
            }
            CajetaTypePtr t = param.expression->getResolvedType();
            if (!t) t = CajetaType::of(v);
            capturedArgs.push_back(v);
            capturedArgTypes.push_back(t);
        }

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
        static uint64_t trampolineCounter = 0;
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
        // Load each arg from ctx[i+1] and build the entries the invoke
        // path expects.
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
        // For static methods, thisValue is nullptr.
        llvm::Value* innerResult = targetClass->invokeMethod(
            methodNameCopy, entries, /*isConstructor=*/false,
            /*thisValue=*/nullptr);
        if (!innerResult) {
            outerBuilder->SetInsertPoint(outerInsertBlock);
            return nullptr;
        }
        // Determine T from the call's result type. invokeMethod hands back
        // the call instruction — its type is what Task<T>::value must hold.
        CajetaTypePtr innerType = CajetaType::of(innerResult);
        if (!innerType) {
            outerBuilder->SetInsertPoint(outerInsertBlock);
            return nullptr;
        }
        auto task = CajetaTask::getOrCreate(module, innerType);
        llvm::Type* taskTy = task->getLlvmType();
        llvm::Value* trampValueSlot = outerBuilder->CreateStructGEP(
            taskTy, taskPtr, CajetaTask::VALUE_FIELD_INDEX,
            "task_value_slot");
        outerBuilder->CreateStore(innerResult, trampValueSlot);
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
        if (llvm::Function* dropPush = module->getRuntimeFunction("__cajeta_drop_push")) {
            if (llvm::Function* taskDropFn = task->getOrCreateDropFunction()) {
                constexpr unsigned DROP_ENTRY_BYTES = 32;
                llvm::Function* parentFnForDrop =
                    outerBuilder->GetInsertBlock()->getParent();
                llvm::IRBuilder<> dropEntryBuilder(
                    &parentFnForDrop->getEntryBlock(),
                    parentFnForDrop->getEntryBlock().begin());
                llvm::Value* dropEntryPtr = dropEntryBuilder.CreateAlloca(
                    llvm::ArrayType::get(i8Ty, DROP_ENTRY_BYTES));
                outerBuilder->CreateCall(dropPush,
                    {dropEntryPtr, taskInstance, taskDropFn});
                if (auto m = module->getCurrentMethod()) {
                    m->registerDropEntry(dropEntryPtr);
                }
                // Publish for ownership-transfer call sites — assignment
                // to a named local marks this entry inactive so the
                // local's class-instance drop becomes the canonical owner.
                dropEntry = dropEntryPtr;
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
        llvm::Value* doneRegSlot = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::DONE_FIELD_INDEX,
            "scope_register_done");
        llvm::Value* excRegSlot = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::EXCEPTION_FIELD_INDEX,
            "scope_register_exc");
        llvm::Value* fiberRegSlot = outerBuilder->CreateStructGEP(
            taskTy, taskInstance, CajetaTask::FIBER_FIELD_INDEX,
            "scope_register_fiber");
        if (llvm::Function* regFn = module->getRuntimeFunction(
                "__cajeta_scope_register")) {
            outerBuilder->CreateCall(regFn,
                {doneRegSlot, excRegSlot, fiberRegSlot});
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

    llvm::Value* DetachExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) return nullptr;
        if (auto inner = dynamic_pointer_cast<Expression>(children[0])) {
            inner->generateCode(module);
        }
        return nullptr;
    }
}
