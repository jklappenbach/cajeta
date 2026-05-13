//
// See CajetaFunctionType.h. Builds the canonical name + the cached LLVM
// FunctionType once at construction; both depend only on the immutable
// parameter and return types.
//

#include "CajetaFunctionType.h"
#include "../compile/CajetaModule.h"

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
        this->llvmType = llvm::PointerType::get(*module->getLlvmContext(), 0);

        std::vector<llvm::Type*> llvmParams;
        llvmParams.reserve(this->parameterTypes.size());
        for (auto& p : this->parameterTypes) {
            llvmParams.push_back(p->getLlvmType());
        }
        llvm::Type* llvmRet = this->returnType
            ? this->returnType->getLlvmType()
            : llvm::Type::getVoidTy(*module->getLlvmContext());
        this->llvmFunctionType = llvm::FunctionType::get(llvmRet, llvmParams, /*isVarArg=*/false);
    }

}
