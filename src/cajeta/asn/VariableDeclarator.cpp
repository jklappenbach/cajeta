//
// Created by James Klappenbach on 11/4/22.
//

#include "VariableDeclarator.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    llvm::Value* VariableDeclarator::generateCode(CajetaModulePtr module) {
        return nullptr;
    }

    llvm::Value* VariableInitializer::generateCode(CajetaModulePtr module) {
        return children.back()->generateCode(module);
    }

    llvm::Value* ArrayInitializer::generateCode(CajetaModulePtr module) {
        for (auto& node: children) {
            node->generateCode(module);
        }
        return nullptr; //llvm::ConstantStruct::get
    }

} // code