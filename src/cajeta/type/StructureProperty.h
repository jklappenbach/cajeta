//
// Created by James Klappenbach on 11/14/22.
//

#pragma once

#include <string>
#include "Modifiable.h"
#include "Annotatable.h"

namespace cajeta {
    class CajetaType;
    typedef shared_ptr<CajetaType> CajetaTypePtr;

    class StructureProperty : public Modifiable, public Annotatable {
    protected:
        string name;
        CajetaTypePtr type;
        int order;
    public:
        StructureProperty(string name, int order) {
            this->name = name;
            this->order = order;
        }


        StructureProperty(string name, CajetaTypePtr type, int order) {
            this->name = name;
            this->type = type;
            this->order = order;
        }

        StructureProperty(string name,
            CajetaTypePtr type,
            set<Modifier> modifiers,
            set<QualifiedNamePtr> annotations,
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

        CajetaTypePtr getType() const {
            return type;
        }

        void setType(CajetaTypePtr type) {
            this->type = type;
        }

        void setOrder(int order) { this->order = order; }

        int getOrder() { return order; }
    };

    typedef shared_ptr<StructureProperty> StructurePropertyPtr;

} // code