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
            // Method reference: `expr::id`, `Type::id`, or `Type::new`. Check before NEW
            // and identifier so we don't mis-route those token-bearing forms.
            result = make_shared<UnsupportedExpression>("method reference", token);
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
        auto [_, val] = loadOperand(module, children[0]);
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
            result = make_shared<ThisExpression>(ctx->expression());
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
            bool byValue;  // true: primitive copy; false: borrow (heap ptr)
        };
        std::vector<Capture> captures;
        ScopePtr outerScope = module->getScopeStack().isEmpty()
            ? nullptr : module->getScopeStack().peek();
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
                captures.push_back({name, t, /*byValue=*/!slotIsPointer});
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

        // ---- Allocate + populate the captures struct on the outer stack,
        //      using the outer builder (we haven't switched insertion yet).
        //      Each primitive capture loads its current value from the
        //      outer field's slot and stores it into the captures struct.
        //      The captures struct address goes into the closure record's
        //      captures slot a few lines down.
        llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        llvm::Value* capturesPtr = nullptr;
        if (capturesTy) {
            llvm::Function* outerFn = outerInsertBlock
                ? outerInsertBlock->getParent() : nullptr;
            if (outerFn) {
                llvm::IRBuilder<> entry(&outerFn->getEntryBlock(),
                    outerFn->getEntryBlock().getFirstInsertionPt());
                capturesPtr = entry.CreateAlloca(capturesTy, nullptr, "captures");
            } else {
                capturesPtr = outerBuilder->CreateAlloca(capturesTy, nullptr, "captures");
            }
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

        // L2 ABI: materialize a closure record `{ ptr fn, ptr captures }`
        // on the outer stack. A function-typed variable's slot (still a
        // single `ptr`) holds the address of this record; the call site
        // loads fn_ptr and captures_ptr from it and indirect-dispatches.
        // capturesPtr is non-null when free identifiers in the body
        // captured outer fields (L2-2 onwards); otherwise null and the
        // synthesized function ignores its first arg.
        llvm::StructType* closureTy = llvm::StructType::get(
            llvmCtx, {(llvm::Type*) ptrTy, (llvm::Type*) ptrTy});
        // Place the alloca in the outer function's entry block so the slot
        // is hoisted out of any inner loops (same convention as the rest of
        // the codegen).
        llvm::Function* closureOuterFn = outerInsertBlock
            ? outerInsertBlock->getParent() : nullptr;
        llvm::Value* closure;
        if (closureOuterFn) {
            llvm::IRBuilder<> entry(&closureOuterFn->getEntryBlock(),
                closureOuterFn->getEntryBlock().getFirstInsertionPt());
            closure = entry.CreateAlloca(closureTy, nullptr, "closure");
        } else {
            closure = outerBuilder->CreateAlloca(closureTy, nullptr, "closure");
        }
        llvm::Value* fnSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 0, "closure.fn");
        outerBuilder->CreateStore(fn, fnSlot);
        llvm::Value* capSlot = outerBuilder->CreateStructGEP(
            closureTy, closure, 1, "closure.captures");
        llvm::Value* capValue = capturesPtr
            ? capturesPtr
            : (llvm::Value*) llvm::ConstantPointerNull::get(ptrTy);
        outerBuilder->CreateStore(capValue, capSlot);
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
}
