//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "cajeta/type/CajetaClass.h"

namespace cajeta {
    class CajetaEnum : public CajetaClass {
        CajetaEnum(llvm::LLVMContext* llvmContext, QualifiedName* qName, set<Modifier>& modifiers) :
            CajetaClass(llvmContext, qName) {
        }
    };
}