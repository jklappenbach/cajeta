//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include <string>
#include <cajeta/type/Modifiable.h>
#include <cajeta/type/Annotatable.h>

namespace cajeta {
    class CajetaType;

    class ClassProperty : public Modifiable, public Annotatable {
    protected:
        string name;
        CajetaType* type;
        int order;
    public:
        ClassProperty(string name,
            CajetaType* type,
            int order) {
            this->name = name;
            this->type = type;
            this->order = order;
        }

        ClassProperty(string name, CajetaType* type) {
            this->name = name;
            this->type = type;
        }

        ClassProperty(string name, int order) {
            this->name = name;
            this->order = order;
        }

        ClassProperty(string name,
            CajetaType* type,
            set<Modifier> modifiers,
            set<QualifiedName*> annotations,
            int order) : Modifiable(modifiers), Annotatable(annotations) {
            this->name = name;
            this->type = type;
            this->order = order;
        }

        const string& getName() const {
            return name;
        }

        void setName(const string& name) {
            this->name = name;
        }

        CajetaType* getType() const {
            return type;
        }

        void setType(CajetaType* type) {
            this->type = type;
        }

        void setOrder(int order) { this->order = order; }

        int getOrder() { return order; }
    };

} // cajeta