//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include "QualifiedName.h"
#include "cajeta/type/Modifiable.h"
#include "cajeta/type/Annotatable.h"
#include "cajeta/type/CajetaType.h"

using namespace std;

namespace cajeta {
    class CajetaModule;

    class FormalParameter : public Modifiable, public Annotatable {
    private:
        string name;
        cajeta::CajetaType* type;
    public:
        FormalParameter(string name, CajetaType* type, set<Modifier>& modifiers, set<QualifiedName*>& annotations);
        FormalParameter(string name, CajetaType* type) {
            this->name = name;
            this->type = type;
        }
        FormalParameter(const FormalParameter& src) {
            name = src.name;
            type = src.type;
        }

        bool isReference() const;

        const string& getName() const;

        llvm::AllocaInst* createStackInstance(llvm::IRBuilder<>& builder);

        CajetaType* getType() const;

        string toCanonical() {
            string canonical = type->toCanonical();
            canonical.append(":").append(name);
            return canonical;
        }

        static FormalParameter* fromContext(CajetaParser::FormalParameterContext* ctx, CajetaModule* module);
    };
}