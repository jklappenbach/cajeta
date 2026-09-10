#pragma once

#include <string>
#include <map>
#include <list>
#include <set>
#include "llvm/IR/Instructions.h"

using namespace std;

namespace cajeta {
    class Field;
    typedef shared_ptr<Field> FieldPtr;
    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;
    class Scope;
    typedef shared_ptr<Scope> ScopePtr;


    /** A name-binding layer: class static, contained-class static / instance /
     *  method, class instance, class method. There is one Scope per method. */
    class Scope {
    protected:
        string name;
        CajetaModulePtr module;
        ScopePtr parent;
        map<string, FieldPtr> fields;
        list<FieldPtr> fieldList;
        map<llvm::AllocaInst*, FieldPtr> allocaToField;
        // Identifiers moved out via `#` in this or an enclosing scope; read paths
        // consult this set and reject them (Identifier.cpp).
        set<string> borrowedBindings;
        // Per-name transfer sites for borrowedBindings, appended to diagnostics.
        map<string, string> transferSites;
        // Ordered log of moves recorded through THIS scope (the target may be an
        // ancestor). Only genuinely NEW marks are appended, so retracting a slice
        // exactly undoes it.
        vector<pair<Scope*, string>> moveLog;
        // Field-access paths ("person.name", "a.b.c") that have been moved out;
        // DotExpression checks this set with prefix semantics.
        set<string> borrowedPaths;
        // Definite assignment: names declared in this scope with no initializer and
        // not yet assigned, which read paths reject. Initialized locals, parameters
        // and enclosing-class fields are never here, and removal is permanent.
        set<string> notYetAssigned;

        // Live read-borrows: a borrowed path ("p.name", or "p") to the local names
        // borrowing from it. A write whose target path overlaps a live borrow is
        // rejected with CAJETA_ERROR_MOVE_OF_BORROW; cleared with this scope.
        map<string, set<string>> liveBorrows;

        // Launch borrow scope: buffer locals with an in-flight `kernel.launch`.
        // Tracked apart from liveBorrows because a launch borrow is released at an
        // explicit `Stream.sync()` / `Event.waitHost()`, not at scope exit.
        set<string> launchBorrows;

        // Single-hop dangling lend: a holder local to the LOCAL owners it holds a
        // plain (non-`#`) lend of. If the holder escapes, those sources die at scope
        // exit — CAJETA_ERROR_DANGLING_LEND, suppressed by spelling `#s` at the lend.
        map<string, set<string>> lendEdges;

        // Call-result provenance: a local to the call it was initialised from when
        // that callee's BODY proves an interior view (Method::returnsInteriorView).
        // Such a local holds no title, so `#local` would mint a second owner.
        map<string, string> callBorrowOrigins;

        void putField(FieldPtr field, string propertyPath);

    public:
        Scope(string name, CajetaModulePtr module, ScopePtr parent = nullptr);

        ~Scope();

        bool containsField(string fieldName);

        /** Add a field to this scope. */
        void putField(FieldPtr field);

        /** The field bound to `fieldName`, for resolving identifiers; throws when
         *  no enclosing scope binds it. */
        FieldPtr getField(string fieldName);

        FieldPtr getField(llvm::AllocaInst* alloca);

        // The binding of `fieldName` in THIS scope only, or null when unbound —
        // what Block::generateCode snapshots and restores at the `}`. Never inserts,
        // unlike getField, so "unbound" stays distinguishable from "bound to null".
        FieldPtr localBinding(const string& fieldName);

        // Put back the binding `localBinding` returned, at the close of a block.
        void restoreBinding(const string& fieldName, FieldPtr prior);

        void setParent(ScopePtr parent) {
            this->parent = parent;
        }

        // Mark an identifier moved-out: later reads in this or any child scope are a
        // use-after-move error. Recorded on the scope that DECLARES the field, so a
        // move in a nested block still invalidates the outer binding.
        void demoteToBorrow(const string& name);

        // As demoteToBorrow, also recording the transfer site diagnostics append.
        void demoteToBorrow(const string& name, const string& note);

        // Re-arm: a fresh assignment clears the moved state, on the declaring scope.
        void restoreOwnership(const string& name);

        // The note recorded with the move of `name`, or "".
        string transferSiteOf(const string& name);

        // Terminated-path retraction: a block retracts its slice on return/throw.
        size_t moveLogSize() const { return moveLog.size(); }
        void retractMovesSince(size_t mark);

        // Branch-arm move state: snapshot, restore for the sibling, union at the join.
        struct MoveMark { Scope* target; string name; string note; };
        vector<MoveMark> snapshotMovesSince(size_t mark) const;
        void reapplyMoves(const vector<MoveMark>& moves);

        // Record that `name` was initialised from a call whose BODY proves an
        // interior view (Method::returnsInteriorView). The criterion is the body,
        // never the return spelling, so anything unproven stays unrecorded.
        void recordCallBorrow(const string& name, const string& origin);
        // The proven-view call `name` came from, or "".
        string callBorrowOriginOf(const string& name);

        // The transfer-of-a-borrow rejection, shared by EVERY site that can spell
        // `#x` on a named local: throws CAJETA_ERROR_MOVE_OF_BORROW when `name` holds
        // a borrow. `modeCarrying` (a `#=` store) keeps only the double-transfer check.
        void rejectTransferOfBorrow(const string& name,
                                    bool modeCarrying = false);

        // True when `name` statically holds a title of its own — not an alias, a
        // formal, or a plain call's borrow. The non-throwing companion to
        // rejectTransferOfBorrow; a `#=` consumes its source only when this is true.
        bool holdsStaticTitle(const string& name);

        // The CALLEE-side mirror: throws CAJETA_ERROR_CAPTURED_BORROW_PARAM when
        // `srcName` is a plain parameter stored beyond the call, naming `#T` as the
        // fix. `intoDesc` names the destination; `sourceLine` is for warn mode only.
        void rejectCapturedBorrowParam(const string& srcName,
                                       const string& intoDesc,
                                       int sourceLine = -1);

        // The warning-first migration switch: ERROR by default, demoted by
        // CAJETA_CAPTURED_BORROW=warn so one build enumerates every site instead of
        // stopping at the first. Read per call, so a test may flip it mid-process.
        static bool capturedBorrowWarns();
        static void setCapturedBorrowWarns(bool on);
        static void clearCapturedBorrowWarnsOverride();

        // Record that `holder` now holds a lend of the local owner `src`.
        void recordLend(const string& holder, const string& src);
        // The local owners `holder` holds lends of (empty when none).
        set<string> lendsOf(const string& holder);

        // The source path `name` borrows from, walked across ancestors, or "".
        string borrowSourceOf(const string& name);

        // True iff `name` has been moved-out in this scope or any ancestor.
        bool isBorrow(const string& name);

        // Mark a field-access path moved ("person.address.city"): later reads of it,
        // or of any path it prefixes, are rejected. Single names use demoteToBorrow.
        void demotePathToBorrow(const string& path);

        // True iff `path` or an ancestor prefix of it is moved, here or in an
        // ancestor scope: "a.b.c" counts as moved if "a" or "a.b" was marked.
        bool isPathBorrow(const string& path);

        // Mark a name declared-but-not-yet-assigned (`MyClass x;`), on this scope.
        // Reads in it or any child scope must see an assignment first.
        void markNotYetAssigned(const string& name);

        // Mark a name definitely assigned, clearing its NYA mark on the declarer.
        void markAssigned(const string& name);

        // True iff `name` was declared without an initializer here or in an ancestor
        // and has not been assigned since; reading such a name is a compile error.
        bool isNotYetAssigned(const string& name);

        // Record a live read-borrow on this scope: `borrower` is the local holding
        // it, `borrowedPath` the dotted source ("p.name", or "p" for a whole local).
        void recordLiveBorrow(const string& borrower, const string& borrowedPath);

        // The path of the first live borrow — here or in an ancestor — that writing
        // `writePath` would invalidate, or "" when the write is safe. A write
        // invalidates a borrow when either path is a prefix of the other.
        string findInvalidatingBorrow(const string& writePath);

        // Launch borrow scope: record marks `bufferName` borrowed by an in-flight
        // launch, isLaunchBorrowed tests this scope and its ancestors, release clears
        // them all at a sync point, and pending lists what is still borrowed here.
        void recordLaunchBorrow(const string& bufferName);
        bool isLaunchBorrowed(const string& bufferName);
        void releaseLaunchBorrows();
        const set<string>& pendingLaunchBorrows() const { return launchBorrows; }

        // Flow-analysis snapshots: a branching node saves the NYA set before each
        // branch and merges after. The merge unions, so a name is NYA past the join
        // iff it was NYA in EITHER branch (definitely assigned iff in BOTH).
        set<string> snapshotNotYetAssigned() const { return notYetAssigned; }
        void restoreNotYetAssigned(const set<string>& s) { notYetAssigned = s; }
        void mergeNotYetAssigned(const set<string>& other) {
            for (auto& n : other) notYetAssigned.insert(n);
        }
    };
}

