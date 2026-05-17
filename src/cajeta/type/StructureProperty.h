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
    class AbstractSyntaxNode;
    typedef shared_ptr<AbstractSyntaxNode> AbstractSyntaxNodePtr;

    class StructureProperty : public Modifiable, public Annotatable {
    protected:
        string name;
        CajetaTypePtr type;
        int order;
        // Optional declared initializer (`public static int32 base = 100;`,
        // or instance-field defaults once those land). For static
        // properties, evaluated at class-vtable build time and threaded
        // into the LLVM global's initializer. For instance fields, kept
        // for future <init> emission. nullptr when no initializer.
        AbstractSyntaxNodePtr initializer;
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

        AbstractSyntaxNodePtr getInitializer() const { return initializer; }
        void setInitializer(AbstractSyntaxNodePtr init) { initializer = init; }
    };

    typedef shared_ptr<StructureProperty> StructurePropertyPtr;

} // code