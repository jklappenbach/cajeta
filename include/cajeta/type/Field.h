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

    class Scope;

    class Field : public Modifiable, public Annotatable {
    protected:
        bool reference;
        string name;
        Initializer* initializer;
        cajeta::CajetaType* type;
        llvm::Value* allocation;

    public:
        Field(string name, CajetaType* type) {
            this->name = name;
            this->type = type;
            this->allocation = nullptr;
        }

        Field(string& name, CajetaType* type, llvm::Value* allocation) {
            this->name = name;
            this->type = type;
            this->allocation = allocation;
        }

        Field(string name,
            CajetaType* type,
            bool reference,
            set<Modifier> modifiers,
            set<QualifiedName*> annotations,
            Initializer* initializer) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->allocation = nullptr;
        }

        Field(string name,
            CajetaType* type,
            bool reference,
            Initializer* initializer,
            set<Modifier> modifiers,
            set<QualifiedName*> annotations) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->allocation = nullptr;
        }

        Field(string& name, bool reference) {
            this->name = name;
            this->reference = reference;
            this->allocation = nullptr;
        }

        void onDelete(CajetaModule* module, Scope* scope);

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

        static list<Field*> fromContext(CajetaParser::FieldDeclarationContext* ctx, CajetaModule* module);
    };
}