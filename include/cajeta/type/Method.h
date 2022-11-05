//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <cajeta/type/Modifiable.h>
#include <llvm/IR/BasicBlock.h>
#include <cajeta/type/QualifiedName.h>
#include <cajeta/type/Annotatable.h>
#include <cajeta/type/CajetaType.h>
#include <cajeta/type/FormalParameter.h>
#include <cajeta/asn/Block.h>
#include <cajeta/asn/Scope.h>
#include <cajeta/type/CajetaType.h>
#include <cajeta/type/Field.h>
#include <queue>
#include <map>

using namespace std;

namespace cajeta {
    class CajetaStructure;
    class CompilationUnit;

    class Method : public Modifiable, public Annotatable {
    private:
        static map<string, Method*> archive;
        llvm::Twine canonical;
        string name;
        CajetaStructure* parent;
        CajetaType* returnType;
        bool constructor;
        list<FormalParameter*> parameters;
        map<string, llvm::AllocaInst*> localVariables;
        llvm::FunctionType* functionType;
        llvm::Function* function;
        Block* block;
        queue<Scope*> scope;
        CajetaParser::MethodBodyContext* methodBodyContext;
    public:
        Method(string& name,
               CajetaType* returnType,
               list<FormalParameter*>& parameters,
               Block* block,
               CompilationUnit* compilationUnit);

        llvm::FunctionType* getFunctionType() { return functionType; }

        bool isConstructor() { return constructor; }

        list<FormalParameter*> getParameters() { return parameters; }

        const string& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        const llvm::Twine& toCanonical() { return canonical; }

        void generate(CompilationUnit* compilationUnit) {
//            llvm::GlobalValue::LinkageTypes linkage;
//            if (modifiers.find(PUBLIC) != modifiers.end() || modifiers.find(PROTECTED) != modifiers.end()) {
//                linkage = llvm::Function::ExternalLinkage;
//            } else {
//                linkage = llvm::Function::PrivateLinkage;
//            }
//
//            bool staticMethod = modifiers.find(STATIC) != modifiers.end();
//            function = llvm::Function::Create(functionType, linkage, canonical,
//                                              ctxParse->getCompilationUnit()->getModule());
//
//            int i = 0;
//            for (FormalParameter* param : parameters) {
//                function->getArg(i)->setName(param->getName());
//            }
        }

        // TODO: move this logic to compilationUnit / visitor
        llvm::AllocaInst* createEntryBlockAlloca(Field* field) {
            llvm::BasicBlock& entryBlock = function->getEntryBlock();
            llvm::IRBuilder<> entryBlockBuilder(&entryBlock, entryBlock.begin());

            llvm::AllocaInst* alloca = field->createStackInstance(entryBlockBuilder);
            return alloca;
        }

        void setMethodBodyContext(CajetaParser::MethodBodyContext* methodBodyContext) {
            this->methodBodyContext = methodBodyContext;
        }

        CajetaParser::MethodBodyContext* getMethodBodyContext() {
            return methodBodyContext;
        }

        void setBlock(Block* block);


        static map<string, Method*>& getArchive();
        static llvm::Twine buildCanonical(CajetaStructure* parent, string name, list<FormalParameter*>& parameters);
    };
}


