//
// Created by James Klappenbach on 2/20/22.
//

#include "ParameterField.h"
#include "../compile/CajetaModule.h"

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
            llvm::Type* llvmType;
            if (type->getTypeFlags() & PRIMITIVE_FLAG) {
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
