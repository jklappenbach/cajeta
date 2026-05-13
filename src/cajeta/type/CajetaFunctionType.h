//
// First-class function type — value-type representation of a callable
// `(T1, T2, ..., Tn) -> R`. See cajeta-docs/Lambdas.md for the full design.
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
        // Cached LLVM FunctionType — the signature `fn(params...) -> return`
        // that any function value of this Cajeta type conforms to. Stored
        // separately from `llvmType` (which is the value-side `ptr`).
        llvm::FunctionType* llvmFunctionType = nullptr;
    public:
        CajetaFunctionType(CajetaModulePtr module,
            std::vector<CajetaTypePtr> parameterTypes,
            CajetaTypePtr returnType);

        const std::vector<CajetaTypePtr>& getParameterTypes() const { return parameterTypes; }
        CajetaTypePtr getReturnType() const { return returnType; }
        llvm::FunctionType* getLlvmFunctionType() const { return llvmFunctionType; }

        // Canonical name follows the source form: `(T1,T2) -> R`. Two
        // function types are equal iff their canonicals match, which is
        // the existing CajetaType equality story.
        static std::string buildCanonical(
            const std::vector<CajetaTypePtr>& parameterTypes,
            CajetaTypePtr returnType);
    };

}
