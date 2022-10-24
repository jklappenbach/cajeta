//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include "cajeta/module/QualifiedName.h"
#include "cajeta/type/Modifiable.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/type/CajetaType.h"

using namespace std;

namespace cajeta {
    class FormalParameter : public Modifiable, public Annotatable {
        string name;
        llvm::AllocaInst* definition;
        cajeta::CajetaType* type;
    public:
        FormalParameter(string& name, CajetaType* type, set<Modifier>& modifiers,
                        set<QualifiedName*>& annotations) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
        }
        FormalParameter(string& name) {
            this->name = name;
        }

        bool isReference() const;

        const string& getName() const;

        llvm::AllocaInst* getDefinition() const;

        CajetaType* getType() const;

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