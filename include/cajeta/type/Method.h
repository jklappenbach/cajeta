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

    class Method : public Modifiable, public Annotatable, public AbstractSyntaxNode {
    private:
        static map<string, Method*> archive;
        string canonical;
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
               CajetaStructure* parent);

        llvm::FunctionType* getFunctionType() { return functionType; }

        bool isConstructor() { return constructor; }

        list<FormalParameter*> getParameters() { return parameters; }

        const string& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        const string& toCanonical() { return canonical; }

        void generateSignature(CompilationUnit* compilationUnit) override;

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
        static string buildCanonical(CajetaStructure* parent,
                                          string name,
                                          list<FormalParameter*>& parameters);

        llvm::Value* generateCode(CompilationUnit* compilationUnit) override {
            return nullptr;
        }
    };
}


