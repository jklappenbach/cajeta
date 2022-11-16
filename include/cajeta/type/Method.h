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
#include "Scope.h"
#include <cajeta/type/CajetaType.h>
#include <cajeta/type/Field.h>
#include <queue>
#include <map>

using namespace std;

namespace cajeta {
    class CajetaModule;
    class Expression;
    class CajetaStructure;

    class Method : public Modifiable, public Annotatable {
    private:
        static map<string, Method*> archive;
        string canonical;
        string name;
        CajetaStructure* parent;
        CajetaType* returnType;
        Block* block;
        list<Scope*> scopes;
        bool constructor;
        vector<FormalParameter*> parameters;

        llvm::IRBuilder<>* builder;
        llvm::FunctionType* llvmFunctionType;
        llvm::Function* llvmFunction;
        llvm::BasicBlock* llvmBasicBlock;
    public:
        Method(string& name,
               CajetaType* returnType,
               vector<FormalParameter*>& parameters,
               Block* block,
               CajetaStructure* parent);

        Method(string& name,
               CajetaType* returnType,
               CajetaStructure* parent);

        llvm::FunctionType* getLlvmFunctionType() { return llvmFunctionType; }
        llvm::Function* getLlvmFunction() { return llvmFunction; }

        bool isConstructor() { return constructor; }

        vector<FormalParameter*> getParameters() { return parameters; }

        const string& getName() const {
            return name;
        }
        void setName(const string& name) {
            this->name = name;
        }
        void destroyScope() {
            Scope* scope = scopes.back();
            delete scope;
            scopes.pop_back();
        }
        Field* getVariable(string name) {
            Scope* scope = scopes.back();
            return scope->getField(name);
        }
        const string& toCanonical() { return canonical; }
        void generatePrototype(CajetaModule* module);
        void setBlock(Block* block);
        void createScope(CajetaModule* module);
        void createLocalVariable(CajetaModule* module, Field* field);
        void setLocalVariable(CajetaModule* module, string name, llvm::Value* value);
        void generateCode(CajetaModule* compilationUnit);
        static map<string, Method*>& getArchive();
        static string buildCanonical(CajetaStructure* parent, const string& name, vector<FormalParameter*>& parameters);
        static string buildCanonical(CajetaStructure* parent, const string& name, vector<CajetaType*>& parameters);
        static string buildCanonical(CajetaStructure* parent, const string& name, vector<llvm::Value*>& parameters);
    };
}


