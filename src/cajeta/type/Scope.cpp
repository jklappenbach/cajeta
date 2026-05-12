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

    void Scope::markMoved(const string& name) {
        // Find the scope where the name was declared and record the move there;
        // otherwise record it locally so later checks still see it.
        Scope* target = this;
        while (target) {
            if (target->fields.find(name) != target->fields.end()) {
                target->movedNames.insert(name);
                return;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
        movedNames.insert(name);
    }

    bool Scope::isMoved(const string& name) {
        if (movedNames.find(name) != movedNames.end()) return true;
        if (parent) return parent->isMoved(name);
        return false;
    }
}