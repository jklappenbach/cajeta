//
// See CajetaFunctionType.h. Builds the canonical name + the cached LLVM
// FunctionType once at construction; both depend only on the immutable
// parameter and return types.
//

#include "CajetaFunctionType.h"
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"
#include "CajetaClass.h"
#include "CajetaStruct.h"

namespace cajeta {

    std::string CajetaFunctionType::buildCanonical(
        const std::vector<CajetaTypePtr>& parameterTypes,
        CajetaTypePtr returnType) {
        std::string s = "(";
        for (size_t i = 0; i < parameterTypes.size(); ++i) {
            if (i > 0) s += ",";
            s += parameterTypes[i] ? parameterTypes[i]->toCanonical() : std::string("?");
        }
        s += ") -> ";
        s += returnType ? returnType->toCanonical() : std::string("?");
        return s;
    }

    CajetaFunctionType::CajetaFunctionType(CajetaModulePtr module,
        std::vector<CajetaTypePtr> parameterTypes,
        CajetaTypePtr returnType)
        : parameterTypes(std::move(parameterTypes)),
          returnType(std::move(returnType)) {
        // Function values are pointers at the value level — a lambda
        // assignment stores the address of the synthesized function. The
        // *signature* lives in llvmFunctionType (cached below) and gets
        // used at call sites to type the indirect call instruction.
        std::string canon = buildCanonical(this->parameterTypes, this->returnType);
        this->qName = QualifiedName::getOrInsert(canon, "");
        this->canonical = canon;
        this->typeFlags = POINTER_FLAG;
        // The value-side LLVM type is `ptr` — a function-typed local holds a
        // pointer to a closure record `{ ptr fn, ptr captures }`. L2-1 keeps
        // the slot type unchanged from L1 (still a single `ptr`); only the
        // pointed-to layout grew. Call sites load the record and indirect-
        // dispatch through fn_ptr, passing captures_ptr as the first arg.
        llvm::Type* ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
        this->llvmType = ptrTy;

        // L2 calling convention: every lambda function takes `ptr captures`
        // as its first arg. Non-capturing lambdas pass null; capturing
        // lambdas (L2-2+) pass the address of their captures struct. The
        // synthesized function ignores the arg in L2-1 — it's about the
        // ABI shape, not capture semantics.
        std::vector<llvm::Type*> llvmParams;
        llvmParams.reserve(this->parameterTypes.size() + 1);
        llvmParams.push_back(ptrTy);
        for (auto& p : this->parameterTypes) {
            llvmParams.push_back(toCallingConvType(p, ptrTy));
        }
        // Same pass-by-pointer rule for the return type: a class-or-array
        // return is conventionally a `ptr` to the heap value, not the
        // struct itself. Without this the indirect-call's return type
        // mismatches the underlying method's signature, which goes
        // through the same coercion in Method::generatePrototype.
        llvm::Type* llvmRet = toCallingConvType(this->returnType, ptrTy);
        if (!llvmRet) {
            llvmRet = llvm::Type::getVoidTy(*module->getLlvmContext());
        }
        this->llvmFunctionType = llvm::FunctionType::get(llvmRet, llvmParams, /*isVarArg=*/false);
    }

    // Mirror of Method::generatePrototype's pass-by-pointer choice for
    // parameter and return types. Class instances and arrays cross the
    // call boundary as `ptr` to the heap value; structs and primitives
    // travel by value. Keeping the rule in lockstep means a method
    // looked up via reference can be called through CajetaFunctionType's
    // signature without per-arg coercion.
    llvm::Type* CajetaFunctionType::toCallingConvType(CajetaTypePtr p, llvm::Type* ptrTy) {
        if (!p) return nullptr;
        bool isStruct = std::dynamic_pointer_cast<CajetaStruct>(p) != nullptr;
        bool isArr = std::dynamic_pointer_cast<CajetaArray>(p) != nullptr;
        bool isClassLike = std::dynamic_pointer_cast<CajetaClass>(p) != nullptr;
        bool isPrim = (p->getTypeFlags() & PRIMITIVE_FLAG) != 0;
        bool passByPointer = (isClassLike && !isStruct) && (isArr || !isPrim);
        return passByPointer ? ptrTy : p->getLlvmType();
    }

}
