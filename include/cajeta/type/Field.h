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



using namespace std;

namespace cajeta {
    class CajetaType;

    class Field : public Modifiable, public Annotatable {
        bool reference;
        bool var;
        string name;
        cajeta::CajetaType* type;
        llvm::AllocaInst* stackInstance;
    public:
        Field(string& name, CajetaType* type) {
            this->name = name;
            this->type = type;
        }
        Field(string& name, CajetaType* type, bool reference, set<Modifier>& modifiers,
              set<QualifiedName*>& annotations) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->type = type;
            this->reference = reference;
        }
        Field(string& name, bool reference) {
            this->name = name;
            this->reference = reference;
        }

        bool isReference() const {
            return reference;
        }

        bool isVar() const {
            return var;
        }

        const string& getName() const {
            return name;
        }

        CajetaType* getType() const {
            return type;
        }

        llvm::AllocaInst* createStackInstance(llvm::IRBuilder<>& builder);

        static list<Field*> fromContext(CajetaParser::FieldDeclarationContext* ctx);
    };
}