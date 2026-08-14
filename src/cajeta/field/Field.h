//
// Created by James Klappenbach on 2/20/22.
//

#pragma once

#include <set>
#include <list>
#include "../type/QualifiedName.h"
#include "../type/Modifiable.h"
#include "../type/Annotatable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include <llvm/IR/IRBuilder.h>
#include "../util/MemoryManager.h"

using namespace std;

namespace cajeta {
    class Field;
    typedef shared_ptr<Field> FieldPtr;

    class CajetaType;
    typedef shared_ptr<CajetaType> CajetaTypePtr;

    class Initializer;
    typedef shared_ptr<Initializer> InitializerPtr;

    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class Scope;
    typedef shared_ptr<Scope> ScopePtr;

    /**
     * Types of fields:
     * - Alloca variables, stack allocated.  This includes all native types and arrays.  Alloca variables are not assignable as a reference.
     * - Heap variables, allocated with new.  This is all classes, and also includes arrays.
     * - Arrays
     *  - Two types of arrays:
     *      - Alloca: int64[4] anArray; <-- This creates a type, int64[4]
     *      - Heap: int64[] aReference; <-- This creates a reference
     *
     *      Can references point at stack based arrays?  I don't see why not.  However the following rules would need to be enforced:
     *      - The dimension of the reference must match the stack alloca dimension
     *      - Only non-ownership references
     *
     */
    class Field : public Modifiable, public Annotatable {
    protected:
        CajetaModulePtr module;
        FieldPtr parent;
        bool reference;
        string name;
        string hierarchicalName;
        InitializerPtr initializer;
        cajeta::CajetaTypePtr type;
        llvm::AllocaInst* alloca;
        llvm::Value* dropEntry = nullptr;
        bool runtimeConditionalOwner = false;
        bool stackInstance = false;
        // script-units U4 — seeded into a script entry's root scope from the
        // SessionState table (a binding created by an earlier unit of the
        // same session). Carries a type but no alloca in this unit; moved
        // bindings reject reads (spec §4.2), live cross-unit reads land with
        // the kernel's read-through-session codegen.
        bool sessionSeeded = false;
        // script-units §4 — this declaration already registered the name in
        // the runtime session registry. Not derivable from getDropEntry():
        // the owner path registers INSTEAD of pushing a drop entry, so a
        // bound owner and a never-bound borrow both have a null entry.
        bool sessionBound = false;
        bool ownershipAudited = false;
        // slices 9.2.1 — for OWNING String-element array locals: the stack
        // sidecar shared by the element-store helpers and the element-walk
        // drop entry (LocalVariableDeclaration registers both). nullptr for
        // every other field; element stores fall back to the raw store.
        llvm::Value* elemOwnSidecar = nullptr;
        bool _hasBorrowCaptures = false;
        // For struct view-mode locals (`Header h = Header(bytes)`):
        // the field this view aliases. Set at LocalVariableDeclaration
        // time when the initializer is a struct-view-construction call;
        // consulted at ReturnStatement to reject returning a view of
        // a same-scope local buffer. nullptr for non-view structs and
        // for views over caller-provided buffers (parameters, fields).
        FieldPtr _viewSource;
        // For view-mode locals: distinguishes the borrow form (`View(buf)`,
        // false — default) from the owning form (`View(#buf)`, true). The
        // owning form transfers buffer ownership to the view; scope-exit
        // frees the underlying byte[] via __cajeta_view_drop_owned. The
        // borrow form leaves ownership with the source field and only
        // tracks the view-aliasing relationship via _viewSource above.
        bool _isOwningView = false;

    public:
        Field(CajetaModulePtr module, string name, CajetaTypePtr type, FieldPtr parent = nullptr) {
            this->module = module;
            this->name = name;
            this->type = type;
            this->parent = parent;
            this->reference = false;
            this->alloca = nullptr;
        }

        Field(CajetaModulePtr module, string name, llvm::AllocaInst* alloc);

        Field(CajetaModulePtr module, string name, CajetaTypePtr type, llvm::AllocaInst* alloca, FieldPtr parent = nullptr) {
            this->module = module;
            this->name = name;
            this->type = type;
            this->parent = parent;
            this->reference = false;
            this->alloca = alloca;
        }

        Field(CajetaModulePtr module, string name,
            CajetaTypePtr type,
            bool reference,
            set<Modifier> modifiers,
            set<QualifiedNamePtr> annotations,
            InitializerPtr initializer,
            FieldPtr parent = nullptr) : Modifiable(modifiers), Annotatable(annotations) {
            this->module = module;
            this->name = name;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->parent = parent;
            this->alloca = nullptr;
        }

        Field(CajetaModulePtr module,
            string name,
            CajetaTypePtr type,
            bool reference,
            InitializerPtr initializer,
            set<Modifier> modifiers,
            set<QualifiedNamePtr> annotations,
            FieldPtr parent = nullptr) : Modifiable(modifiers), Annotatable(annotations) {
            this->module = module;
            this->name = name;
            this->initializer = initializer;
            this->type = type;
            this->reference = reference;
            this->parent = parent;
            this->alloca = nullptr;
        }

        Field(CajetaModulePtr module, string& name, bool reference, FieldPtr parent = nullptr) {
            this->module = module;
            this->name = name;
            this->reference = reference;
            this->parent = parent;
            this->alloca = nullptr;
        }

        FieldPtr getParent() {
            return parent;
        }

        void setParent(FieldPtr parent) {
            this->parent = parent;
        }

        bool isReference() const {
            return reference;
        }

        const string& getName() const {
            return name;
        }

        const string& getHierarchicalName() {
            // Memoize once. The old form recomputed on every call and, on the
            // second call for a parented field, fell through to overwrite the
            // cached `Parent.name` with the bare `name` — returning the wrong
            // value on alternate reads.
            if (hierarchicalName.empty()) {
                hierarchicalName = parent
                    ? parent->buildHierarchicalName() + "." + name
                    : name;
            }
            return hierarchicalName;
        }

        CajetaTypePtr getType() const {
            return type;
        }

        void setAllocation(llvm::AllocaInst* alloca) {
            this->alloca = alloca;
        }

        // Drop-chain entry alloca for this owner, or null if this field isn't an
        // owner (e.g. primitive, borrow, parameter without `#`). Set by codegen
        // when the owner pushes onto the chain.
        llvm::Value* getDropEntry() const { return dropEntry; }
        void setDropEntry(llvm::Value* e) { dropEntry = e; }

        // True when this local was initialized by a `stack` construction
        // (`Cell c = stack Cell(...)`). Storage class lives on the
        // construction, not the type, so the declaration site is the only
        // place that knows — it records the fact here for later analyses.
        // Consumed by the `#`-return escape check
        // (stack-return-transfer-error-spec §2.1).
        bool isStackInstance() const { return stackInstance; }
        void setStackInstance(bool v) { stackInstance = v; }

        bool isSessionSeeded() const { return sessionSeeded; }
        void setSessionSeeded(bool v) { sessionSeeded = v; }
        bool isSessionBound() const { return sessionBound; }
        void setSessionBound(bool v) { sessionBound = v; }
        // title-stores 6.2.1 — this local/formal's drop entry is armed from a
        // RUNTIME bit (transfer word, return flag, or forwarded slot bit), so
        // a plain retaining store of it is the loud-plain-store hazard.
        bool isRuntimeConditionalOwner() const { return runtimeConditionalOwner; }
        void setRuntimeConditionalOwner(bool v) { runtimeConditionalOwner = v; }
        // 6.2.1 exclusion — the body read `Cajeta.owned(<this formal>)`, so
        // its plain stores are branch-guarded by the author (the container
        // dual-store idiom, spec §3.3.1); the loud-plain-store stays quiet.
        bool isOwnershipAudited() const { return ownershipAudited; }
        void setOwnershipAudited(bool v) { ownershipAudited = v; }
        llvm::Value* getElemOwnSidecar() const { return elemOwnSidecar; }
        void setElemOwnSidecar(llvm::Value* s) { elemOwnSidecar = s; }

        // L3-2: set on function-typed fields that hold a closure with
        // borrow captures. Such a closure is scope-bound — returning it
        // or storing it past the declaring scope would leave the borrows
        // dangling. ReturnStatement consults this to reject the return.
        // Defaults false; LocalVariableDeclaration sets it after the
        // initializer codegen finds the RHS lambda's borrow flag.
        bool hasBorrowCaptures() const { return _hasBorrowCaptures; }
        void setHasBorrowCaptures(bool v) { _hasBorrowCaptures = v; }

        // Struct view-aliasing: the field whose backing buffer this
        // struct view points into. Set when the local is constructed
        // via `Header h = Header(bytes)`; ReturnStatement consults it
        // to reject returns where the source is a function-scope
        // local (the buffer would drop and leave the caller with a
        // dangling view).
        FieldPtr getViewSource() const { return _viewSource; }
        void setViewSource(FieldPtr s) { _viewSource = std::move(s); }

        bool isOwningView() const { return _isOwningView; }
        void setIsOwningView(bool v) { _isOwningView = v; }

        virtual llvm::Value* createLoad() = 0;

        virtual llvm::Value* createStore(llvm::Value* value) = 0;

        virtual llvm::AllocaInst* getOrCreateAllocation() = 0;

        virtual void onDelete() { };

        static list<FieldPtr> fromContext(CajetaParser::FieldDeclarationContext* ctx, CajetaModulePtr module);
    protected:
        virtual string buildHierarchicalName() {
            if (parent) {
                return parent->buildHierarchicalName() + string(".") + name;
            }
            return name;
        }
    };

    typedef shared_ptr<Field> FieldPtr;
}