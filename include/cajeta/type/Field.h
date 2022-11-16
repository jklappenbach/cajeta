//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include <list>
#include "QualifiedName.h"
#include "Modifiable.h"
#include "Annotatable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/IRBuilder.h>
#include <cajeta/util/MemoryManager.h>

using namespace std;

namespace cajeta {
    class CajetaType;
    class Initializer;
    class CajetaModule;

    class Field : public Modifiable, public Annotatable {
    private:
        bool reference;
        string name;
        int arrayDimension;
        Initializer* initializer;
        cajeta::CajetaType* type;
        llvm::Value* allocation;

    public:
        Field(string name, CajetaType* type) {
            this->name = name;
            this->type = type;
        }
        Field(string name, llvm::Value* allocation) {
            
        }
        Field(string name,
              CajetaType* type,
              int arrayDimension,
              bool reference,
              set<Modifier> modifiers,
              set<QualifiedName*> annotations,
              Initializer* initializer) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->arrayDimension = arrayDimension;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->allocation = nullptr;
        }
        Field(string name,
              CajetaType* type,
              int arrayDimension,
              bool reference,
              Initializer* initializer,
              set<Modifier> modifiers,
              set<QualifiedName*> annotations) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->arrayDimension = arrayDimension;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
        }
        Field(string& name, bool reference, int arrayDimension = 0) {
            this->name = name;
            this->reference = reference;
            this->arrayDimension = arrayDimension;
        }

        void onDelete(CajetaModule* module);

        bool isReference() const {
            return reference;
        }
        const string& getName() const {
            return name;
        }

        CajetaType* getType() const {
            return type;
        }

        void setAllocation(llvm::Value* allocation) {
            this->allocation = allocation;
        }

        llvm::Value* getAllocation() {
            return allocation;
        }

        llvm::Value* getOrCreateStackAllocation(CajetaModule* module);
        llvm::Value* getOrCreateAllocation(CajetaModule* module);
        static list<Field*> fromContext(CajetaParser::FieldDeclarationContext* ctx);
    };
}