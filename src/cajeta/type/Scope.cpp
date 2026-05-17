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

    void Scope::markMovedPath(const string& path) {
        // Record on the scope where the root variable lives, so a move inside
        // a nested block still invalidates the outer binding's sub-paths.
        if (path.empty()) return;
        size_t dot = path.find('.');
        string root = (dot == string::npos) ? path : path.substr(0, dot);
        Scope* target = this;
        while (target) {
            if (target->fields.find(root) != target->fields.end()) {
                target->movedPaths.insert(path);
                return;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
        // Fallback: record locally if the root isn't found in any ancestor.
        movedPaths.insert(path);
    }

    bool Scope::isPathMoved(const string& path) {
        // Check every prefix of `path` ("a", "a.b", "a.b.c") against the
        // moved-path set — if any prefix was moved, the full path is invalid.
        size_t pos = 0;
        while (true) {
            size_t dot = path.find('.', pos);
            string prefix = (dot == string::npos) ? path : path.substr(0, dot);
            if (movedPaths.find(prefix) != movedPaths.end()) return true;
            // The root identifier of the path may also be in the variable-level
            // moved set (covers `#person` followed by a `person.name` read).
            if (pos == 0 && movedNames.find(prefix) != movedNames.end()) return true;
            if (dot == string::npos) break;
            pos = dot + 1;
        }
        if (parent) return parent->isPathMoved(path);
        return false;
    }

    void Scope::markNotYetAssigned(const string& name) {
        notYetAssigned.insert(name);
    }

    void Scope::markAssigned(const string& name) {
        // Walk to the scope where the NYA mark lives and remove it. The mark
        // could be in this scope (assignment in the same block as declaration)
        // or an ancestor (assignment in a nested block).
        Scope* target = this;
        while (target) {
            auto it = target->notYetAssigned.find(name);
            if (it != target->notYetAssigned.end()) {
                target->notYetAssigned.erase(it);
                return;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
        // No mark to remove — fine; the variable was either initialized at
        // declaration or comes from an enclosing class scope.
    }

    bool Scope::isNotYetAssigned(const string& name) {
        if (notYetAssigned.find(name) != notYetAssigned.end()) return true;
        if (parent) return parent->isNotYetAssigned(name);
        return false;
    }
}