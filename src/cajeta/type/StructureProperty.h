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
        // Declaration name position (1-based line, 0-based col), taken from the
        // VariableDeclarator AST node — which, unlike CajetaClass and Method, is
        // an AbstractSyntaxNode and already carries it. 0 = synthesized.
        // Consumed by the xref export (ide-symbol-index §2).
        int declLine = 0;
        int declColumn = 0;
    public:
        int getDeclLine() const { return declLine; }
        int getDeclColumn() const { return declColumn; }

        void setDeclPosition(int line, int column) {
            declLine = line;
            declColumn = column;
        }

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

        // Which type parameter a scalar `P`-typed field came from (`T value`
        // on Optional), or -1. Monomorphization loses the fact; the drop walk
        // needs it to pick the bit-guarded T-origin branch. Set by
        // TemplateInstantiator's post-walk linkage pass.
        int originTypeParamIndex = -1;

        int getOriginTypeParamIndex() const { return originTypeParamIndex; }
        void setOriginTypeParamIndex(int idx) { originTypeParamIndex = idx; }
    };

    typedef shared_ptr<StructureProperty> StructurePropertyPtr;

} // code