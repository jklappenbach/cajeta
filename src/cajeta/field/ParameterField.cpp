// ParameterField - a Field backed by an incoming function parameter.

#include "ParameterField.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaView.h"

namespace cajeta {

    ParameterField::ParameterField(CajetaModulePtr module, FormalParameterPtr formalParameter, llvm::Function* llvmFunction, int paramIndex) :
            Field(module, formalParameter->getName(), formalParameter->getType()),
            formalParameter(formalParameter) {
        reference = false;
        this->llvmFunction = llvmFunction;
        this->paramIndex = paramIndex;
    }


    llvm::Value* ParameterField::createLoad() {
        if (alloca == nullptr) {
            alloca = this->getOrCreateAllocation();
        }
        // A @ValueType slot holds the aggregate INLINE, a reference type a `ptr`.
        if (type->isValueType()) {
            return module->getBuilder()->CreateLoad(type->getLlvmType(), alloca);
        }
        return module->getBuilder()->CreateLoad(
            llvm::PointerType::get(*module->getLlvmContext(), 0), alloca);
    }

    llvm::Value* ParameterField::createStore(llvm::Value* value) {
        if (!alloca) {
            getOrCreateAllocation();
        }
        if (type->getTypeFlags() == PRIMITIVE_FLAG) {
            return module->getBuilder()->CreateStore(value, alloca);
        } else {
            return module->getBuilder()->CreateStore(value, alloca);
        }
    }

    llvm::AllocaInst* ParameterField::getOrCreateAllocation() {
        if (!alloca) {
            // Match generatePrototype's ABI: class and array params arrive as `ptr`.
            bool isStruct = dynamic_pointer_cast<CajetaView>(type) != nullptr;
            bool isArr = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(type) != nullptr;
            bool isPrim = type && (type->getTypeFlags() & PRIMITIVE_FLAG);
            // BY VALUE, so the slot is the aggregate: a ptr-sized one overflows.
            bool passByPointer = (isClassLike && !isStruct) && (isArr || !isPrim)
                && !type->isValueType();
            llvm::Type* llvmType;
            if (passByPointer) {
                llvmType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            } else if ((type->getTypeFlags() & PRIMITIVE_FLAG)
                    || type->isValueType()) {
                llvmType = type->getLlvmType();
            } else {
                llvmType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            }
            // Entry-block alloca: this is lazy, so a first reference in a loop
            // would otherwise re-allocate stack each iteration.
            // A paramIndex past the prototype means the ABI decision changed
            // between prototype and body codegen, so fail NAMING the function.
            if (paramIndex >= (int) llvmFunction->arg_size()) {
                throw Exception(
                    "parameter index " + std::to_string(paramIndex)
                        + " out of range for '"
                        + llvmFunction->getName().str() + "' ("
                        + std::to_string(llvmFunction->arg_size())
                        + " declared args) — prototype/body ABI disagreement",
                    "CAJETA_ERROR_PROTOTYPE_ABI_MISMATCH");
            }
            alloca = module->createEntryAlloca(llvmType);
            module->getBuilder()->CreateStore(llvmFunction->getArg(paramIndex), alloca);
        }
        return alloca;
    }
}
