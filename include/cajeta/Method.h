//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <cajeta/AccessModifier.h>
#include <llvm/IR/BasicBlock.h>
#include <cajeta/TypeDefinition.h>

using namespace std;

namespace cajeta {
    struct Method {
        int accessModifiers;
        llvm::BasicBlock basicBlock;
        std::map<string, TypeDefinition*> types;
    };
}


