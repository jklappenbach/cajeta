// Binary-operator dispatch + comparison derivation: one lookup and derivation
// policy for both host codegen and device lowering, which differ only in the
// resolve/invoke callbacks they pass.
#pragma once

#include <string>
#include <utility>

#include "llvm/IR/Value.h"

#include "BinaryOpExpression.h"   // BinaryOp enum

namespace cajeta {
namespace opdispatch {

// The overloadable symbol, or nullptr for assignment, logical &&/|| and shifts.
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

// A plan for synthesizing an unprovided comparison from one the user DID define:
//   a != b ≡ !(a == b);  a > b ≡ (b < a);  a >= b ≡ !(a < b);  a <= b ≡ !(b < a)
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

// Try the direct operator, then its derivation. `tryInvoke(opName, swap)` returns
// {resolved, result}, resolved==false meaning undefined; `negate(v)` inverts a
// boolean. Returns {handled, value}; !handled means fall through to the built-in.
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
