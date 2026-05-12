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
        BINARY_OP_MOD_EQUALS,
        // Comparisons → i1
        BINARY_OP_LT,
        BINARY_OP_LE,
        BINARY_OP_GT,
        BINARY_OP_GE,
        BINARY_OP_EQ,
        BINARY_OP_NE,
        // Short-circuit logical → i1
        BINARY_OP_LOGAND,
        BINARY_OP_LOGOR
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
                case BINARY_OP_LT:
                case BINARY_OP_LE:
                case BINARY_OP_GT:
                case BINARY_OP_GE:
                case BINARY_OP_EQ:
                case BINARY_OP_NE:
                case BINARY_OP_LOGAND:
                case BINARY_OP_LOGOR:
                    assignment = false;
                    break;
            }
        }

        void resolveTypes(CajetaModulePtr module) override {
            // Walk children first, then take lhs's type as our result type. A real type
            // promotion pass would pick the wider of lhs/rhs; for now this matches the
            // existing assumption in codegen (lhs drives the op type).
            AbstractSyntaxNode::resolveTypes(module);
            if (!children.empty()) {
                if (auto lhs = dynamic_pointer_cast<Expression>(children[0])) {
                    resolvedType = lhs->getResolvedType();
                }
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;
    };
} // code