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

    /** A named binding: an alloca for native types and fixed arrays, a heap
     *  reference for classes. Subclasses supply the load, store and alloca. */
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
        // The drop entry may describe a DISPLACED value: compare before trusting.
        bool entryMayBeStale = false;
        bool stackInstance = false;
        // The borrow-returning call this local came from, if any: `#local` is
        // then a lie, since that call's source still owns and frees the value.
        string callBorrowOrigin;
        // The plain parameter a local was initialised from (straight-line
        // capture), for the same identity-not-name reason.
        string paramBorrowOrigin;
        std::vector<std::pair<int, string>> slotBorrowedLocals;
        // Seeded from an earlier unit of the same session: a type, but no alloca
        // in this unit.
        bool sessionSeeded = false;
        // Registered in the runtime session registry. Not derivable from the
        // drop entry: the owner path registers INSTEAD of pushing one.
        bool sessionBound = false;
        // A seeded binding whose class was REDEFINED. A flag of its own, since
        // the first generation's suffix is itself the empty string.
        bool staleGenerationMark = false;
        string staleGeneration;
        string currentGeneration;
        bool ownershipAudited = false;
        // For an owning String-element array local: the stack sidecar shared by
        // its element stores and its element-walk drop entry. Else nullptr.
        llvm::Value* elemOwnSidecar = nullptr;
        bool _hasBorrowCaptures = false;
        // For a struct view local, the same-scope field it aliases — null for a
        // view over a caller's buffer, and so the ReturnStatement check.
        FieldPtr _viewSource;
        // `View(#buf)` rather than `View(buf)`: the view took the buffer over,
        // and scope exit frees it instead of only dropping the alias.
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

        const string& getCallBorrowOrigin() const {
            return callBorrowOrigin;
        }

        void setCallBorrowOrigin(const string& origin) {
            callBorrowOrigin = origin;
        }

        const string& getParamBorrowOrigin() const {
            return paramBorrowOrigin;
        }

        void setParamBorrowOrigin(const string& origin) {
            paramBorrowOrigin = origin;
        }

        // spec 5.10 — slots of an ARRAY local that lend a frame local, so the array may not leave the frame (slot -1 = non-constant index).
        const std::vector<std::pair<int, string>>& getSlotBorrowedLocals() const {
            return slotBorrowedLocals;
        }
        void addSlotBorrowedLocal(int slot, const string& local) {
            slotBorrowedLocals.emplace_back(slot, local);
        }
        void copySlotBorrowedLocalsFrom(const Field& other) {
            slotBorrowedLocals = other.slotBorrowedLocals;
        }

        const string& getHierarchicalName() {
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

        // The drop-chain entry alloca, or null when this field owns nothing.
        llvm::Value* getDropEntry() const { return dropEntry; }
        void setDropEntry(llvm::Value* e) { dropEntry = e; }

        // Initialised by a `stack` construction. Storage class lives on the
        // construction, so the declaration site is the only one that knows.
        bool isStackInstance() const { return stackInstance; }
        void setStackInstance(bool v) { stackInstance = v; }

        bool isSessionSeeded() const { return sessionSeeded; }
        void setSessionSeeded(bool v) { sessionSeeded = v; }
        bool isSessionBound() const { return sessionBound; }
        void setSessionBound(bool v) { sessionBound = v; }
        // `stale` is the VALUE's generation, `current` the name's; they differ.
        void setStaleGeneration(const string& stale, const string& current) {
            staleGenerationMark = true;
            staleGeneration = stale;
            currentGeneration = current;
        }
        bool isStaleGeneration() const { return staleGenerationMark; }
        const string& getStaleGeneration() const { return staleGeneration; }
        const string& getCurrentGeneration() const { return currentGeneration; }
        // This drop entry is armed from a RUNTIME bit (transfer word, return
        // flag, forwarded slot bit), so a plain retaining store of it is loud.
        bool isRuntimeConditionalOwner() const { return runtimeConditionalOwner; }
        void setRuntimeConditionalOwner(bool v) { runtimeConditionalOwner = v; }
        bool isEntryMayBeStale() const { return entryMayBeStale; }
        void setEntryMayBeStale(bool v) { entryMayBeStale = v; }
        // The body read `Cajeta.owned(<this formal>)`, so its plain stores are
        // the author's own branch-guarded dual-store and stay quiet.
        bool isOwnershipAudited() const { return ownershipAudited; }
        void setOwnershipAudited(bool v) { ownershipAudited = v; }
        llvm::Value* getElemOwnSidecar() const { return elemOwnSidecar; }
        void setElemOwnSidecar(llvm::Value* s) { elemOwnSidecar = s; }

        // Holds a closure with borrow captures: scope-bound, so it cannot return.
        bool hasBorrowCaptures() const { return _hasBorrowCaptures; }
        void setHasBorrowCaptures(bool v) { _hasBorrowCaptures = v; }

        // The field whose buffer this struct view points into; returning a view
        // of a function-scope local leaves the caller a dangling one.
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