//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <cajeta/Modifiable.h>
#include <llvm/IR/BasicBlock.h>
#include <cajeta/TypeDefinition.h>
#include "QualifiedName.h"
#include "Annotatable.h"
#include "Type.h"
#include "FormalParameter.h"

using namespace std;

namespace cajeta {
    class Method : public Modifiable, public Annotatable {
    private:
        string name;
        Type* returnType;
        map<string, FormalParameter*> parameters;

        llvm::Function* function;
        llvm::BasicBlock* basicBlock;
    public:
        Method(string name, Type* returnType, set<Modifier>& modifiers, set<QualifiedName*>& annotations) :
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


