#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <map>

using namespace std;

namespace cajeta {
    class Field;
    class CajetaModule;

    struct Scope {
        CajetaModule* module;
        Scope* parent;
        map<string, Field*> fields;
        Scope(CajetaModule* module);
        Scope(Scope* parent, CajetaModule* module);

        ~Scope();

        void setField(Field* field);

        Field* getField(string fieldName);
    };
}

