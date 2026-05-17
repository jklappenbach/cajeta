#pragma once

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <map>
#include <list>
#include <set>
#include "llvm/IR/Value.h"
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
        set<string> movedNames;
        // Field-access paths (e.g. "person.name" or "a.b.c") that have been
        // moved out. Read paths through DotExpression check this set with
        // prefix semantics — see DotExpression.cpp.
        set<string> movedPaths;
        // P3 — definite-assignment analysis (cajeta-docs/stdlib/UnifiedClasses.md § Definite
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
        // rejected with CAJETA_ERROR_USE_AFTER_MOVE — the borrower
        // would dangle the moment the source is mutated.
        //
        // Recorded on the borrower's scope; cleaned up when that
        // scope is destroyed, matching the borrower's lifetime.
        map<string, set<string>> liveBorrows;

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

        void setParent(ScopePtr parent) {
            this->parent = parent;
        }

        // Mark an identifier as moved-out. Subsequent reads of the same name in
        // this or any child scope will be rejected with a use-after-move error.
        // The mark is recorded on the scope that declares the field (so a move
        // inside a nested block still invalidates the outer binding); if the
        // field isn't found in any enclosing scope, the mark is recorded here
        // as a best-effort signal.
        void markMoved(const string& name);

        // True iff `name` has been moved-out in this scope or any ancestor.
        bool isMoved(const string& name);

        // Mark a field-access path as moved (e.g. "person.address.city").
        // Subsequent reads of the same path *or any path it's a prefix of*
        // are rejected. Single-identifier names should use markMoved instead.
        void markMovedPath(const string& path);

        // True iff `path` or any ancestor prefix of `path` is in the moved set
        // (in this scope or any ancestor). `"a.b.c"` is considered moved if
        // any of "a", "a.b", or "a.b.c" was marked.
        bool isPathMoved(const string& path);

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

