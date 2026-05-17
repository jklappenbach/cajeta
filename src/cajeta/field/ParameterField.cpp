//
// Created by James Klappenbach on 2/20/22.
//

#include "ParameterField.h"
#include "../compile/CajetaModule.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaView.h"

namespace cajeta {

    ParameterField::ParameterField(CajetaModulePtr module, FormalParameterPtr formalParameter, llvm::Function* llvmFunction, int paramIndex) :
            Field(module, formalParameter->getName(), formalParameter->getType()) {
        reference = false;
        this->llvmFunction = llvmFunction;
        this->paramIndex = paramIndex;
    }


    llvm::Value* ParameterField::createLoad() {
        if (alloca == nullptr) {
            alloca = this->getOrCreateAllocation();
        }
        return module->getBuilder()->CreateLoad(type->getLlvmType()->getPointerTo(), alloca);
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
            // Match the ABI Method::generatePrototype chose for this slot:
            // class/array (non-struct) parameters are passed as `ptr`, so
            // the local slot is `ptr` too. Otherwise (primitives, structs)
            // alloc the type itself.
            bool isStruct = dynamic_pointer_cast<CajetaView>(type) != nullptr;
            bool isArr = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
            bool isClassLike = dynamic_pointer_cast<CajetaClass>(type) != nullptr;
            bool isPrim = type && (type->getTypeFlags() & PRIMITIVE_FLAG);
            bool passByPointer = (isClassLike && !isStruct) && (isArr || !isPrim);
            llvm::Type* llvmType;
            if (passByPointer) {
                llvmType = llvm::PointerType::get(*module->getLlvmContext(), 0);
            } else if (type->getTypeFlags() & PRIMITIVE_FLAG) {
                llvmType = type->getLlvmType();
            } else {
                llvmType = type->getLlvmType()->getPointerTo();
            }
            alloca = module->getBuilder()->CreateAlloca(llvmType);
            module->getBuilder()->CreateStore(llvmFunction->getArg(paramIndex), alloca);
        }
        return alloca;
    }
}
