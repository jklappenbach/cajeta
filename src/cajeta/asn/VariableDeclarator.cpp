//
// Created by James Klappenbach on 11/4/22.
//

#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"
#include "../error/Diagnostics.h"
#include "../type/CajetaArray.h"
#include "expression/ArrayLowering.h"

namespace cajeta {
    llvm::Value* VariableDeclarator::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* VariableInitializer::generateCode(CajetaModulePtr module) {
        // An initializer's job is to produce a value that the surrounding
        // declaration writes into the local's slot. If the wrapped
        // expression evaluates to an l-value — IdentifierExpression's
        // alloca for `int32 a = b;`, ArrayIndexExpression's GEP for
        // `int32 v = arr[i];`, DotExpression's field GEP for
        // `int32 f = obj.x;` — the slot store needs the r-value loaded
        // through, not the slot pointer itself. Forward via loadIfLValue
        // so every consumer of an initializer (StackField, HeapField,
        // generated stores) sees a real value.
        auto& back = children.back();
        llvm::Value* v = back->generateCode(module);
        auto exprAst = dynamic_pointer_cast<Expression>(back);
        // A `void` call is present but valueless, so the null-init guards miss it
        // and initializing from one hung the compiler rather than diagnosing (1.2.3).
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
        // `int32[] xs = {1, 2, 3}` — allocate an array header of length N and
        // populate each slot in source order. The caller (LocalVariableDeclaration
        // or VariableInitializer) is responsible for calling setElementType before
        // codegen; without it we can't size the allocation or coerce values. The
        // store loop is shared with the `[...]` literal expression (array-literals
        // §7) so the two forms cannot drift.
        if (!elementType) {
            return nullptr;
        }
        return emitArrayFromElements(module, elementType, children);
    }

} // code