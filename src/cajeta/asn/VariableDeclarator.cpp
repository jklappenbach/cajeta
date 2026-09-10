// Codegen for a declarator's initializer forms.

#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../error/Diagnostics.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaFunctionType.h"
#include "expression/ArrayLowering.h"

namespace cajeta {
    llvm::Value* VariableDeclarator::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* VariableInitializer::generateCode(CajetaModulePtr module) {
        // A wrapped expression may evaluate to an l-value (an alloca, a GEP), and
        // the surrounding slot store needs the r-value loaded through it, so
        // every initializer is forwarded via loadIfLValue.
        auto& back = children.back();
        llvm::Value* v = back->generateCode(module);
        auto exprAst = dynamic_pointer_cast<Expression>(back);
        // A `void` call is present but valueless, so the null-init guards miss it.
        if (exprAst && exprAst->getResolvedType()
                && exprAst->getResolvedType()->toCanonical() == "void") {
            throw locatedException(
                exprAst->getSourceLine(), exprAst->getSourceColumn() + 1,
                "initializer is a 'void' expression, which has no value to store",
                "CAJETA_ERROR_UNRESOLVED_EXPRESSION");
        }
        return loadIfLValue(module, v, exprAst);
    }

    llvm::Value* ArrayInitializer::generateCode(CajetaModulePtr module) {
        // Allocate an array header of length N and populate it in source order;
        // the caller must have called setElementType or it cannot be sized.
        if (!elementType) {
            return nullptr;
        }
        // Brace initializers are retired for data arrays but survive for
        // function-typed dispatch tables, so only non-function elements fail.
        if (!dynamic_pointer_cast<CajetaFunctionType>(elementType)) {
            throw locatedException(
                getSourceLine(), getSourceColumn() + 1,
                "array brace-initializer `{ ... }` is retired; use a bracket "
                "literal `[ ... ]` instead (e.g. `[1, 2, 3]`). Braces build "
                "aggregates (`Point { x: 1 }`) and function-typed dispatch "
                "tables only.",
                "CAJETA_ERROR_ARRAY_BRACE_INIT_RETIRED");
        }
        return emitArrayFromElements(module, elementType, children);
    }

} // code