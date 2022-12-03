#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <map>
#include "llvm/IR/Value.h"

using namespace std;

namespace cajeta {
    class Field;

    class CajetaModule;

    // TODO: Create PropertyField, LocalField, FormalParameter fields, and ensure they have support with the scope.  Make sure we have static scope,
    class Scope {
    protected:
        CajetaModule* module;
        Scope* parent;
        map<string, Field*> fields;

        void putField(Field* field, string propertyPath);

    public:
        Scope(CajetaModule* module);

        Scope(Scope* parent, CajetaModule* module);

        ~Scope();

        bool containsField(string fieldName);

        void putField(Field* field);

        Field* getField(string fieldName);
    };
}

