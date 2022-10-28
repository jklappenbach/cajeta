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
    class CajetaStructure;

    class Method : public Modifiable, public Annotatable {
    private:
        llvm::Twine canonical;
        string name;
        CajetaStructure* parent;
        CajetaType* returnType;
        bool constructor;
        list<FormalParameter*> parameters;
        llvm::FunctionType* functionType;
        llvm::Function* function;
        Block* block;
    public:
        Method(string name, CajetaStructure* parent, CajetaType* returnType, list<FormalParameter*>& parameters,
               set<Modifier>& modifiers, set<QualifiedName*>& annotations);

        llvm::FunctionType* getFunctionType() { return functionType; }

        bool isConstructor() { return constructor; }

        list<FormalParameter*> getParameters() { return parameters; }

        const llvm::Twine& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        void generate(llvm::LLVMContext& llvmContext, llvm::Module& module) {
            llvm::GlobalValue::LinkageTypes linkage;
            if (modifiers.find(PUBLIC) != modifiers.end() || modifiers.find(PROTECTED) != modifiers.end()) {
                linkage = llvm::Function::ExternalLinkage;
            } else {
                linkage = llvm::Function::PrivateLinkage;
            }

            bool staticMethod = modifiers.find(STATIC) != modifiers.end();
            function = llvm::Function::Create(functionType, linkage, canonical, module);

            int i = 0;
            for (FormalParameter* param : parameters) {
                function->getArg(i)->setName(param->getName());
            }
        }

        void addBlock(Block* block) {

        }
    };
}


