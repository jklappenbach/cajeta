//
// Created by James Klappenbach on 4/8/23.
//

#include "BinaryOpExpression.h"
#include "../../error/CajetaExceptions.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"

namespace cajeta {

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
        module->getAsnStack().push_back(shared_from_this());

        llvm::Value* rhs;
        llvm::Value* lhs;
        llvm::Value* result = nullptr;
        lhs = children[0]->generateCode(module);
        rhs = children[1]->generateCode(module);
        long lhsTypeFlags = CajetaType::getTypeFlagsOf(lhs);
        long rhsTypeFlags = CajetaType::getTypeFlagsOf(rhs);

        switch (binaryOp) {
            case BINARY_OP_ASSIGN:
                if (lhsTypeFlags & STRUCT_TYPE_ID) {
                    if (!(rhsTypeFlags & STRUCT_TYPE_ID)) {
                        // throw an exception
                    } else {
                        FieldPtr lhsStruct = module->getScopeStack().peek()->getField((llvm::AllocaInst*) lhs);
                        FieldPtr rhsStruct = module->getScopeStack().peek()->getField((llvm::AllocaInst*) rhs);

                        // Assignment must be to a field
                        if (lhsStruct == nullptr) {
                            // throw exception
                        }

//                        if (rhsStruct)
//
//                        rhs = CajetaType::NormalizeRhs()

                    }
                }
                result = module->getBuilder()->CreateStore(rhs, lhs);
                break;
            case BINARY_OP_ADD:
                if (lhs->getType()->isFloatTy()) {
                    result = module->getBuilder()->CreateFAdd(lhs, rhs);
                } else {
                    result = module->getBuilder()->CreateAdd(lhs, rhs);
                }
                break;
            case BINARY_OP_SUB:
                if (lhs->getType()->isFloatTy()) {
                    result = module->getBuilder()->CreateFSub(lhs, rhs);
                } else {
                    result = module->getBuilder()->CreateSub(lhs, rhs);
                }
                break;
            case BINARY_OP_MUL:
                if (lhs->getType()->isFloatTy()) {
                    result = module->getBuilder()->CreateFMul(lhs, rhs);
                } else {
                    result = module->getBuilder()->CreateMul(lhs, rhs);
                }
                break;
            case BINARY_OP_DIV:
//                if (lhs->getType()->isFloatTy()) {
//                    result = pModule->getBuilder()->CreateFDiv(lhs, rhs);
//                } else if ((lhsType->getTypeFlags() | rhsType->getTypeFlags()) & SIGNED_FLAG) {
//                    result = pModule->getBuilder()->CreateSDiv(lhs, rhs);
//                } else {
//                    result = pModule->getBuilder()->CreateUDiv(lhs, rhs);
//                }
                break;
            case BINARY_OP_BITAND:
                result = module->getBuilder()->CreateAnd(lhs, rhs);
                break;
            case BINARY_OP_BITOR:
                result = module->getBuilder()->CreateOr(lhs, rhs);
                break;
            case BINARY_OP_BITXOR:
                result = module->getBuilder()->CreateXor(lhs, rhs);
                break;
            case BINARY_OP_SHIFTRIGHT:
                result = module->getBuilder()->CreateAShr(lhs, rhs);
                break;
            case BINARY_OP_USHIFTRIGHT:
                result = module->getBuilder()->CreateLShr(lhs, rhs);
                break;
            case BINARY_OP_SHIFTLEFT:
                result = module->getBuilder()->CreateShl(lhs, rhs);
                break;
            case BINARY_OP_MOD:
                result = module->getBuilder()->CreateSRem(lhs, rhs);
                break;
            case BINARY_OP_ADD_EQUALS:
                if (lhs->getType()->isFloatTy()) {
                    result = module->getBuilder()->CreateFAdd(lhs, rhs);
                } else {
                    result = module->getBuilder()->CreateAdd(lhs, rhs);
                }

                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_SUB_EQUALS:
                if (lhs->getType()->isFloatTy()) {
                    result = module->getBuilder()->CreateFSub(lhs, rhs);
                } else {
                    result = module->getBuilder()->CreateSub(lhs, rhs);
                }

                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_MUL_EQUALS:
                if (lhs->getType()->isFloatTy()) {
                    result = module->getBuilder()->CreateFMul(lhs, rhs);
                } else {
                    result = module->getBuilder()->CreateMul(lhs, rhs);
                }
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_DIV_EQUALS:
//                if (lhs->getType()->isFloatTy()) {
//                    result = pModule->getBuilder()->CreateFDiv(lhs, rhs);
//                } else if ((lhsType->getTypeFlags() | rhsType->getTypeFlags()) & SIGNED_FLAG) {
//                    result = pModule->getBuilder()->CreateSDiv(lhs, rhs);
//                } else {
//                    result = pModule->getBuilder()->CreateUDiv(lhs, rhs);
//                }
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_BITAND_EQUALS:
                result = module->getBuilder()->CreateAnd(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_BITOR_EQUALS:
                result = module->getBuilder()->CreateOr(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_BITXOR_EQUALS:
                result = module->getBuilder()->CreateXor(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_SHIFTRIGHT_EQUALS:
                result = module->getBuilder()->CreateAShr(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_USHIFTRIGHT_EQUALS:
                result = module->getBuilder()->CreateLShr(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_SHIFTLEFT_EQUALS:
                result = module->getBuilder()->CreateShl(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
            case BINARY_OP_MOD_EQUALS:
                result = module->getBuilder()->CreateSRem(lhs, rhs);
                result = module->getBuilder()->CreateStore(lhs, result);
                break;
        }
        module->getAsnStack().pop_back();
        return result;
    }

} // code