// First-class function type - the value-type representation of a callable
// `(T1, ..., Tn) -> R`. v1 is non-capturing only: the LLVM value is a bare `ptr`.
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
        // Return-ABI discriminator: true (default) = the pointer-return / heap-ownership
        // form `(P) -> #R`; false reserves the sret value-return form `(P) -> R`.
        bool returnsOwnership = true;
        // Cached LLVM signature, kept apart from `llvmType` (the value-side `ptr`).
        llvm::FunctionType* llvmFunctionType = nullptr;
    public:
        CajetaFunctionType(CajetaModulePtr module,
            std::vector<CajetaTypePtr> parameterTypes,
            CajetaTypePtr returnType,
            bool returnsOwnership = true);

        const std::vector<CajetaTypePtr>& getParameterTypes() const { return parameterTypes; }
        CajetaTypePtr getReturnType() const { return returnType; }
        bool isReturnsOwnership() const { return returnsOwnership; }
        // Frozen-aware: the cached FunctionType is LLVMContext-bound, so a frozen (shared)
        // function type routes through a per-thread side table. Identical while not frozen.
        llvm::FunctionType* getLlvmFunctionType() const;
        void setLlvmFunctionType(llvm::FunctionType* t);
        // Build the signature FunctionType in `ctx`, for the frozen-stdlib rebuild.
        llvm::FunctionType* buildLlvmFunctionType(llvm::LLVMContext* ctx) const;

        // True iff the LLVM signature uses the sret ABI `void (ptr sret(R), ptr captures,
        // params...)`. Call sites allocate and prepend the result slot when it is true.
        bool usesSret() const;

        // Canonical follows the source form: `(T1,T2) -> R` for sret, `(T1,T2) -> #R` for
        // the heap-ownership form. Two function types are equal iff their canonicals match.
        static std::string buildCanonical(
            const std::vector<CajetaTypePtr>& parameterTypes,
            CajetaTypePtr returnType,
            bool returnsOwnership = true);

        // Mirror of Method::generatePrototype's pass-by-pointer choice at the call
        // boundary; exposed so an indirect call site types itself the same way.
        static llvm::Type* toCallingConvType(CajetaTypePtr p, llvm::Type* ptrTy);
    };

}
