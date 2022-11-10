#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <map>

using namespace std;

namespace cajeta {
    class Field;

    struct Scope {
        Scope* parent;
        map<string, Field*> fields;
        Scope() {
            parent = nullptr;
        }
        Scope(Scope* parent) {
            this->parent = parent;
        }

        ~Scope();

        void setField(Field* field);

        Field* getField(string fieldName);
    };
}

