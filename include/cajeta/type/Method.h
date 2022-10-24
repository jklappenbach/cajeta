//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "cajeta/type/Modifiable.h"
#include <llvm/IR/BasicBlock.h>
#include "cajeta/module/QualifiedName.h"
#include "Annotatable.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/ast/FormalParameter.h"
#include "Block.h"

using namespace std;

namespace cajeta {
    class Block;

    class Method : public Modifiable, public Annotatable {
    private:
        string name;
        CajetaType* returnType;
        map<string, FormalParameter*> parameters;
        Block* block;

        llvm::Function* function;
        llvm::BasicBlock* basicBlock;
    public:
        Method(string name, CajetaType* returnType, set<Modifier>& modifiers, set<QualifiedName*>& annotations) :
                Modifiable(modifiers), Annotatable(annotations) {
            this->returnType = returnType;
            this->name = name;
        }

        map<string, FormalParameter*> getParameters() { return parameters; }

        void create() {
            //llvm::FunctionType* functionType = llvm::FunctionType::get(returnType->type, false);
//            llvm::Function* function = llvm::Function::Create(functionType,
//                                                              llvm::Function::ExternalLinkage,
//                                                              methodIdentifier,
//                                                              module);

        }
    };
}


