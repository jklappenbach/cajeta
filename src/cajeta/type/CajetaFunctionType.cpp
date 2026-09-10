// See CajetaFunctionType.h. Builds the canonical name and the cached LLVM
// FunctionType at construction, from the immutable parameter/return types.

#include "CajetaFunctionType.h"
#include "../compile/CajetaModule.h"
#include "CajetaArray.h"
#include "CajetaClass.h"
#include "CajetaView.h"
#include "../compile/CompilationContext.h"
#include <unordered_map>

namespace cajeta {

    // Per-thread side-table for a frozen function type's cached LLVM
    // FunctionType, which is LLVMContext-bound and so cannot be shared.
    static std::unordered_map<const CajetaFunctionType*, llvm::FunctionType*>& frozenFnTypeBindings() {
        static thread_local std::unordered_map<const CajetaFunctionType*, llvm::FunctionType*> tbl;
        return tbl;
    }

    llvm::FunctionType* CajetaFunctionType::getLlvmFunctionType() const {
        if (isFrozen()) {
            auto& tbl = frozenFnTypeBindings();
            auto it = tbl.find(this);
            if (it != tbl.end()) return it->second;
            llvm::LLVMContext* ctx = currentLlvmContext();
            if (!ctx) return nullptr;
            llvm::FunctionType* t = buildLlvmFunctionType(ctx);
            tbl[this] = t;
            return t;
        }
        return llvmFunctionType;
    }

    void CajetaFunctionType::setLlvmFunctionType(llvm::FunctionType* t) {
        if (isFrozen()) { frozenFnTypeBindings()[this] = t; return; }
        llvmFunctionType = t;
    }

    // Whether the `#` in the canonical name carries meaning for `returnType`.
    // Non-sret-eligible returns share one LLVM signature either way, so they all
    // normalize to "#" rather than splitting `(P) -> R` into two distinct types.
    static bool sretCanonicalDiscriminates(const CajetaTypePtr& returnType) {
        auto rtClass = std::dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass || rtClass->isInterface()) return false;
        if (std::dynamic_pointer_cast<CajetaArray>(returnType)) return false;
        if (std::dynamic_pointer_cast<CajetaView>(returnType)) return false;
        return true;
    }

    std::string CajetaFunctionType::buildCanonical(
        const std::vector<CajetaTypePtr>& parameterTypes,
        CajetaTypePtr returnType,
        bool returnsOwnership) {
        std::string s = "(";
        for (size_t i = 0; i < parameterTypes.size(); ++i) {
            if (i > 0) s += ",";
            s += parameterTypes[i] ? parameterTypes[i]->toCanonical() : std::string("?");
        }
        s += ") -> ";
        if (returnsOwnership || !sretCanonicalDiscriminates(returnType)) s += "#";
        s += returnType ? returnType->toCanonical() : std::string("?");
        return s;
    }

    CajetaFunctionType::CajetaFunctionType(CajetaModulePtr module,
        std::vector<CajetaTypePtr> parameterTypes,
        CajetaTypePtr returnType,
        bool returnsOwnership)
        : parameterTypes(std::move(parameterTypes)),
          returnType(std::move(returnType)),
          returnsOwnership(returnsOwnership) {
        std::string canon = buildCanonical(this->parameterTypes, this->returnType, returnsOwnership);
        this->qName = QualifiedName::getOrInsert(canon, "");
        this->canonical = canon;
        this->typeFlags = POINTER_FLAG;
        setLlvmType(llvm::PointerType::get(*module->getLlvmContext(), 0));  // U6.2
        setLlvmFunctionType(buildLlvmFunctionType(module->getLlvmContext()));  // U6.4.1
    }

    llvm::FunctionType* CajetaFunctionType::buildLlvmFunctionType(llvm::LLVMContext* ctx) const {
        // A function value is a `ptr` to a closure record `{ ptr fn, ptr captures }`;
        // call sites load it and indirect-dispatch through fn, passing captures first.
        llvm::Type* ptrTy = llvm::PointerType::get(*ctx, 0);

        // Two shapes, both mirroring Method::generatePrototype: ownership form
        // `R (ptr captures, params...)`, sret form `void (ptr sret(R), ptr captures,
        // params...)`. The sret attribute itself is set on the Function/CallInst.
        bool useSret = false;
        if (!returnsOwnership) {
            auto rtClass = std::dynamic_pointer_cast<CajetaClass>(this->returnType);
            bool isArr = std::dynamic_pointer_cast<CajetaArray>(this->returnType) != nullptr;
            bool isView = std::dynamic_pointer_cast<CajetaView>(this->returnType) != nullptr;
            useSret = rtClass && !rtClass->isInterface() && !isArr && !isView;
        }
        std::vector<llvm::Type*> llvmParams;
        llvmParams.reserve(this->parameterTypes.size() + 2);
        if (useSret) {
            llvmParams.push_back(ptrTy);  // sret slot — param 0
        }
        llvmParams.push_back(ptrTy);  // captures — param 0 (ownership form) or 1 (sret form)
        for (auto& p : this->parameterTypes) {
            llvmParams.push_back(toCallingConvType(p, ptrTy));
        }
        llvm::Type* llvmRet;
        if (useSret) {
            llvmRet = llvm::Type::getVoidTy(*ctx);
        } else {
            llvmRet = toCallingConvType(this->returnType, ptrTy);
            if (!llvmRet) {
                llvmRet = llvm::Type::getVoidTy(*ctx);
            }
        }
        return llvm::FunctionType::get(llvmRet, llvmParams, /*isVarArg=*/false);
    }

    bool CajetaFunctionType::usesSret() const {
        if (returnsOwnership) return false;
        auto rtClass = std::dynamic_pointer_cast<CajetaClass>(returnType);
        if (!rtClass || rtClass->isInterface()) return false;
        if (std::dynamic_pointer_cast<CajetaArray>(returnType)) return false;
        if (std::dynamic_pointer_cast<CajetaView>(returnType)) return false;
        return true;
    }

    llvm::Type* CajetaFunctionType::toCallingConvType(CajetaTypePtr p, llvm::Type* ptrTy) {
        if (!p) return nullptr;
        bool isStruct = std::dynamic_pointer_cast<CajetaView>(p) != nullptr;
        bool isArr = std::dynamic_pointer_cast<CajetaArray>(p) != nullptr;
        bool isClassLike = std::dynamic_pointer_cast<CajetaClass>(p) != nullptr;
        bool isPrim = (p->getTypeFlags() & PRIMITIVE_FLAG) != 0;
        bool passByPointer = (isClassLike && !isStruct) && (isArr || !isPrim);
        return passByPointer ? ptrTy : p->getLlvmType();
    }

}
