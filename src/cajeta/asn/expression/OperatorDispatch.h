//
// Representation-agnostic binary-operator dispatch + comparison derivation
// (value-type-overloading-plan.md S6). Extracted from BinaryOpExpression so the
// SAME operator-lookup + derivation policy serves both the host expression
// codegen and the device kernel lowering (S8) — the two differ only in HOW an
// operator is resolved and invoked (host: resolveMethod/invokeMethod + CreateNot;
// device: the @Device-operator lowering), which the caller supplies as
// callbacks. The op→symbol map and the !=/>/>=/<= derivation table live here,
// once.
//
#pragma once

#include <string>
#include <utility>

#include "llvm/IR/Value.h"

#include "BinaryOpExpression.h"   // BinaryOp enum

namespace cajeta {
namespace opdispatch {

// The overloadable symbol for a binary operator ("+", "==", "<", …), or nullptr
// for ops with no user-overloadable static form (assignment, logical &&/||,
// shifts — handled on their own paths). Single source of truth for both the host
// dispatch gate and device lowering.
inline const char* binaryOpSymbol(BinaryOp op) {
    switch (op) {
        case BINARY_OP_ADD:    return "+";
        case BINARY_OP_SUB:    return "-";
        case BINARY_OP_MUL:    return "*";
        case BINARY_OP_DIV:    return "/";
        case BINARY_OP_MOD:    return "%";
        case BINARY_OP_EQ:     return "==";
        case BINARY_OP_NE:     return "!=";
        case BINARY_OP_LT:     return "<";
        case BINARY_OP_GT:     return ">";
        case BINARY_OP_LE:     return "<=";
        case BINARY_OP_GE:     return ">=";
        case BINARY_OP_BITAND: return "&";
        case BINARY_OP_BITOR:  return "|";
        case BINARY_OP_BITXOR: return "^";
        default:               return nullptr;
    }
}

// A comparison-derivation plan: synthesize an unprovided comparison from a base
// operator the user DID define (OperatorOverloading.md §7):
//   a != b  ≡  !(a == b)
//   a >  b  ≡   (b <  a)      [swap operands]
//   a >= b  ≡  !(a <  b)
//   a <= b  ≡  !(b <  a)      [swap operands]
// baseSym == nullptr means the operator derives nothing (try direct only).
struct Derivation {
    const char* baseSym;
    bool swapOperands;
    bool negateResult;
};

inline Derivation binaryOpDerivation(BinaryOp op) {
    switch (op) {
        case BINARY_OP_NE: return {"==", /*swap=*/false, /*negate=*/true};
        case BINARY_OP_GT: return {"<",  /*swap=*/true,  /*negate=*/false};
        case BINARY_OP_GE: return {"<",  /*swap=*/false, /*negate=*/true};
        case BINARY_OP_LE: return {"<",  /*swap=*/true,  /*negate=*/true};
        default:           return {nullptr, false, false};
    }
}

// Try the direct operator, then its comparison derivation.
//   tryInvoke(opName, swapOperands) -> {resolved, result}: resolve + invoke the
//       named operator with operands in the given order; resolved==false means
//       the operator isn't defined (so a derivation may be attempted).
//   negate(v) -> the boolean negation of v.
// Returns {handled, value}: handled==true iff a direct or derived operator
// applied; the caller then returns `value`, otherwise it falls through to its
// built-in path. Representation-agnostic: works for any pair of callbacks.
template <typename TryInvoke, typename Negate>
std::pair<bool, llvm::Value*> dispatchBinaryOperator(
        BinaryOp op, const TryInvoke& tryInvoke, const Negate& negate) {
    const char* sym = binaryOpSymbol(op);
    if (!sym) {
        return {false, nullptr};
    }
    std::pair<bool, llvm::Value*> direct =
        tryInvoke(std::string("operator") + sym, /*swapOperands=*/false);
    if (direct.first) {
        return {true, direct.second};
    }
    Derivation d = binaryOpDerivation(op);
    if (d.baseSym) {
        std::pair<bool, llvm::Value*> base =
            tryInvoke(std::string("operator") + d.baseSym, d.swapOperands);
        if (base.first) {
            return {true, d.negateResult ? negate(base.second) : base.second};
        }
    }
    return {false, nullptr};
}

} // namespace opdispatch
} // namespace cajeta
