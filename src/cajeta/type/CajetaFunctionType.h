//
// First-class function type — value-type representation of a callable
// `(T1, T2, ..., Tn) -> R`. See cajeta-docs/stdlib/Lambdas.md for the full design.
//
// v1 (L1): non-capturing lambdas only. The LLVM-level value of a function-
// typed expression is a bare function pointer (`ptr`). Captures (L2) will
// extend this to a fat pointer `{ fn_ptr, captures_ptr }`, at which point
// the LLVM representation changes — call sites and variable slots will
// migrate together.
//

#pragma once

#include "CajetaType.h"

#include <vector>

namespace cajeta {

    class CajetaFunctionType;
    typedef std::shared_ptr<CajetaFunctionType> CajetaFunctionTypePtr;

    class CajetaFunctionType : public CajetaType {
    private:
        std::vector<CajetaTypePtr> parameterTypes;
        CajetaTypePtr returnType;
        // Function-type return ABI discriminator (M5(b) — see
        // cajeta-docs/stdlib/ValueReturns.md). `true` (default) = the
        // pre-existing pointer-return / heap-ownership form `(P) -> #R`;
        // `false` reserves the sret value-return form `(P) -> R` that N3
        // will wire up. N1+N2 plumb the field so the canonical
        // discriminates the two forms; the LLVM signature production
        // currently still emits the ptr-return shape regardless.
        bool returnsOwnership = true;
        // Cached LLVM FunctionType — the signature `fn(params...) -> return`
        // that any function value of this Cajeta type conforms to. Stored
        // separately from `llvmType` (which is the value-side `ptr`).
        llvm::FunctionType* llvmFunctionType = nullptr;
    public:
        CajetaFunctionType(CajetaModulePtr module,
            std::vector<CajetaTypePtr> parameterTypes,
            CajetaTypePtr returnType,
            bool returnsOwnership = true);

        const std::vector<CajetaTypePtr>& getParameterTypes() const { return parameterTypes; }
        CajetaTypePtr getReturnType() const { return returnType; }
        bool isReturnsOwnership() const { return returnsOwnership; }
        llvm::FunctionType* getLlvmFunctionType() const { return llvmFunctionType; }

        // Canonical name follows the source form: `(T1,T2) -> R` for the
        // sret value-return form, `(T1,T2) -> #R` for the heap-ownership
        // form. The `#` mirrors the source-level annotation on method
        // returns and keeps the two ABIs as distinct canonical-map
        // entries. Two function types are equal iff their canonicals
        // match.
        static std::string buildCanonical(
            const std::vector<CajetaTypePtr>& parameterTypes,
            CajetaTypePtr returnType,
            bool returnsOwnership = true);

        // Mirror of Method::generatePrototype's pass-by-pointer choice
        // for params and returns at the call boundary. Exposed because
        // the matching call site in MethodReferenceExpression needs to
        // type the indirect call the same way the function-typed
        // signature does.
        static llvm::Type* toCallingConvType(CajetaTypePtr p, llvm::Type* ptrTy);
    };

}
