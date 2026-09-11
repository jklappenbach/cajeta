#include "Scope.h"
#include "../field/Field.h"
#include "../field/ParameterField.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"
#include "../ownership/MigrationSwitch.h"
#include "CajetaClass.h"
#include "CajetaFunctionType.h"

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
        // A slotless field has no alloca to reverse-map, and keying the map on
        // null would make every such field alias every other one.
        if (llvm::AllocaInst* slot = field->getOrCreateAllocation()) {
            allocaToField[slot] = field;
        }
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
        // Guard rather than erase: unbinding a name is visible to analyses that
        // run after the block, and the two alloca-keyed maps never collided.
        if (prior != nullptr) {
            fields[fieldName] = prior;
        }
    }

    void Scope::demoteToBorrow(const string& name) {
        demoteToBorrow(name, "");
    }

    // Lend edges live on the HOLDER's declaring scope, as demoteToBorrow and
    // recordCallBorrow also do, so a nested record is visible outside.
    void Scope::recordLend(const string& holder, const string& src) {
        Scope* target = this;
        while (target) {
            if (target->fields.find(holder) != target->fields.end()) break;
            target = target->parent ? target->parent.get() : nullptr;
        }
        if (!target) target = this;
        target->lendEdges[holder].insert(src);
    }

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

    void Scope::rejectTransferOfBorrow(const string& name, bool modeCarrying) {
        FieldPtr field = getField(name);
        if (!field) return;
        bool isFormal = (bool) dynamic_pointer_cast<ParameterField>(field);
        auto klass = dynamic_pointer_cast<CajetaClass>(field->getType());
        // Value types copy and shared-capable values share-bump: no double-free.
        bool titleBearing = klass && !klass->isValueType()
                && !klass->isSharedCapableValue();
        if (!titleBearing) return;

        // EVERY check here belongs to `#v`, which ASSERTS that its source holds
        // a title to surrender. `#=` asserts nothing: it forwards whatever mode
        // the source has, so a titleless source simply makes the destination a
        // borrow — no second owner is minted and nothing can double-free. That
        // is true however the source came to be titleless, whether it never
        // owned (an alias, a field read, a plain call's result) or was demoted
        // by an earlier transfer. `dst = #src` twice is the error; `dst #= src`
        // twice is a transfer followed by a borrow.
        if (modeCarrying) return;

        // (a) Demoted by an EARLIER transfer. Formals included: `isBorrow` is
        // true only once THIS method transferred the name, never from a lend.
        if (isBorrow(name)) {
            string note = transferSiteOf(name);
            throw Exception(
                "cannot transfer ownership of `" + name + "`: it is a borrow"
                    + (note.empty() ? "" : " — already transferred ("
                        + note + ")")
                    + ". You cannot transfer ownership more than once, or "
                      "from a borrow. Fix: transfer from the owner, or "
                      "construct a fresh value.",
                "CAJETA_ERROR_MOVE_OF_BORROW");
        }

        // A formal's ownership is decided at the call site and carried at run
        // time, so rejecting `#p` statically would outlaw mode-forwarding.
        if (isFormal) return;

        // (b) Never owned: an alias or field-read borrow with a known source.
        if (!field->getDropEntry()) {
            string owner = borrowSourceOf(name);
            if (!owner.empty()) {
                throw Exception(
                    "cannot transfer ownership of `" + name
                        + "`: it is a borrow; ownership belongs to `" + owner
                        + "`. You cannot transfer ownership more than once, "
                          "or from a borrow. Fix: transfer from the owner, or "
                          "store an owned value (fresh construction / "
                          "clone()) first.",
                    "CAJETA_ERROR_MOVE_OF_BORROW");
            }
        }

        // (c) A borrow returned by a plain call; these locals DO carry a drop
        // entry. Read off the FIELD: a by-name map collides across methods.
        string callOrigin = field->getCallBorrowOrigin();
        if (!callOrigin.empty()) {
            throw Exception(
                "cannot transfer ownership of `" + name
                    + "`: it holds a BORROW returned by `" + callOrigin
                    + "`, whose return type is not spelled `#`. The owner is "
                      "whatever object that call read the value out of, and it "
                      "still frees it; `#" + name + "` would mint a second "
                      "owner and free it twice. Fix: copy the value and "
                      "transfer the copy, or call an owned-returning (`#`) "
                      "variant if the API has one.",
                "CAJETA_ERROR_MOVE_OF_BORROW");
        }
    }

    bool Scope::holdsStaticTitle(const string& name) {
        FieldPtr field = getField(name);
        if (!field) return false;
        if (dynamic_pointer_cast<ParameterField>(field)) return false;
        auto klass = dynamic_pointer_cast<CajetaClass>(field->getType());
        if (!klass || klass->isValueType() || klass->isSharedCapableValue()) {
            return false;
        }
        if (isBorrow(name)) return false;               // (a) already transferred
        if (!field->getDropEntry()) return false;       // (b) never owned
        if (!field->getCallBorrowOrigin().empty()) return false;  // (c) call borrow
        return true;
    }

    namespace {
        ownership::MigrationSwitch g_capturedBorrow("CAJETA_CAPTURED_BORROW");
    }

    bool Scope::capturedBorrowWarns() { return g_capturedBorrow.warns(); }

    void Scope::setCapturedBorrowWarns(bool on) {
        g_capturedBorrow.setWarns(on);
    }

    void Scope::clearCapturedBorrowWarnsOverride() {
        g_capturedBorrow.clearOverride();
    }

    void Scope::rejectCapturedBorrowParam(const string& srcName,
                                          const string& intoDesc,
                                          int sourceLine) {
        FieldPtr field = getField(srcName);
        if (!field) return;

        // Only a FORMAL: a local is already policed by DANGLING_LEND.
        auto pf = dynamic_pointer_cast<ParameterField>(field);
        string origin = srcName;
        if (!pf) {
            // Reached through an intermediate local, whose provenance is on the
            // FIELD by identity, never by name.
            const string& via = field->getParamBorrowOrigin();
            if (via.empty()) return;
            origin = via;
            FieldPtr pfield = getField(via);
            pf = pfield ? dynamic_pointer_cast<ParameterField>(pfield)
                        : nullptr;
            if (!pf) return;
        }

        // A `#T` formal already told the caller it would be kept.
        auto fp = pf->getFormalParameter();
        if (fp && fp->isTransferred()) return;

        auto klass = dynamic_pointer_cast<CajetaClass>(field->getType());
        auto fnTy = dynamic_pointer_cast<CajetaFunctionType>(field->getType());
        bool titleBearing = (klass && !klass->isValueType()
                && !klass->isSharedCapableValue()) || fnTy != nullptr;
        if (!titleBearing) return;

        string what = (origin == srcName)
            ? ("parameter `" + origin + "`")
            : ("parameter `" + origin + "` (via `" + srcName + "`)");
        string message = fnTy
            ? ("cannot keep " + what + " in " + intoDesc
                + ": a plain parameter is a BORROW — a closure local passed here "
                  "stays the caller's and is freed with its frame, so this object "
                  "would be left pointing at freed memory. Fix: store with `#=`, "
                  "which records the title that arrived (a lambda literal moves "
                  "in, a name lends — the ArrayList model), or spell the "
                  "parameter with a leading `#` on its function type so the call "
                  "site must surrender ownership.")
            : ("cannot keep " + what + " in " + intoDesc
                + ": a plain parameter is a BORROW — the caller keeps the "
                  "title and frees it, so this object would be left pointing "
                  "at freed memory once the call returns. Fix: spell the "
                  "parameter `#" + klass->toCanonical() + "` so the call site "
                  "must surrender ownership, or store with `#=` if this type "
                  "is a container whose caller chooses (the ArrayList model), "
                  "or copy the value.");

        if (!capturedBorrowWarns()) {
            throw Exception(message, "CAJETA_ERROR_CAPTURED_BORROW_PARAM");
        }

        string className;
        string methodName;
        if (module) {
            if (!module->getStructureStack().empty()
                    && module->getStructureStack().back()) {
                className = module->getStructureStack().back()
                    ->getQName()->toCanonical();
            }
            if (MethodPtr m = module->getCurrentMethod()) {
                methodName = m->getName();
            }
        }
        g_capturedBorrow.report(
            "[captured-borrow] " + className + "." + methodName + ":"
                + std::to_string(sourceLine) + " param=" + origin
                + " src=" + srcName + " into=" + intoDesc
                + " type=" + (klass ? klass->toCanonical() : string("(function)")),
            "CAJETA_WARN_CAPTURED_BORROW_PARAM", message,
            module ? module->getSourcePath() : string(), sourceLine);
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
        // Recorded where the ROOT lives, so a move in a nested block still
        // invalidates the outer binding's sub-paths.
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
        borrowedPaths.insert(path);
    }

    bool Scope::isPathBorrow(const string& path) {
        // Any moved PREFIX of `path` invalidates the whole path.
        size_t pos = 0;
        while (true) {
            size_t dot = path.find('.', pos);
            string prefix = (dot == string::npos) ? path : path.substr(0, dot);
            if (borrowedPaths.find(prefix) != borrowedPaths.end()) return true;
            // The root may also sit in the variable-level moved set.
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
        Scope* target = this;
        while (target) {
            auto it = target->notYetAssigned.find(name);
            if (it != target->notYetAssigned.end()) {
                target->notYetAssigned.erase(it);
                return;
            }
            target = target->parent ? target->parent.get() : nullptr;
        }
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
        // Writing W invalidates borrow B when W == B, when W is a strict
        // prefix of B (clobbering the parent of a borrowed sub-path), or when B
        // is a strict prefix of W (writing through what the borrow points at).
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