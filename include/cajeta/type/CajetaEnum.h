//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "cajeta/type/CajetaStructure.h"

namespace cajeta {
    class CajetaEnum : public CajetaStructure {
        CajetaEnum(llvm::LLVMContext* llvmContext, QualifiedName* qName, set<Modifier>& modifiers) :
                CajetaStructure(llvmContext, qName, modifiers) {
        }
    };
}