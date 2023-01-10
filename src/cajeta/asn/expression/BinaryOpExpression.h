//
// Created by James Klappenbach on 4/8/23.
//

#pragma once

#include "Expression.h"

namespace cajeta {

    /**
     * <assoc=right> expression bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=') expression
     */
    enum BinaryOp {
        BINARY_OP_ADD,
        BINARY_OP_SUB,
        BINARY_OP_MUL,
        BINARY_OP_DIV,
        BINARY_OP_BITAND,
        BINARY_OP_BITOR,
        BINARY_OP_BITXOR,
        BINARY_OP_SHIFTRIGHT,
        BINARY_OP_USHIFTRIGHT,
        BINARY_OP_SHIFTLEFT,
        BINARY_OP_MOD,
        BINARY_OP_ASSIGN,
        BINARY_OP_ADD_EQUALS,
        BINARY_OP_SUB_EQUALS,
        BINARY_OP_MUL_EQUALS,
        BINARY_OP_DIV_EQUALS,
        BINARY_OP_BITAND_EQUALS,
        BINARY_OP_BITOR_EQUALS,
        BINARY_OP_BITXOR_EQUALS,
        BINARY_OP_SHIFTRIGHT_EQUALS,
        BINARY_OP_USHIFTRIGHT_EQUALS,
        BINARY_OP_SHIFTLEFT_EQUALS,
        BINARY_OP_MOD_EQUALS
    };

    class BinaryOpExpression : public Expression {
    private:
        BinaryOp binaryOp;
        bool assignment;
        bool requireIntOps;
        MethodPtr overrideMethod;
    public:
        BinaryOpExpression(BinaryOp binaryOp, antlr4::Token* token) : Expression(token) {
            overrideMethod = nullptr;
            this->binaryOp = binaryOp;

            switch (binaryOp) {
                case BINARY_OP_ADD:
                case BINARY_OP_SUB:
                case BINARY_OP_MUL:
                case BINARY_OP_DIV:
                    assignment = false;
                    break;
                case BINARY_OP_BITAND:
                case BINARY_OP_BITOR:
                case BINARY_OP_BITXOR:
                case BINARY_OP_SHIFTRIGHT:
                case BINARY_OP_USHIFTRIGHT:
                case BINARY_OP_SHIFTLEFT:
                case BINARY_OP_MOD:
                    assignment = false;
                    requireIntOps = true;
                    break;
                case BINARY_OP_ASSIGN:
                case BINARY_OP_ADD_EQUALS:
                case BINARY_OP_SUB_EQUALS:
                case BINARY_OP_MUL_EQUALS:
                case BINARY_OP_DIV_EQUALS:
                    assignment = true;
                    break;
                case BINARY_OP_BITAND_EQUALS:
                case BINARY_OP_BITOR_EQUALS:
                case BINARY_OP_BITXOR_EQUALS:
                case BINARY_OP_SHIFTRIGHT_EQUALS:
                case BINARY_OP_USHIFTRIGHT_EQUALS:
                case BINARY_OP_SHIFTLEFT_EQUALS:
                case BINARY_OP_MOD_EQUALS:
                    assignment = true;
                    requireIntOps = true;
                    break;
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
} // cajeta