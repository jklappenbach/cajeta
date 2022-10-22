//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <cajeta/TypeDefinition.h>
#include <set>
#include "cajeta/module/QualifiedName.h"
#include "Modifiable.h"
#include "Annotatable.h"
#include "Type.h"

using namespace std;

namespace cajeta {
    class FormalParameter : public Modifiable, public Annotatable {
        string name;
        TypeDefinition* typeDefinition;
        llvm::AllocaInst* definition;
        cajeta::Type* type;
    public:
        FormalParameter(string& name, Type* type, set<Modifier>& modifiers,
                        set<QualifiedName*>& annotations) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
        }
        FormalParameter(string& name) {
            this->name = name;
        }

        bool isReference() const;

        const string& getName() const;

        TypeDefinition* getTypeDefinition() const;

        llvm::AllocaInst* getDefinition() const;

        Type* getType() const;

        void define() { }
        void allocate(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->allocate(builder, module, ctxLlvm);
        }
        void release(llvm::IRBuilder<>* builder, llvm::Module* module, llvm::LLVMContext* llvmContext) {
            // typeDefinition->free(builder, module, ctxLlvm);
        }
        static FormalParameter* fromContext(CajetaParser::FormalParameterContext*);
    };
}