//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaType.h"
#include "Field.h"
#include "Method.h"
#include "Modifiable.h"

namespace cajeta {
    class CajetaClass : public CajetaType {
    public:
        CajetaClass(llvm::LLVMContext* ctxLlvm, QualifiedName* qName, set<Modifier>& modifiers) : CajetaType(qName, modifiers) {
            this->llvmType = llvm::StructType::create(*ctxLlvm, llvm::StringRef(qName->getTypeName()));
            // TODO: FIX ME! Type::global[qName] = this;
        }
    };
}