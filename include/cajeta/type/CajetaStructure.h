//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/StructureField.h"
#include "cajeta/type/Method.h"
#include "cajeta/type/Scope.h"

namespace cajeta {

    class ClassBodyDeclaration;
    class CajetaModule;

    class CajetaStructure : public CajetaType {
    protected:
        map<string, Method*> methods;
        map<string, StructureField*> fields;
        Scope* scope;
    public:
        CajetaStructure() {
            scope = nullptr;
        }
        CajetaStructure(QualifiedName* qName);
        virtual bool isPrimitive() { return false; }
        Scope* getScope() { return scope; }
        void addMethod(Method* method);
        void addMethods(list<Method*> methods);
        void setClassBody(ClassBodyDeclaration* classBody);
        virtual void generateSignature(CajetaModule* module);
        virtual void generateCode(CajetaModule* module);
        void ensureDefaultConstructor(CajetaModule* module);
        map<string, StructureField*>& getFields() { return fields; }
        map<string, Method*>& getMethods() { return methods; }
    };

} // cajeta