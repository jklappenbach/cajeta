//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <cajeta/TypeDefinition.h>
#include <set>
#include <list>
#include "cajeta/module/QualifiedName.h"
#include "Modifiable.h"
#include "Annotatable.h"

using namespace std;

namespace cajeta {
    class Type;

    class Field : public Modifiable, public Annotatable {
        bool reference;
        bool var;
        string name;
        cajeta::Type* type;
    public:
        Field(string& name, bool reference, set<Modifier>& modifiers, set<QualifiedName*>& annotations) : Modifiable(modifiers),
                Annotatable(annotations) {
            this->name = name;
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

        Type* getType() const {
            return type;
        }

        void define() { }
        void allocate(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->allocate(builder, module, ctxLlvm);
        }
        void release(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->free(builder, module, ctxLlvm);
        }
        static list<Field*> fromContext(CajetaParser::FieldDeclarationContext* ctx);
    };
}