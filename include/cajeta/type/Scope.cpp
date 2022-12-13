#include "Scope.h"
#include "cajeta/type/Field.h"
#include <cajeta/compile/CajetaModule.h>

namespace cajeta {
    Scope::Scope(string name, CajetaModule* module) {
        this->name = name;
        this->module = module;
        parent = nullptr;
    }

    Scope::Scope(string name, Scope* parent, CajetaModule* module) {
        this->name = name;
        this->module = module;
        this->parent = parent;
    }

    Scope::~Scope() {
        for (auto itr = fields.rbegin(); itr != fields.rend(); itr++) {
            Field* field = (*itr).second;
            field->onDelete(module, this);
            delete field;
        }
        fields.clear();
    }

    bool Scope::containsField(string fieldName) {
        return fields.find(fieldName) != fields.end();
    }

    void Scope::putField(Field* field) {
        fields[field->getName()] = field;
    }

    Field* Scope::getField(string fieldName) {
        Field* field = fields[fieldName];
        if (field == nullptr && parent != nullptr) {
            return parent->getField(fieldName);
        }
        return field;
    }
}