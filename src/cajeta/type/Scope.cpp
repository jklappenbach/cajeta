#include "Scope.h"
#include "../field/Field.h"
#include "../compile/CajetaModule.h"

namespace cajeta {
    Scope::Scope(string name, CajetaModulePtr module, ScopePtr parent) {
        this->name = name;
        this->module = module;
        this->parent = parent;
    }

    Scope::~Scope() {
        for (auto field: fieldList) {
            field->onDelete();
        }
        fields.clear();
    }

    bool Scope::containsField(string fieldName) {
        return fields.find(fieldName) != fields.end();
    }

    void Scope::putField(FieldPtr field) {
        fields[field->getName()] = field;
        fieldList.push_back(field);
        allocaToField[field->getOrCreateAllocation()] = field;
    }

    FieldPtr Scope::getField(string fieldName) {
        FieldPtr field = fields[fieldName];
        if (field == nullptr && parent != nullptr) {
            return parent->getField(fieldName);
        }
        return field;
    }

    FieldPtr Scope::getField(llvm::AllocaInst* alloca) {
        return allocaToField[alloca];
    }
}