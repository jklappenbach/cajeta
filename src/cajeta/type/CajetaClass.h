//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "CajetaType.h"
#include "StructureProperty.h"
#include "../method/Method.h"
#include "Scope.h"
#include "Templates.h"

#include <vector>

namespace cajeta {
    class CajetaInterface;
    typedef shared_ptr<CajetaInterface> CajetaInterfacePtr;

    class ClassBodyDeclaration;
    typedef shared_ptr<ClassBodyDeclaration> ClassBodyDeclarationPtr;

    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaClass;
    typedef shared_ptr<CajetaClass> CajetaClassPtr;

    // Discriminator written into word 2 of an interface fat pointer.
    // Selected at the assignment site that builds the fat pointer
    // (class→iface, struct→iface, #class→iface) and consumed by
    // the kind-tag drop dispatch (S10.4).
    enum InterfaceValueKind : int64_t {
        IFACE_KIND_BORROWED_CLASS = 0,
        IFACE_KIND_OWNED_CLASS = 1,
        IFACE_KIND_BORROWED_STRUCT = 2
    };

    // Size of an interface fat-pointer value in bytes. Three machine
    // words: data_ptr (8) + vtable_ptr (8) + kind_tag (8) = 24.
    // Centralized here so layout / parameter passing / DI synthesis
    // all agree on the size.
    constexpr unsigned IFACE_FAT_POINTER_BYTES = 24;

    class CajetaClass : public CajetaType {
    protected:
        // Methods maintains the methods declared / overridden in this particular method
        map<string, MethodPtr> methods;
        map<string, MethodPtr> staticMethods;

        // Constructors
        map<string, map<string, MethodPtr>> labeledConstructorMap;
        map<string, map<string, MethodPtr>> unlabeledConstructorMap;

        // Virtual Methods
        map<string, map<string, MethodPtr>> labeledMethodMap;
        map<string, map<string, MethodPtr>> unlabeledMethodMap;

        list<MethodPtr> virtualMethodList;
        // Hash for each virtualMethodList entry, in lockstep order. For
        // ordinary methods the hash matches signatureHash(method->toCanonical()).
        // For interface entries, the hash is the *interface* method's canonical
        // (not the concrete-implementing method's), so a receiver typed as the
        // interface dispatches to the right slot. Populated by buildVirtualTable;
        // consumed by StructureMetadata::createVirtualTableConstant.
        vector<int64_t> virtualSlotHashList;
        list<MethodPtr> methodList;
        map<string, StructurePropertyPtr> properties;
        list<StructurePropertyPtr> propertyList;
        list<QualifiedNamePtr> qExtended;
        list<QualifiedNamePtr> qImplemented;

        list<CajetaClassPtr> superClasses;
        list<CajetaInterfacePtr> interfaces;
        // Concrete CajetaClass pointers for interfaces this class implements
        // (CajetaInterface is just a CajetaClass with isInterface()=true; we
        // store as CajetaClassPtr so buildVirtualTable can treat them uniformly
        // with the supertype walk).
        list<CajetaClassPtr> implementedInterfaces;
        // Interfaces have no instance fields and their methods are abstract
        // markers — `new MyInterface()` is invalid and their methods have
        // no LLVM function. The flag toggles those behaviors on the same
        // CajetaClass that the visitor builds for `interface X { ... }`.
        bool interfaceFlag = false;
        // Forward-reference placeholder marker. CajetaType::fromContext
        // sets this when it creates a CajetaClass for a name that's
        // known-to-the-archive but hasn't been declared yet by the
        // visitor walk. visitClassDeclaration finds the placeholder,
        // fills in its real shape (qName, modifiers, super/implements,
        // methods, properties), and clears the flag. Any placeholder
        // still set after all parsing is a leak — flagged by the
        // post-parse pass.
        bool placeholderFlag = false;
        // True once generatePrototype has run successfully. The sweep in
        // CajetaModule::buildPendingPrototypes uses this as the
        // fixed-point marker: a class with all-non-placeholder parents
        // and !prototypeBuilt is the next candidate to lay out.
        bool prototypeBuilt = false;
        CajetaModulePtr module;
        ScopePtr scope;

        // Templates. `typeParameters` non-empty AND `typeArguments` empty =
        // an unmaterialized template (don't run generatePrototype on it; it
        // isn't a real type). Both non-empty = a concrete instantiation
        // (`Box<int32>`), a real type that codegens normally. Both empty =
        // an ordinary non-templated class.
        //
        // `templateSource` holds the raw text of the class declaration —
        // captured during the visit pass while the ANTLR CharStream is still
        // live. We deliberately don't retain parse-tree pointers: ANTLR's
        // context nodes carry parent links up to the compilation unit, so
        // pinning one class transitively pins the entire file's tree. The
        // text is small, re-parsing on demand is cheap, and the result is
        // cached after first instantiation (see CajetaClass::instantiate).
        vector<TypeParameter> typeParameters;
        vector<CajetaTypePtr> typeArguments;
        string templateSource;
        // Back-pointer from a concrete instantiation to the template class it
        // came from. Null for templates and for plain (non-templated) classes.
        // Used by inferDiamondArgs to recognize that `List<int32>` is "a List"
        // when unifying against a `List<T>` parameter declaration.
        CajetaClassPtr templateOrigin;

        // Default to nullptr so writeVirtualTable's "already built?" guard
        // works on fresh instances. Without explicit initialization these
        // pointers held indeterminate values, which for newly-allocated
        // template instantiations could look non-null and silently skip
        // vtable construction — and the resulting garbage pointer crashed
        // the next instance-construction codegen.
        llvm::StructType* llvmVirtualTableType = nullptr;
        llvm::GlobalVariable* llvmVirtualTableGlobal = nullptr;
        llvm::StructType* llvmRttiType = nullptr;
        llvm::StructType* llvmReferenceType = nullptr;
        llvm::GlobalVariable* llvmRttiGlobal = nullptr;
        // Synthesized drop wrapper for this class — see getOrCreateDropFunction.
        // Cached on first request so LocalVariableDeclaration's drop-entry
        // registration is a constant-time lookup per declaration site.
        llvm::Function* llvmDropFunction = nullptr;
        // P7.1 — synthesized stack-drop wrapper. Same shape as the heap
        // drop above but does NOT free the body; intended for stack-
        // allocated class locals whose body is reclaimed by the function
        // epilogue. Walks owned class-ref fields (matching CajetaStruct's
        // existing auto-walk behavior) so embedded ownership doesn't leak.
        llvm::Function* llvmStackDropFunction = nullptr;
        // Gap 1 (virtual dispatch on drop) — set true after the vtable
        // global's drop_fn slot has been patched to point at
        // llvmDropFunction. Keeps patchVirtualTableDropFn idempotent so
        // multiple heap-class local sites all see a no-op after the
        // first patch.
        bool llvmDropFunctionPatched = false;

        // S9.5.2 — per-(class, interface) vtable globals. Sibling of
        // CajetaStruct::interfaceVTables. Keyed by interface canonical
        // name; value is a `[N x ptr]` constant whose entries point at
        // this class's concrete implementations in interface-declaration
        // order. The fat-pointer dispatch model uses these as the
        // vtable_ptr word of the interface value, regardless of whether
        // the implementer is a class or a struct.
        std::map<std::string, llvm::GlobalVariable*> interfaceVTables;

        // Static class fields — one llvm::GlobalVariable per static
        // property, lazily defined in this class's home module on
        // first use. Keyed by property name. Cross-module references
        // route through `ensureGlobalInModule` so callers in a
        // different module see an extern decl resolved at JIT link
        // time. See `getOrCreateStaticFieldGlobal`.
        std::map<std::string, llvm::GlobalVariable*> staticFieldGlobals;

        // Sub-object slot-start map — for every ancestor class (including
        // self), the LLVM struct slot index where that ancestor's
        // sub-object begins inside THIS class's layout. Populated by
        // generatePrototype. Used by getSubObjectByteOffset to compute
        // the byte adjustment that `this` needs at parent-method call
        // sites under the per-parent sub-object layout (Gap 8 fix).
        //
        //   subObjectSlotMap[self] = 0
        //   subObjectSlotMap[firstParent] = 0 (shares primary vtable)
        //   subObjectSlotMap[secondParent] = slot where its own vtable sits
        //
        // Keyed by raw pointer because CajetaClass holds shared_ptrs to
        // its ancestors and we just need identity comparison.
        std::map<const CajetaClass*, int> subObjectSlotMap;

        // Secondary-vtable cache. One entry per non-first-parent ancestor
        // for which a secondary vtable has been materialized. Keyed by
        // parent's canonical name (since CajetaClassPtr identity may
        // shift after a template instantiation refresh).
        std::map<std::string, llvm::GlobalVariable*> secondaryVTables;

        // MultiClassing Phase 3 v4 vbase ABI (cajeta-docs/stdlib/
        // MultiClassing.md § Phase 3): for every transitive non-self
        // ancestor of this class, the layout reserves a `ptr` slot at
        // the END of own-fields (after all sub-objects + own properties).
        // The pointer is initialized in the ctor to point at this
        // ancestor's inline sub-object position; in a diamond descendant,
        // the descendant's auto-ctor body overwrites non-first parents'
        // vbase slots to point at the FIRST (canonical) position so all
        // paths to a shared ancestor agree on storage.
        //
        // DotExpression loads through this pointer when accessing an
        // inherited field: `field_ptr = (load vbase_to_X) + offsetInX`.
        // Own-field access stays a direct GEP.
        //
        // Keyed by raw CajetaClass*; order preserved by vbaseAncestors
        // (for stable iteration in ctor init + slot assignment).
        std::vector<CajetaClassPtr> vbaseAncestors;
        std::map<const CajetaClass*, int> vbaseSlotMap;

        MethodPtr getClosestMethod(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical);
        MethodPtr getClosestConstructor(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical);

    public:
        CajetaClass(CajetaModulePtr module) {
            this->module = module;
            scope = nullptr;
        }
        CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented);

        CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented);

        /**
         * Create a structure that provides for a boolean type for whether the reference owns the instance and should delete at scope-end,
         * as well as a pointer to the struct, which was allocated from the heap.
         * @return The type defining the reference.
         */
        llvm::Type* getLlvmReferenceType();

        bool isParentOrKind(CajetaClassPtr source);

        bool isInterface() const { return interfaceFlag; }
        void setIsInterface(bool v) { interfaceFlag = v; }
        bool isPlaceholder() const { return placeholderFlag; }
        void setPlaceholder(bool v) { placeholderFlag = v; }

        bool isPrototypeBuilt() const { return prototypeBuilt; }

        // Run generatePrototype if every superclass / implemented interface
        // has been filled in (i.e. is no longer a placeholder). When a
        // parent is still a placeholder, defer — the post-parse sweep in
        // CajetaModule::buildPendingPrototypes runs again after each fill-in
        // and will pick this class up once its parents are ready.
        // Returns true if generatePrototype ran (or was already built),
        // false if deferred. Idempotent.
        bool tryGeneratePrototype();

        // Class instances flow by pointer in cajeta — field slots
        // of class type store the pointer to the instance, not the
        // instance struct inline. While a forward-reference
        // placeholder is unfilled, getLlvmType() returns `ptr` so
        // earlier-parsed classes composing their layouts against
        // this type get a sized slot. Once the real declaration
        // arrives, generatePrototype establishes the named
        // StructType and llvmType is non-null on subsequent calls,
        // so this override falls through to the base. Definition
        // lives in CajetaClass.cpp because the `ptr` fallback needs
        // module->getLlvmContext() (CajetaModule isn't complete
        // at this header's include level).
        llvm::Type* getLlvmType() override;

        // Fill an existing placeholder with the real declaration's
        // shape. visitClassDeclaration calls this when it finds a
        // pre-existing placeholder in canonicalMap whose canonical
        // matches the class being declared. All existing references
        // to the placeholder (held by fields/methods of earlier-
        // parsed classes) become valid for the real class because
        // it's the same shared_ptr instance.
        void fillFromDeclaration(CajetaModulePtr m,
                                  QualifiedNamePtr q,
                                  list<QualifiedNamePtr> ext,
                                  list<QualifiedNamePtr> impl) {
            this->module = m;
            this->qName = q;
            this->qExtended = std::move(ext);
            this->qImplemented = std::move(impl);
            this->placeholderFlag = false;
        }
        list<CajetaClassPtr>& getImplementedInterfaces() { return implementedInterfaces; }
        const list<QualifiedNamePtr>& getQImplemented() const { return qImplemented; }
        void setQImplemented(list<QualifiedNamePtr> q) { qImplemented = std::move(q); }
        void resolveImplementedInterfaces();

        ScopePtr getScope() { return scope; }

        void addMethod(MethodPtr method);

        void addMethods(list<MethodPtr> methods);

        void addProperty(StructurePropertyPtr field);

        map<string, map<string, MethodPtr>>& getUnlabeledMethodMap() {
            return unlabeledMethodMap;
        }

        list<MethodPtr>& getVirtualMethodList() {
            return virtualMethodList;
        }

        const vector<int64_t>& getVirtualSlotHashList() const {
            return virtualSlotHashList;
        }

        map<string, StructurePropertyPtr>& getProperties() { return properties; }

        list<StructurePropertyPtr>& getPropertyList() { return propertyList; }

        map<string, MethodPtr>& getMethods() { return methods; }

        list<MethodPtr>& getMethodList() { return methodList; }

        CajetaModulePtr getModule() { return module; }

        list<CajetaClassPtr>& getSuperClasses() { return superClasses; }

        // LLVM struct index for a class field. Class instances reserve LLVM
        // slot 0 for the vtable pointer, so user properties live at LLVM
        // indices 1..N even though `StructureProperty::getOrder()` is
        // 0-based. `CajetaStruct` overrides this to return the order
        // verbatim (no vtable slot).
        // Count how many fields this class inherits from all ancestors. The
        // subclass's LLVM struct layout is { vtable, <inherited>, <own> },
        // so inherited fields occupy [1 .. 1+inherited_count) and own
        // fields start at [1+inherited_count, ...). Recursive — each
        // parent contributes its own inherited fields plus its own.
        int countInheritedFields() const {
            int count = 0;
            for (auto& parent : superClasses) {
                int ownNonStatic = 0;
                for (auto& p : parent->propertyList) {
                    if (!p->isStatic()) ownNonStatic++;
                }
                count += parent->countInheritedFields() + ownNonStatic;
            }
            return count;
        }

        // Map a StructureProperty to its slot index in this class's LLVM
        // struct. Walks the layout in the SAME order as the layout builder
        // in CajetaClass::generatePrototype (per-parent sub-object layout
        // — Gap 8 fix). For each class we visit:
        //   1. If it owns a vtable slot here (always for self / non-first
        //      parents; never for a first-parent share), advance past it.
        //   2. Recurse into its parents — the first parent shares this
        //      sub-object's leading vtable, subsequent parents get their
        //      own.
        //   3. Append its own non-static fields.
        // This mirrors the layout exactly so a field's slot index lines
        // up with where setBody put it.
        // MultiClassing Phase 3 v4 vbase accessors.
        const std::vector<CajetaClassPtr>& getVbaseAncestors() const {
            return vbaseAncestors;
        }
        // Returns -1 when the class isn't in the vbase map (e.g. self,
        // unrelated class, or a class whose prototype hasn't been built).
        int getVbaseSlotIndex(const CajetaClass* ancestor) const {
            if (!ancestor) return -1;
            auto it = vbaseSlotMap.find(ancestor);
            if (it == vbaseSlotMap.end()) return -1;
            return it->second;
        }

        virtual int getFieldLlvmIndex(const StructurePropertyPtr& prop) const {
            // Static properties have no slot in the instance struct —
            // they live in dedicated globals. Return -1 so a caller
            // that mistakenly GEPs by index gets a quick failure
            // rather than indexing into someone else's slot.
            if (prop && prop->isStatic()) return -1;
            int slot = 0;
            int result = -1;
            std::function<void(const CajetaClass*, bool)> walk =
                [&](const CajetaClass* cls, bool ownVtable) {
                    if (result >= 0) return;
                    if (ownVtable) slot++;  // claim sub-object vtable slot
                    int idx = 0;
                    for (const auto& parent : cls->superClasses) {
                        walk(parent.get(), /*ownVtable=*/(idx != 0));
                        if (result >= 0) return;
                        idx++;
                    }
                    for (const auto& p : cls->propertyList) {
                        if (result >= 0) return;
                        if (p->isStatic()) continue;
                        if (p.get() == prop.get()) { result = slot; return; }
                        slot++;
                    }
                    // MultiClassing Phase 3 v4: skip cls's vbase pointer
                    // slots (appended after own properties in the layout)
                    // so subsequent sub-objects' indices align with the
                    // embedSubObject walker.
                    if (result < 0) {
                        slot += (int) cls->vbaseAncestors.size();
                    }
                };
            walk(this, /*ownVtable=*/true);
            return result;
        }

        // Byte offset of an ancestor's sub-object inside this class's
        // instance layout. Returns 0 for self and the first-parent chain
        // (they share this class's primary vtable). Returns the byte
        // offset of the sub-object's vtable slot for any non-first-parent
        // chain.
        //
        // Computed from subObjectSlotMap + the LLVM DataLayout. Used at
        // parent-method/ctor call sites to adjust `this` so the parent's
        // pre-compiled IR (which uses the parent's own struct indices)
        // lands on the correct sub-object inside the subclass instance.
        uint64_t getSubObjectByteOffset(const CajetaClass* ancestor) const;

        // List of (ancestor, slot, byteOffset) for every non-first-parent
        // sub-object in this class's layout. Used by ClassCreatorRest to
        // initialize each secondary vptr slot at `new` time and by the
        // secondary-vtable builder to enumerate which (this-class, parent)
        // pairs need a synthesized vtable.
        struct NonFirstSubObject {
            CajetaClassPtr ancestor;
            int slot;
            uint64_t byteOffset;
        };
        std::vector<NonFirstSubObject> getNonFirstSubObjects();

        // Secondary vtable for the (this class, parent) pair — a vtable
        // structurally compatible with `parent`'s standalone vtable
        // (same hash entries / sort order) but whose function pointers
        // are this class's most-derived implementations. When the impl
        // lives on a different class than `parent` (i.e., a cross-class
        // override), the entry points at a synthesized thunk that
        // adjusts `this` back to this class's primary pointer before
        // tail-calling the override.
        //
        // Cached per (this-class, parent) — idempotent. The global
        // lives in this class's home module.
        llvm::GlobalVariable* getOrCreateSecondaryVTable(
            CajetaClassPtr parent);

        // Polymorphic-MI upcast adjustment. When assigning a class value
        // of static type `srcType` to a slot of declared type `dstType`,
        // and dstType is an ancestor of srcType whose sub-object lives at
        // non-zero offset inside srcType's instance layout, shift the
        // pointer to that sub-object's start. Returns `srcValue` unchanged
        // when no adjustment is needed (same class, first-parent chain,
        // interface, or non-class types). Used by LocalVariableDeclaration,
        // assignment, return, and parameter-passing sites that hand a
        // subclass instance to a parent-typed slot.
        static llvm::Value* adjustForUpcast(
            CajetaModulePtr module,
            llvm::Value* srcValue,
            CajetaTypePtr srcType,
            CajetaTypePtr dstType);

        // Synthesize a this-offset thunk. Used when a secondary vtable
        // entry points at a cross-class override: the thunk receives
        // `this` as the parent-sub-object pointer, subtracts
        // `parentOffsetInThis`, and tail-calls `impl` with the adjusted
        // pointer. Idempotent — looks up by symbol name first.
        llvm::Function* synthesizeOffsetThunk(
            CajetaClassPtr parent,
            MethodPtr impl,
            uint64_t parentOffsetInThis);

        void setVirtualTableType(llvm::StructType* llvmVirtualTableType) {
            this->llvmVirtualTableType = llvmVirtualTableType;
        }

        llvm::StructType* getVirtualTableType() {
            return llvmVirtualTableType;
        }

        void setVirtualTableGlobal(llvm::GlobalVariable* llvmVirtualTableGlobal) {
            this->llvmVirtualTableGlobal = llvmVirtualTableGlobal;
        }

        llvm::GlobalVariable* getVirtualTableGlobal() {
            return llvmVirtualTableGlobal;
        }

        // Synthesize (or return the cached) per-class drop wrapper. The
        // wrapper takes `ptr instance`, calls the user-declared `drop()`
        // method on it if one exists, then frees the heap allocation via
        // __cajeta_free. LocalVariableDeclaration registers a drop entry
        // pointing at this function for every class-typed local so
        // instances are reclaimed at scope exit. Returns null only when
        // the runtime helpers aren't yet linked, in which case the
        // caller skips the drop-entry registration. Virtual so subclasses
        // (e.g. CajetaTask) can inject extra synchronization — Task's
        // drop must wait for `done` before freeing or it races the
        // worker fiber.
        virtual llvm::Function* getOrCreateDropFunction();

        // P7.1 — synthesize (or return cached) stack-drop function. Same
        // contract as getOrCreateDropFunction except no __cajeta_free at
        // the end. For stack-allocated class locals where the body is
        // owned by the stack frame, not the heap allocator.
        llvm::Function* getOrCreateStackDropFunction();

        // Gap 1 (MemoryModel.md § Known gaps) — virtual dispatch on drop.
        // Patch the vtable global's drop_fn slot (index 3 in the vtable
        // layout) to point at this class's synthesized heap-drop wrapper.
        // Called lazily from LocalVariableDeclaration::generateCode when
        // a heap class local first registers its drop entry — the runtime
        // helper __cajeta_class_virtual_drop loads this slot to dispatch
        // through the dynamic type, so `Animal a = heap Dog()` fires
        // ~Dog() at scope exit.
        //
        // Idempotent: the patch only runs once per class (tracked via
        // llvmDropFunctionPatched). Safe to call multiple times.
        void patchVirtualTableDropFn();

        // Static class fields. Lazily defines (or fetches) the LLVM
        // global variable backing this class's static property `prop`.
        // The definition lives in the class's home module; if
        // `callerModule` differs, the returned global is the
        // module-local extern decl produced by ensureGlobalInModule
        // (resolved at JIT link time to the home module's definition).
        //
        // Returns nullptr when prop is null, not actually static, or
        // has no usable LLVM type.
        llvm::GlobalVariable* getOrCreateStaticFieldGlobal(
            StructurePropertyPtr prop,
            CajetaModulePtr callerModule = nullptr);

        // Virtual hook — true iff this class's instance layout has a
        // vtable pointer at LLVM struct index 0, the convention required
        // for __cajeta_class_virtual_drop to dispatch correctly. Plain
        // CajetaClass returns true. Subclasses with custom layouts
        // (CajetaTask<T>'s { fn, arg, done, ... } body has no vtable
        // slot) override to return false so callers fall back to static
        // dispatch through getOrCreateDropFunction.
        virtual bool hasVtablePointerAtSlotZero() const { return true; }

        // S9.5.2 — synthesize a per-(class, interface) vtable global for
        // every interface this class implements. Called from
        // generatePrototype after method prototypes exist. Same emission
        // shape as CajetaStruct::synthesizeInterfaceVTables — flat
        // `[N x ptr]` constant in interface-declaration order, named
        // `class.<sanitized>_iface_<sanitized>_VTable` (the prefix
        // distinguishes from the struct case).
        void synthesizeInterfaceVTables();

        // Lookup for the synthesized global by interface canonical name.
        // Returns nullptr if this class doesn't implement that interface.
        llvm::GlobalVariable* getInterfaceVTable(const std::string& interfaceCanonical) const {
            auto it = interfaceVTables.find(interfaceCanonical);
            return it != interfaceVTables.end() ? it->second : nullptr;
        }

        const std::map<std::string, llvm::GlobalVariable*>&
        getInterfaceVTables() const { return interfaceVTables; }

        void setRttiGlobal(llvm::GlobalVariable* llvmRttiGlobal) {
            this->llvmRttiGlobal = llvmRttiGlobal;
        }

        llvm::GlobalVariable* getRttiGlobal() {
            return llvmRttiGlobal;
        }

        void setClassBody(ClassBodyDeclarationPtr classBody);

        // Resolve names in `qExtended` to actual CajetaClassPtr instances and
        // populate `superClasses`. Looks up parents in the module's structures
        // map (populated when each class registers itself during prototype
        // generation), so parents must be declared earlier in the source than
        // their subclasses — forward references aren't supported in v1.
        //
        // The lookup tries the qName's full canonical (e.g. "test.Animal") and
        // falls back to the short type name (e.g. just "Animal") since
        // `QualifiedName::fromContext` for a single identifier picks the
        // wrong package on its own.
        void resolveSuperClasses();

        // Single-pass hierarchy walk that populates `virtualMethodList` in slot
        // order. For each non-static, non-constructor method:
        //   - If the canonical (unlabeled) signature is new, append a fresh slot.
        //   - If it matches an ancestor's method (override), replace the slot's
        //     MethodPtr with this class's method; the index stays the same.
        // Sets `Method::virtualTableIndex` to the slot index in passing.
        // Idempotent — clears `virtualMethodList` before rebuilding.
        void buildVirtualTable();

        // Full vtable build entry point: builds the slot list, then invokes
        // StructureMetadata::populate to materialize the LLVM vtable type and
        // global. Safe to call multiple times (no-ops after first success).
        void writeVirtualTable();

        // Template predicates and accessors. `isTemplate()` means typeParameters
        // were declared and no type arguments have been bound yet — the class
        // is a recipe, not a type. `isInstantiation()` means concrete arguments
        // have been supplied. See Templates.h and CajetaClass::instantiate.
        bool isTemplate() const { return !typeParameters.empty() && typeArguments.empty(); }
        bool isInstantiation() const { return !typeArguments.empty(); }

        // Materialize a concrete class from this template under the given
        // arguments. Idempotent: a second call with the same args returns
        // the cached instantiation. No-op if this class is not a template
        // (returns `this`). Defined in TemplateInstantiator.cpp; declared
        // here as a member for ergonomic call sites, but implemented in a
        // separate TU to keep CajetaClass.h free of visitor / parser
        // dependencies. See MEMORY model: the template's source snippet is
        // re-parsed on each unique instantiation; the result is cached in
        // `module->getStructures()` keyed by canonical-with-args name.
        CajetaClassPtr instantiate(vector<CajetaTypePtr> args);

        // Diamond-operator inference (TPL-7). Given the argument types of a
        // `new Box<>(args)` call site, examine this template's constructor
        // signatures and return the type-parameter bindings (in declaration
        // order). Throws CAJETA_ERROR_TYPE_INFERENCE on ambiguity, conflict,
        // or no match. Callers typically pass the result straight into
        // `instantiate(...)`.
        vector<CajetaTypePtr> inferDiamondArgs(const vector<CajetaTypePtr>& argTypes);
        const vector<TypeParameter>& getTypeParameters() const { return typeParameters; }
        const vector<CajetaTypePtr>& getTypeArguments() const { return typeArguments; }
        void setTypeParameters(vector<TypeParameter> params) { typeParameters = std::move(params); }
        void setTypeArguments(vector<CajetaTypePtr> args) { typeArguments = std::move(args); }
        const string& getTemplateSource() const { return templateSource; }
        void setTemplateSource(string src) { templateSource = std::move(src); }
        CajetaClassPtr getTemplateOrigin() const { return templateOrigin; }
        void setTemplateOrigin(CajetaClassPtr origin) { templateOrigin = std::move(origin); }

        llvm::Value* invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisInstance = nullptr,
                                   CajetaModulePtr callerModule = nullptr,
                                   bool forceDirectCall = false,
                                   const vector<CajetaTypePtr>& explicitMethodTypeArgs = {});

        // Look up a method on this class or any of its ancestors. Each class
        // indexes methods under keys that embed its own class name, so the
        // recursion re-keys at each level. Returns nullptr if not found.
        //
        // `explicitMethodTypeArgs` (default empty) carries the type-args
        // from `expr.<TypeArgs>method(args)` call sites. When non-empty,
        // the method-template fallback path uses the supplied args
        // directly (skipping unification) — both more efficient and
        // necessary when inference can't reach a binding from the
        // value args alone (e.g. `Optional.<int32>None()` has no value
        // args to infer T from).
        MethodPtr resolveMethod(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs = {});

        virtual void generatePrototype();

        virtual void generateCode();

        // P6.2 — emit a per-class clinit-style function that evaluates
        // any static-field initializer whose shape didn't constant-fold
        // (anything beyond a leading-sign integer/float literal) and
        // stores the result into the global. Registered with
        // llvm.global_ctors at default priority so it runs at module
        // load. No-op when every static field's initializer folded.
        void generateStaticInitializers();

        void generateMetadata();

        void ensureDefaultConstructor();

        // Inject a compiler-synthesized structural hash() override
        // when the class carries @AutoHash and doesn't manually
        // declare hash(). Called from generatePrototype after
        // ensureDefaultConstructor. See SynthesizedHashMethod for the
        // field-walk implementation and the diagnostic shape used
        // when an unsupported field type is encountered.
        void synthesizeAutoHash();

        // Lombok-mirror synthesizers (cajeta-docs/stdlib/Annotations.md
        // § Section 2). Each is gated on a class-level or field-level
        // annotation and runs once during generatePrototype after
        // ensureDefaultConstructor + synthesizeAutoHash. User-declared
        // methods with the same name+arity win — the synthesizer skips.

        // @Getter on class (all fields) or on individual fields.
        // Synthesizes `public <fieldType> <fieldName>()` per qualifying
        // field. See SynthesizedGetterMethod for codegen.
        void synthesizeGetters();

        // @Setter on class (all non-final fields) or on individual
        // fields. Synthesizes `public void <fieldName>(T v)`. Skipped
        // for `final` fields. See SynthesizedSetterMethod for codegen.
        void synthesizeSetters();

        void createInheritanceMethodMap(CajetaClassPtr structure = shared_ptr<CajetaClass>(nullptr));


        void setRttiType(llvm::StructType* llvmRttiType) {
            this->llvmRttiType = llvmRttiType;
        }

        llvm::StructType* getRttiType() {
            return llvmRttiType;
        }
    };
} // code