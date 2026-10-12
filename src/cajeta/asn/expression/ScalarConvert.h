// Scalar conversions that extend by the SOURCE type's signedness (int64-scan-verbs 8.4).

#pragma once

#include <llvm/IR/IRBuilder.h>

#include "Expression.h"
#include "../../type/CajetaType.h"

namespace cajeta {

    /** Whether a value of `t` converts as signed: an unsigned integer or a boolean does not, anything else does. */
    inline bool convertsSigned(const CajetaTypePtr& t) {
        if (!t) return true;
        CajetaTypeFlags f = t->getTypeFlags();
        if (!(f & PRIMITIVE_FLAG) || (f & FLOAT_FLAG) || !(f & INT_FLAG)) return true;
        return (f & SIGNED_FLAG) != 0;
    }

    /** Whether the value of node `n` converts as signed, resolving its type when unset; an initializer reads its expression. */
    inline bool convertsSigned(const AbstractSyntaxNodePtr& n, const CajetaModulePtr& module) {
        if (!n) return true;
        if (auto e = std::dynamic_pointer_cast<Expression>(n)) {
            if (!e->getResolvedType() && module) e->resolveTypes(module);
            return convertsSigned(e->getResolvedType());
        }
        if (n->getChildren().size() == 1) return convertsSigned(n->getChildren()[0], module);
        return true;
    }

    /** Converts integer `v` to integer or floating-point `dst` by the source's signedness. */
    inline llvm::Value* convertIntBySource(llvm::IRBuilderBase& b, llvm::Value* v, llvm::Type* dst,
                                           bool srcSigned) {
        if (dst->isIntegerTy()) return b.CreateIntCast(v, dst, srcSigned);
        return srcSigned ? b.CreateSIToFP(v, dst) : b.CreateUIToFP(v, dst);
    }

} // namespace cajeta
