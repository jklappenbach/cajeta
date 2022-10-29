#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "cajeta/type/Field.h"

namespace cajeta {
    struct Scope {
        Scope* parent;
        map<string, Field*> fields;
        Scope() {
            parent = nullptr;
        }
        Scope(Scope* parent) {
            this->parent = parent;
        }

        ~Scope() {
            for (auto fieldEntry : fields) {
                Field* field = fieldEntry.second;
                //field->
            }
            fields.clear();
        }

        Field* get(string fieldName) {
            Field* field = fields[fieldName];
            if (field == nullptr && parent != nullptr) {
                return parent->get(fieldName);
            }
            return field;
        }
    };
}

