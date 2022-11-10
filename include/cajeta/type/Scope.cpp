#include "Scope.h"
#include "cajeta/type/Field.h"

namespace cajeta {
    Scope::~Scope() {
        for (auto& fieldEntry: fields) {
            Field* field = fieldEntry.second;
            field->onDelete();
            delete field;
        }
        fields.clear();
    }

    void Scope::setField(Field* field) {
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