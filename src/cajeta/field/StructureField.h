//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include "Field.h"

namespace cajeta {

    class StructureField : public Field, public enable_shared_from_this<Field> {
        int index;
    public:
        StructureField(CajetaModulePtr module, string name, CajetaTypePtr type, bool reference, set<Modifier> modifiers,
            set<QualifiedNamePtr> annotations, InitializerPtr initializer, int index, FieldPtr parent = nullptr) :
            Field(module, name, type, reference, modifiers, annotations, initializer, parent) {
            this->index = index;
        }

        StructureField(CajetaModulePtr module, string name, CajetaTypePtr type, llvm::AllocaInst* alloca, int index, FieldPtr parent = nullptr)
            : Field(module, name, type, alloca, parent) {
            this->index = index;
        }

        StructureField(CajetaModulePtr module, string name, CajetaTypePtr type, int index, FieldPtr parent = nullptr)
            : Field(module, name, type, parent) {
            this->index = index;
        }

        llvm::Value* createLoad() override;
    };

} // cajeta