//
// Created by James Klappenbach on 2/19/22.
//

#include "cajeta/type/Method.h"
#include "cajeta/type/CajetaStructure.h"

using namespace std;

namespace cajeta {
    Method::Method(string name, CajetaStructure* parent, CajetaType* returnType, list<FormalParameter*>& parameters,
                   set<Modifier>& modifiers, set<QualifiedName*>& annotations) :
    Modifiable(modifiers), Annotatable(annotations) {
        this->parent = parent;
        this->returnType = returnType;
        constructor = parent->getQName()->getTypeName() == name;
        canonical.concat(parent->getQName()->toCanonical());
        canonical.concat("::");
        canonical.concat(name);
        canonical.concat("(");
        if (!parameters.empty()) {
            vector<llvm::Type*> llvmTypes;
            bool staticMethod = modifiers.find(STATIC) != modifiers.end();
            if (!staticMethod) {
                FormalParameter* thisParam = new FormalParameter("this", parent);
                parameters.push_front(thisParam);
            }
            bool first = true;
            for (FormalParameter* parameter : parameters) {
                if (first) {
                    first = false;
                } else {
                    name.append(",");
                }
                llvmTypes.push_back(parameter->getType()->getLlvmType());
                this->parameters.push_back(parameter);
                canonical.concat(parameter->getType()->toCanonical());
            }
            functionType = llvm::FunctionType::get(returnType->getLlvmType(), llvmTypes, false);
        } else {
            functionType = llvm::FunctionType::get(returnType->getLlvmType(), false);
        }

        name.append(")");
    }
}