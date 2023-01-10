//
// Created by James Klappenbach on 3/22/23.
//

#pragma once
#include "Field.h"

namespace cajeta {

    class StackField : public Field, public enable_shared_from_this<Field> {
    public:
        StackField(CajetaModulePtr module, string name, CajetaTypePtr type, FieldPtr parent = nullptr) : Field(module, name, type, parent) { }

        StackField(CajetaModulePtr module, string name, CajetaTypePtr type, bool reference, set<Modifier> modifiers, set<QualifiedNamePtr> annotations,
            InitializerPtr initializer);

        llvm::Value* createLoad() override;
        llvm::Value* createStore(llvm::Value* value) override;
        llvm::AllocaInst* getOrCreateAllocation() override;
    };

} // cajeta