//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include <list>
#include "cajeta/type/QualifiedName.h"
#include "cajeta/type/Modifiable.h"
#include "cajeta/type/Annotatable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/IRBuilder.h>
#include "cajeta/util/MemoryManager.h"

using namespace std;

namespace cajeta {
    class CajetaType;

    class Initializer;

    class CajetaModule;

    class Scope;

    class Field : public Modifiable, public Annotatable {
    protected:
        Field* parent;
        bool reference;
        string name;
        string hierarchicalName;
        Initializer* initializer;
        cajeta::CajetaType* type;
        llvm::Value* origin;
        llvm::Value* load;

        virtual string buildHierarchicalName() {
            if (parent) {
                return parent->buildHierarchicalName() + string(".") + name;
            }
            return name;
        }

    public:
        Field(string name, CajetaType* type, Field* parent = nullptr) {
            this->name = name;
            this->type = type;
            this->parent = parent;
            this->reference = false;
            this->origin = nullptr;
            this->load = nullptr;
        }

        Field(string& name, CajetaType* type, llvm::Value* origin, Field* parent = nullptr) {
            this->name = name;
            this->type = type;
            this->parent = parent;
            this->reference = false;
            this->origin = origin;
            this->load = nullptr;
        }

        Field(string name,
            CajetaType* type,
            bool reference,
            set<Modifier> modifiers,
            set<QualifiedName*> annotations,
            Initializer* initializer, Field* parent = nullptr) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->parent = parent;
            this->origin = nullptr;
            this->load = nullptr;
        }

        Field(string name,
            CajetaType* type,
            bool reference,
            Initializer* initializer,
            set<Modifier> modifiers,
            set<QualifiedName*> annotations,
            Field* parent = nullptr) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->parent = parent;
            this->origin = nullptr;
            this->load = nullptr;
        }

        Field(string& name, bool reference, Field* parent = nullptr) {
            this->name = name;
            this->reference = reference;
            this->parent = parent;
            this->origin = nullptr;
            this->load = nullptr;
        }

        virtual void onDelete(CajetaModule* module, Scope* scope);

        Field* getParent() {
            return parent;
        }

        void setParent(Field* parent) {
            this->parent = parent;
        }

        bool isReference() const {
            return reference;
        }

        const string& getName() const {
            return name;
        }

        const string& getHierarchicalName() {
            if (hierarchicalName.empty() && parent) {
                return hierarchicalName = parent->buildHierarchicalName() + "." + name;
            }
            return hierarchicalName = name;
        }

        CajetaType* getType() const {
            return type;
        }

        virtual llvm::Value* createLoad(CajetaModule* module);

        void setAllocation(llvm::Value* origin) {
            this->origin = origin;
        }

        virtual llvm::Value* getOrCreateAllocation(CajetaModule* module);

        static list<Field*> fromContext(CajetaParser::FieldDeclarationContext* ctx, CajetaModule* module);
    };
}