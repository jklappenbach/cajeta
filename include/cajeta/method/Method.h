//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include "cajeta/type/Modifiable.h"
#include <llvm/IR/BasicBlock.h>
#include "cajeta/type/QualifiedName.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/asn/Block.h"
#include "cajeta/type/Scope.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/field/Field.h"
#include <queue>
#include <map>

using namespace std;

namespace cajeta {
    class CajetaModule;

    class Expression;

    class CajetaStructure;

    class Method : public Modifiable, public Annotatable {
    protected:
        static map<string, Method*> archive;
        string canonical;
        string name;
        CajetaStructure* parent;
        CajetaType* returnType;
        Block* block;
        bool constructor;
        map<string, FormalParameter*> parameters;
        vector<FormalParameter*> parameterList;
        int virtualTableIndex;

        CajetaModule* module;
        llvm::IRBuilder<>* builder;
        llvm::FunctionType* llvmFunctionType;
        llvm::Function* llvmFunction;
        llvm::BasicBlock* llvmBasicBlock;
    public:
        Method(CajetaModule* module,
            string& name,
            CajetaType* returnType,
            vector<FormalParameter*>& parameters,
            Block* block,
            CajetaStructure* parent);

        Method(CajetaModule* module,
            string name,
            CajetaType* returnType,
            CajetaStructure* parent);

        llvm::FunctionType* getLlvmFunctionType() { return llvmFunctionType; }

        llvm::Function* getLlvmFunction() { return llvmFunction; }

        bool isConstructor() { return constructor; }

        vector<FormalParameter*> getParameterList() { return parameterList; }

        map<string, FormalParameter*> getParameters() { return parameters; }

        CajetaType* getReturnType() { return returnType; }

        const string& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        int getVirtualTableIndex() {
            return virtualTableIndex;
        }

        void setVirtualTableIndex(int virtualTableIndex) {
            this->virtualTableIndex = virtualTableIndex;
        }

        bool operator<(const Method& src) const {
            return virtualTableIndex < src.virtualTableIndex;
        }

        void destroyScope();

        Field* getVariable(string name);

        CajetaModule* getModule() { return module; }

        const string& toCanonical() { return canonical; }

        void generatePrototype();

        void setBlock(Block* block);

        void createScope();

        void putScope(Field* field);

        void createLocalVariable(CajetaModule* module, Field* field);

        void setLocalVariable(CajetaModule* module, string name, llvm::Value* value);

        virtual void generateCode();

        static map<string, Method*>& getArchive();

        static string buildCanonical(CajetaStructure* parent, const string& name, vector<FormalParameter*>& parameters);

        static string buildCanonical(CajetaStructure* parent, const string& name, vector<CajetaType*>& parameters);

        static string buildCanonical(CajetaStructure* parent, const string& name, vector<llvm::Value*>& parameters);
    };
}


