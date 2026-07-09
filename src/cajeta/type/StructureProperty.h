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
        // element-ownership §7.1.4 — when this field's DECLARED type in the
        // template was `P[]` (array of type parameter P), the index of P in
        // the template's parameter list; -1 otherwise. Monomorphization
        // resolves the field type to the concrete element type, losing the
        // came-from-a-type-parameter fact; this preserves it so the drop
        // walk can pair the field with isTypeArgumentOwning(index) and
        // decide owned-element teardown. Set by TemplateInstantiator's
        // post-walk linkage pass.
        int originElementTypeParamIndex = -1;
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

        int getOriginElementTypeParamIndex() const { return originElementTypeParamIndex; }
        void setOriginElementTypeParamIndex(int idx) { originElementTypeParamIndex = idx; }

        // optional-borrow-ownership 2.2.3.b — same idea for a SCALAR `P`-typed
        // field (`T value` on Optional). Unit 3B covered `P[]` only and left these
        // on the mode-unaware class-ref drop branch, so a borrow-mode
        // instantiation freed a payload it never owned.
        int originTypeParamIndex = -1;

        int getOriginTypeParamIndex() const { return originTypeParamIndex; }
        void setOriginTypeParamIndex(int idx) { originTypeParamIndex = idx; }
    };

    typedef shared_ptr<StructureProperty> StructurePropertyPtr;

} // code