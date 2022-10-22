//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "Type.h"
#include "Field.h"
#include "Method.h"
#include "Modifiable.h"

namespace cajeta {
    class CajetaClass : public llvm::StructType {
    protected:
        QualifiedName* qName;
        llvm::Type* llvmType;
        set<Method*> methods;
        list<Field*> fields;
        llvm::LLVMContext* ctx;

    public:
        CajetaClass(llvm::LLVMContext* ctxLlvm, QualifiedName* qName, set<Modifier>& modifiers) : Type(qName, modifiers) {
            this->llvmType = llvm::StructType::create(*ctxLlvm, llvm::StringRef(qName->getTypeName()));
            Type::global[qName] = this;
        }
    };
}