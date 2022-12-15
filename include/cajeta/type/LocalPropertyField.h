//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include <cajeta/type/LocalField.h>

namespace cajeta {

    class LocalPropertyField : public LocalField {
        int index;
    public:
        LocalPropertyField(string name, CajetaType* type, bool reference, set<Modifier> modifiers,
            set<QualifiedName*> annotations, Initializer* initializer, Field* parent = nullptr) :
            LocalField(name, type, reference, modifiers, annotations, initializer, parent) { }

        LocalPropertyField(string name, CajetaType* type, llvm::Value* allocation, Field* parent = nullptr) : LocalField(name, type, allocation, parent) { }

        LocalPropertyField(string name, CajetaType* type, Field* parent = nullptr) : LocalField(name, type, parent) { }

        llvm::Value* createLoad(CajetaModule* module) override;

    };

} // cajeta