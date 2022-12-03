//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include <cajeta/type/Field.h>

using namespace std;

namespace cajeta {
    class CajetaModule;

    class Method;

    class FormalParameter : public Field {
    protected:
        Method* parent;
    public:
        FormalParameter(string name, CajetaType* type, bool reference, set<Modifier>& modifiers,
            set<QualifiedName*>& annotations);

        FormalParameter(string name, CajetaType* type) : Field(name, type) {
            this->name = name;
            this->type = type;
        }

        FormalParameter(const FormalParameter& src) : Field(src.getName(), src.getType()) {
            parent = src.parent;
            name = src.name;
            type = src.type;
        }

        Method* getParent() const;

        void setParent(Method* parent);

        bool isReference() const;

        const string& getName() const;

        llvm::AllocaInst* createStackInstance(llvm::IRBuilder<>& builder);

        CajetaType* getType() const;

        string toCanonical();

        static FormalParameter* fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModule* module);
    };
}