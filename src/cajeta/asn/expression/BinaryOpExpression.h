//
// Created by James Klappenbach on 4/8/23.
//

#pragma once

#include "Expression.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Shared codegen helpers (defined in BinaryOpExpression.cpp). Used
    // by anyone emitting a signed-overflow-trapping arithmetic op.
    void emitUbTrap(CajetaModulePtr module,
                    llvm::IRBuilder<>& b,
                    llvm::Value* condTrap,
                    const std::string& label);
    llvm::Value* emitSignedOverflowOp(CajetaModulePtr module,
                                       llvm::IRBuilder<>& b,
                                       llvm::Intrinsic::ID intrinId,
                                       llvm::Value* l,
                                       llvm::Value* r,
                                       const std::string& label);


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
        bool arenaEligible = false;
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

        bool isAssignment() const { return assignment; }

        // Comparison result typing, override-aware (defined in the .cpp —
        // needs CajetaClass::resolveMethod). Returns the matching operator
        // override's declared return type, `boolean` when there is none, or
        // nullptr when the operand types are not yet resolvable (caller
        // leaves resolvedType unset for later re-resolution).
        CajetaTypePtr comparisonResultType(CajetaModulePtr module);
        BinaryOp getBinaryOp() const { return binaryOp; }

        // Frame-arena routing (frame-arena-plan U2): set by Method's escape pre-pass
        // when this is a String concat whose result binds to a non-escaping local.
        // When set, concat codegen bump-allocates the result from the frame arena
        // (no malloc, no live-set) and the local registers no drop entry.
        void setArenaEligible(bool b) { arenaEligible = b; }
        bool isArenaEligible() const { return arenaEligible; }

        void resolveTypes(CajetaModulePtr module) override {
            // Walk children first, then take lhs's type as our result type. A real type
            // promotion pass would pick the wider of lhs/rhs; for now this matches the
            // existing assumption in codegen (lhs drives the op type).
            AbstractSyntaxNode::resolveTypes(module);
            // ...EXCEPT comparisons and short-circuit logical ops, whose
            // result is boolean (the enum comments above say so; codegen
            // emits i1). Typing them as the LHS operand let `x > 0` ride the
            // "number" generic bucket into a boolean formal via closest-
            // match, but a reference LHS had no such bridge:
            // `s != null && s.equals(...)` typed as cajeta.lang.String and
            // missed every overload — a silent resolution miss until
            // silent-resolution-diagnostics made it NO_MATCHING_OVERLOAD.
            // (Operator-overloaded EQ/NE also return boolean by stdlib
            // convention; generateCode re-stamps resolvedType for override
            // calls regardless.)
            switch (binaryOp) {
                case BINARY_OP_LT:
                case BINARY_OP_LE:
                case BINARY_OP_GT:
                case BINARY_OP_GE:
                case BINARY_OP_EQ:
                case BINARY_OP_NE: {
                    // nucleo-frame U1 — a class operand may declare a
                    // comparison override with a NON-boolean return (a DSL
                    // node: `col.price > 0.0` -> Predicate). Type from the
                    // override's declared return when one resolves; boolean
                    // otherwise (every in-tree override returns boolean, so
                    // this changes nothing for them). When the operand types
                    // are not yet resolvable (the pre-pass runs before
                    // locals register), leave the type UNSET so the caller's
                    // later null-check re-resolution computes it with real
                    // operand types — a premature boolean stamp is exactly
                    // what broke argument-position overload resolution.
                    resolvedType = comparisonResultType(module);
                    return;
                }
                case BINARY_OP_LOGAND:
                case BINARY_OP_LOGOR:
                    resolvedType = CajetaType::of("boolean");
                    return;
                default:
                    break;
            }
            if (!children.empty()) {
                if (auto lhs = dynamic_pointer_cast<Expression>(children[0])) {
                    resolvedType = lhs->getResolvedType();
                }
            }
        }

        llvm::Value* generateCode(CajetaModulePtr module) override;

    private:
        // B1: `*` on a matrix LHS — matrix multiply (Matrix*Matrix), matrix-
        // vector (Matrix*Vector), or scalar scale (Matrix*scalar). Sets
        // resolvedType to the result shape and returns the value, or nullptr if
        // the op/operands aren't a handled matrix-multiply form (caller falls
        // through). Defined in BinaryOpExpression.cpp.
        llvm::Value* generateMatrixMul(CajetaModulePtr module, llvm::Value* lhs,
                                       llvm::Value* rhs,
                                       const ExpressionPtr& lhsAst,
                                       const ExpressionPtr& rhsAst);
    };
} // code