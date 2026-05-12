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
    };
}

