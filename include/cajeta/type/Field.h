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
    class Initializer;
    class CajetaModule;

    class Field : public Modifiable, public Annotatable {
        bool reference;
        bool var;
        string name;
        int arrayDimension;
        Initializer* initializer;
        cajeta::CajetaType* type;
        llvm::AllocaInst* allocaInst;
    public:
        Field(string name, CajetaType* type) {
            this->name = name;
            this->type = type;
        }
        Field(string name,
              CajetaType* type,
              int arrayDimension,
              bool reference,
              set<Modifier> modifiers,
              Initializer* initializer) : Modifiable(modifiers) {
            this->name = name;
            this->arrayDimension = arrayDimension;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->allocaInst = nullptr;
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

        llvm::AllocaInst* getOrCreateAllocaInst(CajetaModule* module);

        static list<Field*> fromContext(CajetaParser::FieldDeclarationContext* ctx);
    };
}