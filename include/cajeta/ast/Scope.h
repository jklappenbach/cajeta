#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include "cajeta/type/Field.h"

namespace cajeta {
    enum ScopeType { MODULE_SCOPE, CLASS_SCOPE, METHOD_SCOPE, BLOCK_SCOPE };

    struct Scope {
        ScopeType scopeType;
        Scope* parent;
        map<string, Field*> fields;
        Scope() {
            parent = NULL;
            scopeType = CLASS_SCOPE;
        }
        Scope(Scope* parent, ScopeType scopeType) {
            this->parent = parent;
            this->scopeType = scopeType;
        }


        ~Scope() {
            for (auto itr = fields.begin(); itr != fields.end(); itr++) {
                Field* field = itr->second;
            }
            fields.clear();
        }

        Field* findField(string fieldName) {
            Field* field = fields[fieldName];
            if (field == NULL && parent != NULL) {
                return parent->findField(fieldName);
            }
            return field;
        }
    };
}

