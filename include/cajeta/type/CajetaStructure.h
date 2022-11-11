//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/Field.h"
#include "cajeta/type/Method.h"
#include "cajeta/type/Scope.h"

namespace cajeta {

    class ClassBodyDeclaration;
    class CajetaModule;

    class CajetaStructure : public CajetaType {
    private:
        map<string, Method*> methods;
        map<string, Field*> fields;
        CajetaModule* module;
        Scope* scope;
    public:
        CajetaStructure(CajetaModule* module, QualifiedName* qName);
        virtual bool isPrimitive() { return false; }
        Scope* getScope() { return scope; }
        void addField(Field* field);
        void addFields(list<Field*> fields);
        void addMethod(Method* method);
        void addMethods(list<Method*> methods);
        void setClassBody(ClassBodyDeclaration* classBody);

        void generateSignature(CajetaModule* module);
        void generateCode(CajetaModule* module);
        void ensureDefaultConstructor(CajetaModule* module);
        map<string, Field*>& getFields() { return fields; }
        map<string, Method*>& getMethods() { return methods; }
    };

} // cajeta