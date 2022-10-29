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
        cajeta::CajetaType* type;
    public:
        FormalParameter(string name, CajetaType* type) {
            this->name = name;
            this->type = type;
        }
        FormalParameter(string name, CajetaType* type, set<Modifier>& modifiers,
                        set<QualifiedName*>& annotations) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->type = type;
        }
        FormalParameter(const FormalParameter& src) {
            name = src.name;
            type = src.type;
        }

        bool isReference() const;

        const string& getName() const;

        llvm::AllocaInst* createStackInstance(llvm::IRBuilder<>& builder);

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