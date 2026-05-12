//
// Created by James Klappenbach on 4/8/23.
//

#include "BinaryOpExpression.h"
#include "../../error/CajetaExceptions.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../type/CajetaArray.h"
#include "Expression.h"
#include "DotExpression.h"

namespace cajeta {

    // fp4/fp6/fp8 have no native arithmetic on most targets; widen sub-fp16 operands
    // to fp16, perform the op there, then truncate back to the result type.
    static llvm::Value* emitFpBinOp(CajetaModulePtr module, llvm::Value* lhs, llvm::Value* rhs,
                                    llvm::Instruction::BinaryOps op) {
        auto* builder = module->getBuilder();
        llvm::Type* resultTy = lhs->getType();
        if (resultTy->isFloatingPointTy() && resultTy->getScalarSizeInBits() < 16) {
            llvm::Type* halfTy = llvm::Type::getHalfTy(resultTy->getContext());
            llvm::Value* lw = builder->CreateFPExt(lhs, halfTy);
            llvm::Value* rw = rhs->getType() == halfTy ? rhs : builder->CreateFPExt(rhs, halfTy);
            llvm::Value* res = builder->CreateBinOp(op, lw, rw);
            return builder->CreateFPTrunc(res, resultTy);
        }
        return builder->CreateBinOp(op, lhs, rhs);
    }

    // l-value → r-value coercion. Three cases:
    //   (1) AllocaInst: load with the alloca's allocated type. Common case for local vars.
    //   (2) ArrayIndex GEP: load the element value. Reference-typed elements load as
    //       `ptr` (the slot stores a pointer to the referenced object); primitive
    //       elements load their own LLVM type.
    //   (3) Other pointer-typed Value with a resolvedType on the source AST: load with
    //       that type's LLVM mapping. Used for member-access GEPs from DotExpression.
    // Constants and intermediate r-values pass through unchanged.
    static llvm::Value* loadIfLValue(CajetaModulePtr module, llvm::Value* v, ExpressionPtr ast = nullptr) {
        if (!v) return v;
        auto* builder = module->getBuilder();
        if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(v)) {
            return builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (ast && dynamic_pointer_cast<ArrayIndexExpression>(ast)) {
            CajetaTypePtr elemType = ast->getResolvedType();
            if (elemType) {
                llvm::Type* loadTy;
                if (dynamic_pointer_cast<CajetaArray>(elemType) ||
                    (elemType->getTypeFlags() & STRUCT_FLAG)) {
                    loadTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                } else {
                    loadTy = elemType->getLlvmType();
                }
                if (loadTy) return builder->CreateLoad(loadTy, v);
            }
        }
        if (v->getType()->isPointerTy() && ast) {
            if (auto resolved = ast->getResolvedType()) {
                if (llvm::Type* loadTy = resolved->getLlvmType()) {
                    if (loadTy != v->getType()) {
                        return builder->CreateLoad(loadTy, v);
                    }
                }
            }
        }
        return v;
    }

    // Backwards-compat shim — most call sites don't have the ast handy. Behaves like the
    // old loadIfAlloca: only AllocaInst is unwrapped.
    static llvm::Value* loadIfAlloca(CajetaModulePtr module, llvm::Value* v) {
        return loadIfLValue(module, v, nullptr);
    }

    // Coerce two arithmetic operands to a common LLVM type. Cajeta lacks a global type-
    // promotion pass, so the binary-op site does the minimum needed for the IR verifier
    // to accept the result. Strategy: if either is FP, both go to the wider FP; else if
    // both are integers, both go to the wider integer (signed).
    static std::pair<llvm::Value*, llvm::Value*> coerceArithPair(
            CajetaModulePtr module, llvm::Value* l, llvm::Value* r) {
        auto* builder = module->getBuilder();
        llvm::Type* lt = l->getType();
        llvm::Type* rt = r->getType();
        if (lt == rt) return {l, r};

        if (lt->isFloatingPointTy() || rt->isFloatingPointTy()) {
            // Promote the int side to FP, then widen FP to the larger.
            if (lt->isIntegerTy()) l = builder->CreateSIToFP(l, rt);
            if (rt->isIntegerTy()) r = builder->CreateSIToFP(r, lt);
            lt = l->getType();
            rt = r->getType();
            if (lt->getScalarSizeInBits() > rt->getScalarSizeInBits()) {
                r = builder->CreateFPExt(r, lt);
            } else if (rt->getScalarSizeInBits() > lt->getScalarSizeInBits()) {
                l = builder->CreateFPExt(l, rt);
            }
            return {l, r};
        }
        if (lt->isIntegerTy() && rt->isIntegerTy()) {
            unsigned lb = lt->getScalarSizeInBits();
            unsigned rb = rt->getScalarSizeInBits();
            if (lb < rb) l = builder->CreateIntCast(l, rt, /*isSigned=*/true);
            else if (rb < lb) r = builder->CreateIntCast(r, lt, /*isSigned=*/true);
            return {l, r};
        }
        return {l, r};
    }


    /**
     * Business logic:
     *  - First, translate our arguments into types,and normalize into generic types (number instead of int*, etc)
     *  - Next, determine if we need to promote the RHS.  If so, promote.  Otherwise, throw an error
     *  - Look up the operator and see if its overridable.  If so, check the LHS for override method entry.  If exists, call.
     *  - Otherwise, execute standard library op.
     *
     * @param module
     * @return
     */
    llvm::Value* BinaryOpExpression::generateCode(CajetaModulePtr module) {
        auto* builder = module->getBuilder();
        if (std::getenv("CAJETA_TRACE_BINOP")) {
            llvm::errs() << "BinaryOpExpression::generateCode binaryOp=" << binaryOp
                         << " children.size=" << children.size() << "\n";
        }

        // Short-circuit ops need to evaluate rhs only conditionally — handle before the
        // upfront-evaluate path the other ops use.
        if (binaryOp == BINARY_OP_LOGAND || binaryOp == BINARY_OP_LOGOR) {
            llvm::Value* lhsVal = loadIfAlloca(module, children[0]->generateCode(module));
            llvm::Type* i1Ty = llvm::Type::getInt1Ty(*module->getLlvmContext());
            // Coerce lhs to i1 if it isn't already (e.g. an i32 from a comparison-less subexpr).
            if (lhsVal->getType() != i1Ty) {
                llvm::Value* zero = llvm::ConstantInt::get(lhsVal->getType(), 0);
                lhsVal = builder->CreateICmpNE(lhsVal, zero);
            }
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            llvm::LLVMContext& ctx = *module->getLlvmContext();
            llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(ctx,
                binaryOp == BINARY_OP_LOGAND ? "land_rhs" : "lor_rhs", parentFn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx,
                binaryOp == BINARY_OP_LOGAND ? "land_merge" : "lor_merge", parentFn);
            llvm::BasicBlock* lhsBB = builder->GetInsertBlock();
            // && evaluates rhs when lhs is true; || evaluates rhs when lhs is false.
            if (binaryOp == BINARY_OP_LOGAND) {
                builder->CreateCondBr(lhsVal, rhsBB, mergeBB);
            } else {
                builder->CreateCondBr(lhsVal, mergeBB, rhsBB);
            }
            builder->SetInsertPoint(rhsBB);
            llvm::Value* rhsVal = loadIfAlloca(module, children[1]->generateCode(module));
            if (rhsVal->getType() != i1Ty) {
                llvm::Value* zero = llvm::ConstantInt::get(rhsVal->getType(), 0);
                rhsVal = builder->CreateICmpNE(rhsVal, zero);
            }
            llvm::BasicBlock* rhsEndBB = builder->GetInsertBlock();
            builder->CreateBr(mergeBB);
            builder->SetInsertPoint(mergeBB);
            llvm::PHINode* phi = builder->CreatePHI(i1Ty, 2);
            phi->addIncoming(rhsVal, rhsEndBB);
            phi->addIncoming(
                binaryOp == BINARY_OP_LOGAND
                    ? llvm::ConstantInt::getFalse(ctx)
                    : llvm::ConstantInt::getTrue(ctx),
                lhsBB);
            return phi;
        }

        llvm::Value* lhs = children[0]->generateCode(module);
        llvm::Value* rhs = children[1]->generateCode(module);
        ExpressionPtr lhsAst = dynamic_pointer_cast<Expression>(children[0]);
        ExpressionPtr rhsAst = dynamic_pointer_cast<Expression>(children[1]);
        long lhsTypeFlags = CajetaType::getTypeFlagsOf(lhs);
        long rhsTypeFlags = CajetaType::getTypeFlagsOf(rhs);
        llvm::Value* result = nullptr;

        // Replace bare loadIfAlloca with loadIfLValue passing the relevant ast where useful.
        auto loadL = [&](llvm::Value* v) { return loadIfLValue(module, v, lhsAst); };
        auto loadR = [&](llvm::Value* v) { return loadIfLValue(module, v, rhsAst); };
        (void) loadL; (void) loadR; // used selectively below

        // For non-assignment ops both sides are r-values; for assignment forms lhs is the
        // target address and only rhs needs r-value coercion.
        switch (binaryOp) {
            case BINARY_OP_ASSIGN: {
                // Struct-to-struct assignment: both sides are addresses of struct allocas;
                // emit a memcpy sized by the struct's allocation size from the data layout.
                // STRUCT_FLAG (a real flag bit) is the right discriminator; STRUCT_TYPE_ID
                // is the composite ID value and would false-positive on bitwise AND.
                if ((lhsTypeFlags & STRUCT_FLAG) && (rhsTypeFlags & STRUCT_FLAG)) {
                    llvm::Type* structTy = nullptr;
                    if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                        structTy = a->getAllocatedType();
                    } else if (lhsAst && lhsAst->getResolvedType()) {
                        structTy = lhsAst->getResolvedType()->getLlvmType();
                    }
                    if (structTy) {
                        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                        llvm::Value* size = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(*module->getLlvmContext()),
                            dl.getTypeAllocSize(structTy));
                        llvm::Align align(dl.getABITypeAlign(structTy));
                        builder->CreateMemCpy(lhs, align, rhs, align, size);
                        // For struct assignment, the expression yields the destination
                        // address (lvalue convention).
                        result = lhs;
                        break;
                    }
                }
                llvm::Value* rhsVal = loadR(rhs);
                // Coerce rhs to the destination's element type. Two slot shapes need
                // this: (a) plain local-variable allocas, where the alloca carries the
                // type directly; (b) GEPs into an array element, where the AST gives
                // us the element type. Both matter — without the GEP path a wide
                // integer literal like `xs[0] = 10` (i64 by default) writes 8 bytes
                // into a 4-byte slot and clobbers neighboring memory.
                llvm::Type* slotTy = nullptr;
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                    slotTy = a->getAllocatedType();
                } else if (lhsAst && dynamic_pointer_cast<ArrayIndexExpression>(lhsAst)) {
                    if (auto elemType = lhsAst->getResolvedType()) {
                        // For reference-element arrays the slot holds a pointer;
                        // primitive arrays hold the value directly.
                        if (dynamic_pointer_cast<CajetaArray>(elemType) ||
                            (elemType->getTypeFlags() & STRUCT_FLAG)) {
                            slotTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                        } else if (llvm::Type* lt = elemType->getLlvmType()) {
                            slotTy = lt;
                        }
                    }
                } else if (auto dotLhs = dynamic_pointer_cast<DotExpression>(lhsAst)) {
                    // `obj.field = value` — slot type is the field's declared
                    // type. Walk down the chain to find the field on the
                    // receiver's class/struct.
                    if (!dotLhs->getChildren().empty()) {
                        auto recv = dynamic_pointer_cast<Expression>(dotLhs->getChildren()[0]);
                        if (recv) {
                            if (!recv->getResolvedType()) recv->resolveTypes(module);
                            if (auto klass = dynamic_pointer_cast<CajetaClass>(recv->getResolvedType())) {
                                auto& props = klass->getProperties();
                                auto it = props.find(dotLhs->getIdentifier());
                                if (it != props.end()) {
                                    slotTy = it->second->getType()->getLlvmType();
                                }
                            }
                        }
                    }
                }
                if (slotTy && rhsVal->getType() != slotTy) {
                    if (slotTy->isIntegerTy() && rhsVal->getType()->isIntegerTy()) {
                        rhsVal = builder->CreateIntCast(rhsVal, slotTy, /*isSigned=*/true);
                    } else if (slotTy->isFloatingPointTy() && rhsVal->getType()->isFloatingPointTy()) {
                        rhsVal = builder->CreateFPCast(rhsVal, slotTy);
                    } else if (slotTy->isFloatingPointTy() && rhsVal->getType()->isIntegerTy()) {
                        rhsVal = builder->CreateSIToFP(rhsVal, slotTy);
                    } else if (slotTy->isIntegerTy() && rhsVal->getType()->isFloatingPointTy()) {
                        rhsVal = builder->CreateFPToSI(rhsVal, slotTy);
                    }
                }
                builder->CreateStore(rhsVal, lhs);
                // The expression's value is the assigned r-value (C/Java convention),
                // not the StoreInst — that way `x = 100` can be used inside a ternary
                // or other surrounding expression.
                result = rhsVal;
                break;
            }
            case BINARY_OP_ADD: {
                llvm::Value* l = loadL(lhs);
                llvm::Value* r = loadR(rhs);
                // String concatenation: if either operand evaluates to a pointer
                // and neither side is an array, lower to __cajeta_str_concat,
                // auto-stringifying primitive operands first.
                bool lIsArr = lhsAst && dynamic_pointer_cast<CajetaArray>(lhsAst->getResolvedType());
                bool rIsArr = rhsAst && dynamic_pointer_cast<CajetaArray>(rhsAst->getResolvedType());
                bool lIsPtr = l->getType()->isPointerTy() && !lIsArr;
                bool rIsPtr = r->getType()->isPointerTy() && !rIsArr;
                if (lIsPtr || rIsPtr) {
                    auto& llvmCtx = *module->getLlvmContext();
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                    auto stringify = [&](llvm::Value* v) -> llvm::Value* {
                        llvm::Type* t = v->getType();
                        if (t->isPointerTy()) return v;
                        if (t->isIntegerTy(1)) {
                            llvm::Value* widened = builder->CreateZExt(v, i32Ty);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                            return builder->CreateCall(fn, {widened});
                        }
                        if (t->isIntegerTy()) {
                            v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                            return builder->CreateCall(fn, {v});
                        }
                        if (t->isFloatingPointTy()) {
                            if (t != f64Ty) v = builder->CreateFPCast(v, f64Ty);
                            llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                            return builder->CreateCall(fn, {v});
                        }
                        return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                    };
                    llvm::Value* ls = stringify(l);
                    llvm::Value* rs = stringify(r);
                    llvm::Function* concat = module->getRuntimeFunction("__cajeta_str_concat");
                    result = builder->CreateCall(concat, {ls, rs});
                    break;
                }
                auto [pl, pr] = coerceArithPair(module, l, r);
                result = pl->getType()->isFloatingPointTy()
                    ? emitFpBinOp(module, pl, pr, llvm::Instruction::FAdd)
                    : builder->CreateAdd(pl, pr);
                break;
            }
            case BINARY_OP_SUB: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = l->getType()->isFloatingPointTy()
                    ? emitFpBinOp(module, l, r, llvm::Instruction::FSub)
                    : builder->CreateSub(l, r);
                break;
            }
            case BINARY_OP_MUL: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = l->getType()->isFloatingPointTy()
                    ? emitFpBinOp(module, l, r, llvm::Instruction::FMul)
                    : builder->CreateMul(l, r);
                break;
            }
            case BINARY_OP_DIV: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                if (l->getType()->isFloatingPointTy()) {
                    result = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                } else if ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) {
                    result = builder->CreateSDiv(l, r);
                } else {
                    result = builder->CreateUDiv(l, r);
                }
                break;
            }
            case BINARY_OP_BITAND: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateAnd(l, r);
                break;
            }
            case BINARY_OP_BITOR: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateOr(l, r);
                break;
            }
            case BINARY_OP_BITXOR: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateXor(l, r);
                break;
            }
            case BINARY_OP_SHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateAShr(l, r);
                break;
            }
            case BINARY_OP_USHIFTRIGHT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateLShr(l, r);
                break;
            }
            case BINARY_OP_SHIFTLEFT: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                result = builder->CreateShl(l, r);
                break;
            }
            case BINARY_OP_MOD: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                if (l->getType()->isFloatingPointTy()) {
                    result = builder->CreateFRem(l, r);
                } else if ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) {
                    result = builder->CreateSRem(l, r);
                } else {
                    result = builder->CreateURem(l, r);
                }
                break;
            }
            // Compound assignments: compute at the wider type, then truncate back to the
            // alloca's element type before storing.
            case BINARY_OP_ADD_EQUALS:
            case BINARY_OP_SUB_EQUALS:
            case BINARY_OP_MUL_EQUALS:
            case BINARY_OP_DIV_EQUALS:
            case BINARY_OP_BITAND_EQUALS:
            case BINARY_OP_BITOR_EQUALS:
            case BINARY_OP_BITXOR_EQUALS:
            case BINARY_OP_SHIFTRIGHT_EQUALS:
            case BINARY_OP_USHIFTRIGHT_EQUALS:
            case BINARY_OP_SHIFTLEFT_EQUALS:
            case BINARY_OP_MOD_EQUALS: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                llvm::Value* newVal = nullptr;
                bool isFp = l->getType()->isFloatingPointTy();
                bool isSigned = ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0;
                switch (binaryOp) {
                    case BINARY_OP_ADD_EQUALS:
                        newVal = isFp ? emitFpBinOp(module, l, r, llvm::Instruction::FAdd)
                                      : builder->CreateAdd(l, r);
                        break;
                    case BINARY_OP_SUB_EQUALS:
                        newVal = isFp ? emitFpBinOp(module, l, r, llvm::Instruction::FSub)
                                      : builder->CreateSub(l, r);
                        break;
                    case BINARY_OP_MUL_EQUALS:
                        newVal = isFp ? emitFpBinOp(module, l, r, llvm::Instruction::FMul)
                                      : builder->CreateMul(l, r);
                        break;
                    case BINARY_OP_DIV_EQUALS:
                        if (isFp) newVal = emitFpBinOp(module, l, r, llvm::Instruction::FDiv);
                        else if (isSigned) newVal = builder->CreateSDiv(l, r);
                        else newVal = builder->CreateUDiv(l, r);
                        break;
                    case BINARY_OP_BITAND_EQUALS:     newVal = builder->CreateAnd(l, r);  break;
                    case BINARY_OP_BITOR_EQUALS:      newVal = builder->CreateOr(l, r);   break;
                    case BINARY_OP_BITXOR_EQUALS:     newVal = builder->CreateXor(l, r);  break;
                    case BINARY_OP_SHIFTRIGHT_EQUALS: newVal = builder->CreateAShr(l, r); break;
                    case BINARY_OP_USHIFTRIGHT_EQUALS:newVal = builder->CreateLShr(l, r); break;
                    case BINARY_OP_SHIFTLEFT_EQUALS:  newVal = builder->CreateShl(l, r);  break;
                    case BINARY_OP_MOD_EQUALS:
                        newVal = isSigned ? builder->CreateSRem(l, r) : builder->CreateURem(l, r);
                        break;
                    default: break;
                }
                // Narrow back to the alloca element type if the wider arithmetic produced
                // a wider value than lhs's slot.
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(lhs)) {
                    llvm::Type* slotTy = a->getAllocatedType();
                    if (newVal && newVal->getType() != slotTy) {
                        if (slotTy->isIntegerTy() && newVal->getType()->isIntegerTy()) {
                            newVal = builder->CreateIntCast(newVal, slotTy, /*isSigned=*/true);
                        } else if (slotTy->isFloatingPointTy() && newVal->getType()->isFloatingPointTy()) {
                            newVal = builder->CreateFPCast(newVal, slotTy);
                        }
                    }
                }
                if (newVal) {
                    builder->CreateStore(newVal, lhs);
                }
                result = newVal;
                break;
            }
            case BINARY_OP_LT:
            case BINARY_OP_LE:
            case BINARY_OP_GT:
            case BINARY_OP_GE:
            case BINARY_OP_EQ:
            case BINARY_OP_NE: {
                auto [l, r] = coerceArithPair(module, loadL(lhs), loadR(rhs));
                bool isFp = l->getType()->isFloatingPointTy();
                bool isSigned = ((lhsTypeFlags | rhsTypeFlags) & SIGNED_FLAG) != 0;
                if (isFp) {
                    switch (binaryOp) {
                        case BINARY_OP_LT: result = builder->CreateFCmpOLT(l, r); break;
                        case BINARY_OP_LE: result = builder->CreateFCmpOLE(l, r); break;
                        case BINARY_OP_GT: result = builder->CreateFCmpOGT(l, r); break;
                        case BINARY_OP_GE: result = builder->CreateFCmpOGE(l, r); break;
                        case BINARY_OP_EQ: result = builder->CreateFCmpOEQ(l, r); break;
                        case BINARY_OP_NE: result = builder->CreateFCmpONE(l, r); break;
                        default: break;
                    }
                } else {
                    switch (binaryOp) {
                        case BINARY_OP_LT:
                            result = isSigned ? builder->CreateICmpSLT(l, r) : builder->CreateICmpULT(l, r);
                            break;
                        case BINARY_OP_LE:
                            result = isSigned ? builder->CreateICmpSLE(l, r) : builder->CreateICmpULE(l, r);
                            break;
                        case BINARY_OP_GT:
                            result = isSigned ? builder->CreateICmpSGT(l, r) : builder->CreateICmpUGT(l, r);
                            break;
                        case BINARY_OP_GE:
                            result = isSigned ? builder->CreateICmpSGE(l, r) : builder->CreateICmpUGE(l, r);
                            break;
                        case BINARY_OP_EQ: result = builder->CreateICmpEQ(l, r); break;
                        case BINARY_OP_NE: result = builder->CreateICmpNE(l, r); break;
                        default: break;
                    }
                }
                break;
            }
            case BINARY_OP_LOGAND:
            case BINARY_OP_LOGOR:
                // Handled at the top of the function via short-circuit branching.
                break;
        }
        return result;
    }

} // code