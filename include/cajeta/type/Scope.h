#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <map>
#include <list>
#include "llvm/IR/Value.h"

using namespace std;

namespace cajeta {
    class Field;

    class CajetaModule;

    /**
     * Supported scopes:
     *
     * Given that we have a single class per module, we have the following layers of scope:
     *
     * 1. Class Static
     * 2. Contained Class Static
     *      2a. Contained Class Instance
     *          2b. Contained Class Method
     * 3. Class Instance
     * 4. Class Method
     */
    class Scope {
    protected:
        string name;
        CajetaModule* module;
        Scope* parent;
        map<string, Field*> fields;
        list<Field*> fieldList;

        void putField(Field* field, string propertyPath);

    public:
        Scope(string name, CajetaModule* module);

        Scope(string name, Scope* parent, CajetaModule* module);

        ~Scope();

        bool containsField(string fieldName);

        void putField(Field* field);

        Field* getField(string fieldName);
    };
}

