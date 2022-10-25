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
        CajetaClass(llvm::LLVMContext& ctxLlvm, QualifiedName* qName, set<Modifier>& modifiers) : CajetaType(qName, modifiers) {
            llvm::StringRef ref(qName->getTypeName());
            try {
                this->llvmType = llvm::StructType::create(ctxLlvm, "Test");
            } catch (const exception& e) {
                const char* what = e.what();
                cout << what;
            }
            // TODO: FIX ME! Type::global[qName] = this;
        }
    };
}