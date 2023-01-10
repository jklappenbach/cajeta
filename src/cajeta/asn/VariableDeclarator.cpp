//
// Created by James Klappenbach on 11/4/22.
//

#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* VariableDeclarator::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return nullptr;
    }

    llvm::Value* VariableInitializer::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        module->getAsnStack().pop_back();
        return children.back()->generateCode(module);
    }

    llvm::Value* ArrayInitializer::generateCode(CajetaModulePtr module) {
        module->getAsnStack().push_back(shared_from_this());
        for (auto& node: children) {
            node->generateCode(module);
        }
        module->getAsnStack().pop_back();
        return nullptr; //llvm::ConstantStruct::get
    }

} // cajeta