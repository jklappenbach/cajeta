//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include "LocalField.h"

namespace cajeta {

    class LocalPropertyField : public LocalField {
        int index;
    public:
        LocalPropertyField(string name, CajetaType* type, bool reference, set<Modifier> modifiers,
            set<QualifiedName*> annotations, Initializer* initializer, int index, Field* parent = nullptr) :
            LocalField(name, type, reference, modifiers, annotations, initializer, parent) {
            this->index = index;
        }

        LocalPropertyField(string name, CajetaType* type, llvm::Value* allocation, int index, Field* parent = nullptr)
                : LocalField(name, type, allocation, parent) {
            this->index = index;
        }

        LocalPropertyField(string name, CajetaType* type, int index, Field* parent = nullptr)
                : LocalField(name, type, parent) {
            this->index = index;
        }

        llvm::Value* createLoad(CajetaModule* module) override;

    };

} // cajeta