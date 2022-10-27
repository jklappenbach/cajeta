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
        bool constructor;
        map<string, FormalParameter*> parameters;
        Block* block;
        llvm::FunctionType* functionType;
        llvm::Function* function;
        llvm::BasicBlock* basicBlock;
    public:
        Method(string name, CajetaType* returnType, bool constructor, set<Modifier>& modifiers, set<QualifiedName*>& annotations) :
                Modifiable(modifiers), Annotatable(annotations) {
            this->returnType = returnType;
            this->name = name;
            this->constructor = constructor;
        }

        bool isConstructor() { return constructor; }

        map<string, FormalParameter*> getParameters() { return parameters; }

        const string& getName() const {
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
            function = llvm::Function::Create(functionType, linkage, name, module);
        }

        string toString() {
            string value = returnType->getQName()->toString() + " " + name + "(";
            bool first = true;
            for (const auto& paramEntry : parameters) {
                if (first) {
                    first = false;
                } else {
                    value += ",";
                }
                value += paramEntry.second->getType()->getQName()->toString() + " " + paramEntry.first;
            }
            value += ");";
        }
    };
}


