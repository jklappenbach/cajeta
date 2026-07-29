//
// Created by James Klappenbach on 11/4/22.
//

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
        // collection-literals Unit 5 — the array brace-initializer `{ ... }` is
        // retired for value/data arrays: `int32[] xs = {1, 2, 3}` must now be
        // written `int32[] xs = [1, 2, 3]`. Braces build aggregates and the one
        // carved-out exception: function-typed device dispatch tables
        // (`((T) -> R)[] ops = { A::f, B::g }`), whose element type is a function
        // type. Those lower through KernelLowering on device and through here on
        // host, and must keep working, so only reject non-function element types.
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