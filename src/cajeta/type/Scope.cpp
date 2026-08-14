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

    FieldPtr Scope::localBinding(const string& fieldName) {
        auto it = fields.find(fieldName);
        return it == fields.end() ? nullptr : it->second;
    }

    void Scope::restoreBinding(const string& fieldName, FieldPtr prior) {
        // Callers only track names that genuinely shadowed a prior binding, so
        // prior is never null here; guard rather than erase, since unbinding a
        // name is visible to analyses that run after the block (see Block.cpp).
        if (prior != nullptr) {
            fields[fieldName] = prior;
        }
        // fieldList and allocaToField are intentionally left alone: each field
        // owns a distinct alloca, so the shadowing entry never displaced
        // anything there, and both are consulted by alloca (not by name).
    }

    void Scope::demoteToBorrow(const string& name) {
        demoteToBorrow(name, "");
    }

    // 5.2.7 — lend edges live on the HOLDER's declaring scope (same placement
    // rule demoteToBorrow uses), so a lend recorded inside a nested block is still
    // visible at the outer return that escapes the holder.
    void Scope::recordLend(const string& holder, const string& src) {
        Scope* target = this;
        while (target) {
            if (target->fields.find(holder) != target->fields.end()) break;
            target = target->parent ? target->parent.get() : nullptr;
        }
        if (!target) target = this;
        target->lendEdges[holder].insert(src);
    }

    // U2 — same declaring-scope placement as recordLend/demoteToBorrow, so a
    // `#local` inside a nested block still sees the provenance recorded at
    // the declaration.
    void Scope::recordCallBorrow(const string& name, const string& origin) {
        Scope* target = this;
        while (target) {
            if (target->fields.find(name) != target->fields.end()) break;
            target = target->parent ? target->parent.get() : nullptr;
        }
        if (!target) target = this;
        target->callBorrowOrigins[name] = origin;
    }

    string Scope::callBorrowOriginOf(const string& name) {
        Scope* target = this;
        while (target) {
            auto it = target->callBorrowOrigins.find(name);
            if (it != target->callBorrowOrigins.end()) return it->second;
            target = target->parent ? target->parent.get() : nullptr;
        }
        return "";
    }

    set<string> Scope::lendsOf(const string& holder) {
        Scope* target = this;
        while (target) {
            auto it = target->lendEdges.find(holder);
            if (it != target->lendEdges.end()) return it->second;
            target = target->parent ? target->parent.get() : nullptr;
        }
        return {};
    }

    void Scope::demoteToBorrow(const string& name, const string& note) {
        // Find the scope where the name was declared and record the move there;
        // otherwise record it locally so later checks still see it.
        Scope* target = this;
        while (target) {
            if (target->fields.find(name) != target->fields.end()) break;
            target = target->parent ? target->parent.get() : nullptr;
        }
        if (!target) target = this;
        if (target->borrowedBindings.insert(name).second) {
            moveLog.emplace_back(target, name);
        }
        if (!note.empty()) target->transferSites[name] = note;
    }

    void Scope::retractMovesSince(size_t mark) {
        while (moveLog.size() > mark) {
            auto& entry = moveLog.back();
            entry.first->borrowedBindings.erase(entry.second);
            entry.first->transferSites.erase(entry.second);
            moveLog.pop_back();
        }
    }

    vector<Scope::MoveMark> Scope::snapshotMovesSince(size_t mark) const {
        vector<MoveMark> out;
        for (size_t i = mark; i < moveLog.size(); ++i) {
            auto& e = moveLog[i];
            auto nit = e.first->transferSites.find(e.second);
            out.push_back({e.first, e.second,
                nit == e.first->transferSites.end() ? string() : nit->second});
        }
        return out;
    }

    void Scope::reapplyMoves(const vector<MoveMark>& moves) {
        for (auto& m : moves) {
            if (m.target->borrowedBindings.insert(m.name).second) {
                moveLog.emplace_back(m.target, m.name);
            }
            if (!m.note.empty()) m.target->transferSites[m.name] = m.note;
        }
    }

    void Scope::restoreOwnership(const string& name) {
        Scope* target = this;
        while (target) {
            if (target->borrowedBindings.erase(name)) {
                target->transferSites.erase(name);
                return;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
    }

    string Scope::transferSiteOf(const string& name) {
        Scope* target = this;
        while (target) {
            auto it = target->transferSites.find(name);
            if (it != target->transferSites.end()) return it->second;
            target = target->parent ? target->parent.get() : nullptr;
        }
        return "";
    }

    string Scope::borrowSourceOf(const string& name) {
        Scope* target = this;
        while (target) {
            for (auto& entry : target->liveBorrows) {
                if (entry.second.count(name)) return entry.first;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
        return "";
    }

    bool Scope::isBorrow(const string& name) {
        if (borrowedBindings.find(name) != borrowedBindings.end()) return true;
        if (parent) return parent->isBorrow(name);
        return false;
    }

    void Scope::demotePathToBorrow(const string& path) {
        // Record on the scope where the root variable lives, so a move inside
        // a nested block still invalidates the outer binding's sub-paths.
        if (path.empty()) return;
        size_t dot = path.find('.');
        string root = (dot == string::npos) ? path : path.substr(0, dot);
        Scope* target = this;
        while (target) {
            if (target->fields.find(root) != target->fields.end()) {
                target->borrowedPaths.insert(path);
                return;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
        // Fallback: record locally if the root isn't found in any ancestor.
        borrowedPaths.insert(path);
    }

    bool Scope::isPathBorrow(const string& path) {
        // Check every prefix of `path` ("a", "a.b", "a.b.c") against the
        // moved-path set — if any prefix was moved, the full path is invalid.
        size_t pos = 0;
        while (true) {
            size_t dot = path.find('.', pos);
            string prefix = (dot == string::npos) ? path : path.substr(0, dot);
            if (borrowedPaths.find(prefix) != borrowedPaths.end()) return true;
            // The root identifier of the path may also be in the variable-level
            // moved set (covers `#person` followed by a `person.name` read).
            if (pos == 0 && borrowedBindings.find(prefix) != borrowedBindings.end()) return true;
            if (dot == string::npos) break;
            pos = dot + 1;
        }
        if (parent) return parent->isPathBorrow(path);
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

    void Scope::recordLiveBorrow(const string& borrower, const string& borrowedPath) {
        if (borrowedPath.empty() || borrower.empty()) return;
        liveBorrows[borrowedPath].insert(borrower);
    }

    string Scope::findInvalidatingBorrow(const string& writePath) {
        if (writePath.empty()) return "";
        // Check this scope's borrows: writing to W invalidates B if
        // W == B, or W is a strict prefix of B (clobbering a parent
        // path under which a sub-path is borrowed), or B is a strict
        // prefix of W (writing through a sub-field of a borrowed
        // structure mutates what the borrow points at).
        for (auto& entry : liveBorrows) {
            const string& borrowed = entry.first;
            if (borrowed == writePath) return borrowed;
            if (writePath.size() < borrowed.size()
                    && borrowed.compare(0, writePath.size(), writePath) == 0
                    && borrowed[writePath.size()] == '.') {
                return borrowed;
            }
            if (borrowed.size() < writePath.size()
                    && writePath.compare(0, borrowed.size(), borrowed) == 0
                    && writePath[borrowed.size()] == '.') {
                return borrowed;
            }
        }
        if (parent) return parent->findInvalidatingBorrow(writePath);
        return "";
    }

    void Scope::recordLaunchBorrow(const string& bufferName) {
        if (bufferName.empty()) return;
        launchBorrows.insert(bufferName);
    }

    bool Scope::isLaunchBorrowed(const string& bufferName) {
        if (bufferName.empty()) return false;
        if (launchBorrows.count(bufferName)) return true;
        return parent ? parent->isLaunchBorrowed(bufferName) : false;
    }

    void Scope::releaseLaunchBorrows() {
        launchBorrows.clear();
        if (parent) parent->releaseLaunchBorrows();
    }
}