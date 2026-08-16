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


    /**
     * Supported scopes:
     *
     * Given that we have a single class per pModule, we have the following layers of scope:
     *
     * 1. Class Static
     * 2. Contained Class Static
     *      2a. Contained Class Instance
     *          2b. Contained Class Method
     * 3. Class Instance
     * 4. Class Method
     */
    class Scope {
    protected:
        string name;
        CajetaModulePtr module;
        ScopePtr parent;
        map<string, FieldPtr> fields;
        list<FieldPtr> fieldList;
        map<llvm::AllocaInst*, FieldPtr> allocaToField;
        // Names of identifiers in this scope (or some enclosing scope) that have
        // been moved out via `#`. Read paths consult this set and reject
        // accesses for moved identifiers — see Identifier.cpp.
        set<string> borrowedBindings;
        // Optional per-name transfer-site notes for borrowedBindings, appended to
        // use-after-move diagnostics. Lives/clears with the name's entry.
        map<string, string> transferSites;
        // Ordered log of moves recorded through THIS scope (target scope may
        // be an ancestor). Blocks checkpoint it and retract their slice when
        // their codegen ends in a return/throw terminator — a path that never
        // reaches the join contributes no moved state to it (title-tracking
        // §3.1.5). Entries are appended only for genuinely NEW marks, so a
        // retraction exactly undoes its slice.
        vector<pair<Scope*, string>> moveLog;
        // Field-access paths (e.g. "person.name" or "a.b.c") that have been
        // moved out. Read paths through DotExpression check this set with
        // prefix semantics — see DotExpression.cpp.
        set<string> borrowedPaths;
        // P3 — definite-assignment analysis (docs/specification/lang/UnifiedClasses.md § Definite
        // assignment). Names that were declared in this scope without an
        // initializer and have not yet been assigned. Read paths check this
        // set and reject. Variables with initializers, parameters, and
        // fields owned by an enclosing class are never NYA. The set
        // shrinks as assignments fire; once a name is removed, it stays
        // removed (sequential codegen).
        set<string> notYetAssigned;

        // Gap 4 (MemoryModel.md § Known gaps) — live read-borrows.
        // Maps a borrowed path (e.g. "p.name", or just "p") to the set
        // of local names borrowing from it (so disposing borrowers
        // doesn't blank out unrelated borrows of the same source).
        //
        // A borrow is recorded by LocalVariableDeclaration when the
        // initializer is a borrow-shaped read (field-read or local
        // alias of a class instance, see initIsBorrow). The
        // assignment site in BinaryOpExpression checks before
        // writing: if the target path overlaps any live borrow's
        // path (either is a prefix of the other), the write is
        // rejected with CAJETA_ERROR_MOVE_OF_BORROW — the borrower
        // would dangle the moment the source is mutated.
        //
        // Recorded on the borrower's scope; cleaned up when that
        // scope is destroyed, matching the borrower's lifetime.
        map<string, set<string>> liveBorrows;

        // CajetaXPU §3.5 / §11 — launch borrow scope. A `kernel.launch(...)`
        // borrows each Buffer argument for the duration of the (asynchronous)
        // launch; the borrow is released at the next `Stream.sync()` /
        // `Event.waitHost()`. Unlike liveBorrows (released at scope exit), a
        // launch borrow is released at an explicit sync point — so it's
        // tracked separately. Names here are buffer locals with an in-flight
        // launch outstanding. Freeing / reassigning such a buffer before a
        // sync is a compile error (a use-after-free of memory a running kernel
        // still references).
        set<string> launchBorrows;

        // title-tracking 5.2.7 (spec §7.4, sub-fork B) — single-hop dangling
        // lend. Maps a holder local to the LOCAL owners it holds a LEND of
        // (a plain, non-`#` store/arg: `h.c = s`, `h.keep(s)`). If the holder
        // later escapes the method, those sources die at scope exit and the
        // escapee carries dangling pointers — rejected with
        // CAJETA_ERROR_DANGLING_LEND. Recorded at the holder's declaring
        // scope, like demoteToBorrow. Conservative: an extraction does NOT clear
        // the edge (no intra-procedural entry-level tracking); the `#s`
        // spelling at the lend is the suppression.
        map<string, set<string>> lendEdges;

        // stdlib-ownership-convention U2 — call-result provenance. Maps a
        // local to the call it was initialised from when that callee's BODY
        // PROVES the result is an interior view ("o.keyAt(j)": every return a
        // `this.field` read, and at least one — Method::returnsInteriorView).
        // Such a local holds no title, so `#local` would surrender one it
        // does not have and mint a second owner — the JsonObject.keyAt bug
        // that produced garbage keys in cajeta-llama U13.
        //
        // NOT keyed on the declared return spelling. A plain (non-`#`) return
        // is not statically a borrow: it carries a RUNTIME flag, so a
        // plain-return wrapper rides an inner `#` call's title through
        // (spec §1.2, §4.1). The proven-view shape is the narrower question
        // that IS decidable; anything unproven is allowed (§7.2), so the
        // check never blocks valid code.
        map<string, string> callBorrowOrigins;

        void putField(FieldPtr field, string propertyPath);

    public:
        Scope(string name, CajetaModulePtr module, ScopePtr parent = nullptr);

        ~Scope();

        bool containsField(string fieldName);

        /**
         * Add a field to this scope
         *
         * @param field
         */
        void putField(FieldPtr field);

        /**
         * Get a field by field name, useful for resolving identifiers
         *
         * @param fieldName
         * @return A FieldPtr result, or throws an exception
         */
        FieldPtr getField(string fieldName);

        FieldPtr getField(llvm::AllocaInst* alloca);

        // Block-scoped name bindings. There is one Scope per method, so a
        // local declared inside a nested `{ ... }` lands in the same map as
        // the method's parameters and would otherwise clobber a same-named
        // parameter or outer local for the REST of the method — including
        // after the closing brace, where the name must resolve to the outer
        // binding again. Block::generateCode snapshots the prior binding of
        // every name a block directly declares and puts it back at the `}`.
        //
        // localBinding looks only at THIS scope (never the parent chain) and
        // never inserts: getField's `fields[name]` default-constructs a null
        // entry on a miss, so a plain lookup could not tell "unbound" from
        // "bound to null" and would spuriously report a shadow. A null return
        // means nothing was shadowed and the block leaves the binding alone.
        FieldPtr localBinding(const string& fieldName);

        void restoreBinding(const string& fieldName, FieldPtr prior);

        void setParent(ScopePtr parent) {
            this->parent = parent;
        }

        // Mark an identifier as moved-out. Subsequent reads of the same name in
        // this or any child scope will be rejected with a use-after-move error.
        // The mark is recorded on the scope that declares the field (so a move
        // inside a nested block still invalidates the outer binding); if the
        // field isn't found in any enclosing scope, the mark is recorded here
        // as a best-effort signal.
        void demoteToBorrow(const string& name);

        // As demoteToBorrow, additionally recording a note (the transfer site)
        // that use-after-move diagnostics append.
        void demoteToBorrow(const string& name, const string& note);

        // Re-arm: a fresh assignment to a moved-out local clears its moved
        // state (title-tracking §3.1.4). Erases on the declaring scope,
        // mirroring demoteToBorrow's placement.
        void restoreOwnership(const string& name);

        // The note recorded with the move of `name`, or "".
        string transferSiteOf(const string& name);

        // Terminated-path retraction (see moveLog). Blocks capture the size
        // before their children and retract the slice when they end in a
        // return/throw.
        size_t moveLogSize() const { return moveLog.size(); }
        void retractMovesSince(size_t mark);

        // Branch-arm move state (if/else): snapshot the marks an arm
        // introduced (with their notes), restore the pre-branch state for
        // the sibling arm, and reapply at the join for the conservative
        // union — mirrors the definite-assignment snapshot/merge.
        struct MoveMark { Scope* target; string name; string note; };
        vector<MoveMark> snapshotMovesSince(size_t mark) const;
        void reapplyMoves(const vector<MoveMark>& moves);

        // U2 — record that `name` was initialised from a call whose BODY
        // PROVES an interior view: every return a `this.field` read (or an
        // index into one), and at least one — see Method::returnsInteriorView.
        // The criterion is the BODY, not the return spelling: a plain
        // (non-`#`) return is not statically a borrow (spec §1.2, §4.1), so
        // anything unproven stays unrecorded and is allowed (§7.2).
        void recordCallBorrow(const string& name, const string& origin);
        // The proven-view call `name` came from, or "".
        string callBorrowOriginOf(const string& name);

        // U2 (plan 2.2.3) — the transfer-of-a-borrow rejection, shared by
        // EVERY site that can spell `#x` on a named local. Throws
        // CAJETA_ERROR_MOVE_OF_BORROW when `name` holds a borrow; returns
        // quietly otherwise.
        //
        // Factored out because the two spellings of `#x` take different
        // routes through the compiler and had silently diverged: an
        // assignment/return builds a MoveExpression node (whose
        // generateCode held all three checks), while a CALL ARGUMENT is a
        // parse-level `callerTransferred` flag on MethodCallParameter with
        // a BARE IDENTIFIER child — no MoveExpression is ever constructed,
        // so arguments reached none of the checks. Argument position is
        // where the cajeta-llama corruption actually lived
        // (`heap String(#kb, kl)`), i.e. the blind spot covered the
        // motivating case. One body, three call sites, no drift.
        void rejectTransferOfBorrow(const string& name);

        // U3 (spec 2.4, 3.2, 4.2) — the CALLEE-side mirror of the above.
        // Throws CAJETA_ERROR_CAPTURED_BORROW_PARAM when `srcName` is a
        // plain (non-`#`) parameter being stored beyond the call, naming
        // `#T` as the fix. `intoDesc` describes the destination for the
        // diagnostic ("field `held`", "an array element").
        //
        // Callers gate on the STORE FORM: only a plain `=` store reaches
        // here. A `#=` store is the sink contract (§2.3, ArrayList) and is
        // always legal, which is why the opt-out needs no annotation.
        //
        // `sourceLine` locates the store for the warn-mode record below; it is
        // not used by the error path, which carries its location the way every
        // other thrown diagnostic does.
        void rejectCapturedBorrowParam(const string& srcName,
                                       const string& intoDesc,
                                       int sourceLine = -1);

        // 3.3.3 — the warning-first migration switch, prescribed by spec §3.4.
        //
        // Error-first cost Unit 3 its gate: the check landed as a throw, the
        // audit's static pass had classified 10 sites, and the gate then found
        // 76 failures the audit never saw. A throw stops the build at the
        // FIRST site, so enumerating the rest costs one ~90s compile each with
        // no way to see the total. In warn mode the check reports and lets
        // codegen continue, so one build per library enumerates every site.
        //
        // Default is ERROR. The env var `CAJETA_CAPTURED_BORROW=warn` demotes
        // it; `setCapturedBorrowWarns` overrides the env for tests. Read per
        // call rather than cached, so a test that flips it mid-process is not
        // at the mercy of which test ran first.
        static bool capturedBorrowWarns();
        static void setCapturedBorrowWarns(bool on);
        static void clearCapturedBorrowWarnsOverride();

        // 5.2.7 — record `holder` now holds a lend of the local owner `src`.
        void recordLend(const string& holder, const string& src);
        // The local owners `holder` holds lends of (empty when none).
        set<string> lendsOf(const string& holder);

        // The source path `name` borrows from (liveBorrows inverted, walked
        // across ancestors), or "" when `name` is not a recorded borrower.
        string borrowSourceOf(const string& name);

        // True iff `name` has been moved-out in this scope or any ancestor.
        bool isBorrow(const string& name);

        // Mark a field-access path as moved (e.g. "person.address.city").
        // Subsequent reads of the same path *or any path it's a prefix of*
        // are rejected. Single-identifier names should use demoteToBorrow instead.
        void demotePathToBorrow(const string& path);

        // True iff `path` or any ancestor prefix of `path` is in the moved set
        // (in this scope or any ancestor). `"a.b.c"` is considered moved if
        // any of "a", "a.b", or "a.b.c" was marked.
        bool isPathBorrow(const string& path);

        // P3 — mark a name as declared-but-not-yet-assigned. Called by
        // LocalVariableDeclaration when a local is declared without an
        // initializer (`MyClass x;`). The mark is recorded on this
        // scope; subsequent reads in this or any child scope must see
        // an assignment first.
        void markNotYetAssigned(const string& name);

        // P3 — mark a name as definitely assigned (removes any NYA mark
        // for that name in the declaring scope). Called from the
        // assignment-codegen path (BinaryOpExpression's `=` branch on
        // an identifier LHS, LocalVariableDeclaration when the local
        // has an initializer, etc.).
        void markAssigned(const string& name);

        // P3 — true iff `name` was declared without an initializer in
        // this or any ancestor scope and has not been assigned since.
        // Reading such a name is a compile error (the catch-all read
        // path in IdentifierExpression consults this).
        bool isNotYetAssigned(const string& name);

        // Gap 4 — record a live read-borrow on this scope. `borrower`
        // is the local that holds the borrow (e.g. "alias");
        // `borrowedPath` is the dotted source path (e.g. "p.name", or
        // just "p" if borrowing a whole local). Called from
        // LocalVariableDeclaration when initIsBorrow detects a
        // field-read or local-alias initializer.
        void recordLiveBorrow(const string& borrower, const string& borrowedPath);

        // Gap 4 — check whether writing to `writePath` would
        // invalidate any live borrow in this scope or any ancestor.
        // A write to W invalidates a borrow B if either path is a
        // prefix of the other (writing through a borrowed structure
        // mutates what it points at; writing through an ancestor
        // path clobbers the borrowed slot). Returns the offending
        // borrow's path on the first match, or empty string when
        // the write is safe.
        string findInvalidatingBorrow(const string& writePath);

        // CajetaXPU launch borrow scope (§3.5 / §11).
        // recordLaunchBorrow: mark `bufferName` as borrowed by an in-flight
        //   launch (recorded on this scope).
        // isLaunchBorrowed: true iff `bufferName` has an outstanding launch
        //   borrow in this scope or any ancestor.
        // releaseLaunchBorrows: clear all launch borrows in this scope and its
        //   ancestors — called at a Stream.sync() / Event.waitHost() point
        //   (single-stream model in v1; per-stream tracking is a later cut).
        // pendingLaunchBorrows: the still-borrowed names in this scope (for the
        //   "freed/dropped before sync" diagnostic).
        void recordLaunchBorrow(const string& bufferName);
        bool isLaunchBorrowed(const string& bufferName);
        void releaseLaunchBorrows();
        const set<string>& pendingLaunchBorrows() const { return launchBorrows; }

        // P3b — flow-analysis snapshot helpers. IfStatement (and other
        // branching control-flow nodes) save the NYA set before each
        // branch, run the branch, save the result, then merge across
        // branches to produce the post-control-flow NYA set.
        //
        // mergeNYA(other) sets this scope's NYA to the union of its
        // current set and `other`. Semantically: a variable is NYA
        // after the join iff it was NYA in EITHER branch (DA after
        // iff DA in BOTH).
        set<string> snapshotNotYetAssigned() const { return notYetAssigned; }
        void restoreNotYetAssigned(const set<string>& s) { notYetAssigned = s; }
        void mergeNotYetAssigned(const set<string>& other) {
            for (auto& n : other) notYetAssigned.insert(n);
        }
    };
}

