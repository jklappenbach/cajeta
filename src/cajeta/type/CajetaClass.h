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
#include <map>
#include <unordered_map>

namespace cajeta {
    class CajetaInterface;
    typedef shared_ptr<CajetaInterface> CajetaInterfacePtr;

    // U6.3 — the per-class codegen bindings that are LLVMContext-bound (struct
    // types, globals, functions) plus the per-context emission flags. In the
    // frozen-shared stdlib model each thread compiles into its OWN LLVMContext,
    // so a shared (frozen) CajetaClass cannot hold these inline; when frozen,
    // access routes to a per-thread side-table keyed by the class pointer
    // (frozenClassBindings()). Behaviour is unchanged while frozen=false — the
    // inline members are still used. See compiler-threadsafe-plan U6.
    struct ClassLlvmBindings {
        llvm::StructType* llvmVirtualTableType = nullptr;
        llvm::StructType* llvmRttiType = nullptr;
        llvm::StructType* llvmReferenceType = nullptr;
        llvm::GlobalVariable* llvmVirtualTableGlobal = nullptr;
        llvm::GlobalVariable* llvmRttiGlobal = nullptr;
        llvm::GlobalVariable* llvmClassObjectGlobal = nullptr;
        llvm::Function* llvmDropFunction = nullptr;
        llvm::Function* llvmStackDropFunction = nullptr;
        bool llvmDropFunctionPatched = false;
        llvm::Function* llvmReflectInvokeFunction = nullptr;
        bool llvmReflectInvokeBodyEmitted = false;
        llvm::Function* llvmReflectNewFunction = nullptr;
        bool llvmReflectNewBodyEmitted = false;
        std::map<std::string, llvm::GlobalVariable*> interfaceVTables;
        std::map<std::string, llvm::GlobalVariable*> staticFieldGlobals;
        std::map<std::string, llvm::GlobalVariable*> secondaryVTables;
    };

    class ClassBodyDeclaration;
    typedef shared_ptr<ClassBodyDeclaration> ClassBodyDeclarationPtr;

    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaClass;
    typedef shared_ptr<CajetaClass> CajetaClassPtr;

    // Discriminator written into word 2 of an interface fat pointer.
    // Selected at the assignment site that builds the fat pointer
    // (class→iface, #class→iface) and consumed by the kind-tag drop
    // dispatch in __cajeta_iface_drop.
    enum InterfaceValueKind : int64_t {
        IFACE_KIND_BORROWED_CLASS = 0,
        IFACE_KIND_OWNED_CLASS = 1
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
        // Type arguments captured at parse time from `implements
        // Foo<X, Y>` clauses, parallel to `qImplemented` (empty inner
        // vector for non-templated interface references like
        // `implements Comparable`). The args are stored as raw
        // QualifiedNamePtrs from the parse tree — resolving them to
        // canonical CajetaTypes at validation time avoids the
        // dependency-cycle headache of resolving during the parse walk
        // when the referenced types may not yet be in canonicalMap.
        //
        // Used by @Encoding to verify the encoder declared `implements
        // Encoder<T>` for the correct T. The longer-term use is per-
        // (impl, interface<T>) vtable instantiation; today only the
        // verification path consumes this data.
        list<vector<QualifiedNamePtr>> qImplementedTypeArgs;

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
        bool annotationFlag = false;
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
        bool recordType = false;
        CajetaModulePtr module;
        // Emit target for this class's own IR (vtable / RTTI / clinit / static
        // fields). Null → getEmitModule() falls back to `module` (production /
        // non-reuse: resolution and emission coincide). Set only in the stdlib
        // test-reuse path for a stdlib-template instantiation over a USER type:
        // `module` stays the stdlib template module (resolution), emitModule is
        // the user module, so this instantiation's globals land there and the
        // cached stdlib stays pristine. Methods inherit it at construction.
        CajetaModulePtr emitModule;
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
        // element-ownership §2 — per-type-argument ownership mode of THIS
        // instantiation: typeArgumentOwning[i] == true means position i was
        // instantiated `#` (owning). Parallel to typeArguments; empty on a
        // template or when every position is borrow (the default). Folded into
        // the instantiation's cache key/name so `HashMap<#String,V>` and
        // `HashMap<String,V>` are distinct monomorphizations (§8.3.1).
        vector<bool> typeArgumentOwning;
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
        // Cached per-class reflection object — a constant
        // `cajeta.reflect.Class` instance { ptr Class#VTable, ptr rtti }.
        // `T.class` lowers to its address; `getClass()` reaches it through
        // the vtable's classObject slot. One per type, no allocation
        // (Reflection.md Strategy 7).
        llvm::GlobalVariable* llvmClassObjectGlobal = nullptr;
        // Synthesized drop wrapper for this class — see getOrCreateDropFunction.
        // Cached on first request so LocalVariableDeclaration's drop-entry
        // registration is a constant-time lookup per declaration site.
        llvm::Function* llvmDropFunction = nullptr;
        // Synthesized stack-drop wrapper. Same shape as the heap drop
        // above but does NOT free the body; intended for stack-allocated
        // class locals whose body is reclaimed by the function epilogue.
        // Walks owned class-ref fields so embedded ownership doesn't leak.
        llvm::Function* llvmStackDropFunction = nullptr;
        // Gap 1 (virtual dispatch on drop) — set true after the vtable
        // global's drop_fn slot has been patched to point at
        // llvmDropFunction. Keeps patchVirtualTableDropFn idempotent so
        // multiple heap-class local sites all see a no-op after the
        // first patch.
        bool llvmDropFunctionPatched = false;

        // REFL-2: synthesized per-class reflection adapters. The invoke adapter
        // is `void(ptr obj, i32 methodIndex, ptr args, ptr ret)` — a switch over
        // the class's method-list index that marshals args and makes a direct
        // call. Forward-declared (no body) when the #Rtti constant is built so
        // it can take the address; emitReflectInvokeBody fills the body in a
        // post-quiescence pass once every method's LLVM function exists.
        llvm::Function* llvmReflectInvokeFunction = nullptr;
        bool llvmReflectInvokeBodyEmitted = false;

        // REFL-2C: synthesized per-class newInstance adapter. Shape:
        //   ptr __cajeta_<canon>_reflect_new(i32 ctorIndex, ptr args)
        // a switch over the constructor index (see getReflectConstructorList)
        // that allocates + zeroes + installs the vtable(s) + runs the chosen
        // constructor, returning the new instance. Forward-declared when #Rtti
        // is built; body filled by emitReflectNewBody post-quiescence.
        llvm::Function* llvmReflectNewFunction = nullptr;
        bool llvmReflectNewBodyEmitted = false;

        // Per-(class, interface) vtable globals. Keyed by interface
        // canonical name; value is a `[N x ptr]` constant whose entries
        // point at this class's concrete implementations in interface-
        // declaration order. The fat-pointer dispatch model uses these
        // as the vtable_ptr word of the interface value.
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

        // Reuse-cache baseline of this class's MODULE-BOUND llvm bindings,
        // snapshotted at stdlib prime (StdlibReuseCache). All of these are
        // lazily materialized into whatever llvm::Module is active at first use;
        // on the test stdlib-reuse path that can be a per-test USER module, and
        // the cached pointer then outlives that module, producing "references a
        // function/global in a different module" on the NEXT reusing test.
        // restoreReuseBaseline() resets them to the snapshot so each test
        // regenerates into its own module. StructType*/layout fields are NOT
        // snapshotted — they're context-bound (the reuse context is shared) and
        // module-independent, so they stay valid across tests.
        struct ReuseBindingBaseline {
            bool valid = false;
            // The class's emit-target module at prime. If a reusing test sets
            // emitModule to its per-test module (redirect / reuse sink), every
            // subsequent getEmitModule()-driven lookup on this PERSISTENT class —
            // e.g. the runtime-fn callees (__cajeta_free / __cajeta_class_virtual_drop)
            // resolved inside its drop body — lands in that now-freed module,
            // producing cross-module references on the next test. Restored so
            // baseline classes fall back to their own (stdlib) module each test.
            CajetaModulePtr emitModule;
            llvm::GlobalVariable* vtableGlobal = nullptr;
            llvm::GlobalVariable* rttiGlobal = nullptr;
            llvm::Function* dropFunction = nullptr;
            llvm::Function* stackDropFunction = nullptr;
            bool dropFunctionPatched = false;
            std::map<std::string, llvm::GlobalVariable*> interfaceVTables;
            std::map<std::string, llvm::GlobalVariable*> staticFieldGlobals;
            std::map<std::string, llvm::GlobalVariable*> secondaryVTables;
            std::map<Method*, llvm::Function*> methodFns;
        };
        ReuseBindingBaseline reuseBaseline;

        // MultiClassing Phase 3 v4 vbase ABI (docs/specification/
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
        // Annotation type (`annotation Foo {}`): registered in canonicalMap so it
        // resolves as a type token, but never prototyped into structures (so it
        // stays out of the type-based-pointcut discriminator). See
        // visitAnnotationTypeDeclaration / buildPendingPrototypes.
        bool isAnnotation() const { return annotationFlag; }
        void setIsAnnotation(bool v) { annotationFlag = v; }
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
        const list<QualifiedNamePtr>& getQExtended() const { return qExtended; }
        void setQImplemented(list<QualifiedNamePtr> q) { qImplemented = std::move(q); }
        const list<vector<QualifiedNamePtr>>& getQImplementedTypeArgs() const {
            return qImplementedTypeArgs;
        }
        void setQImplementedTypeArgs(list<vector<QualifiedNamePtr>> a) {
            qImplementedTypeArgs = std::move(a);
        }
        void resolveImplementedInterfaces();

        ScopePtr getScope() { return scope; }

        void addMethod(MethodPtr method);
        // Fully unregister a method from every per-class map addMethod populates
        // (methods, staticMethods, methodList, labeled/unlabeled method+ctor
        // maps). Used by the stdlib test-reuse path to drop a prior test's
        // method-template instantiation that was registered on a persistent
        // stdlib host class, so the next test can re-register + re-codegen it into
        // its own module. No production caller (instantiations there live as long
        // as the class).
        void removeMethod(const MethodPtr& method);

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

        // Reparent the emit-target module of an instantiation. Used by the stdlib
        // test-reuse path (Design B): a user-triggered stdlib-template
        // instantiation is owned by the USER module so its vtable/RTTI/bodies
        // emit there, leaving the cached stdlib module pristine. Call BEFORE
        // generatePrototype. `module->getLlvmModule()` is the codegen target.
        void setModuleForInstantiation(CajetaModulePtr m) { module = m; }

        list<MethodPtr>& getMethodList() { return methodList; }

        // Reuse-cache only (StdlibReuseCache). captureReuseBaseline() snapshots
        // this class's module-bound llvm bindings at stdlib prime;
        // restoreReuseBaseline() resets any that drifted (lazily generated into a
        // per-test user module) back to the snapshot between tests, so the next
        // reusing test regenerates into its own module. No-ops / unused in
        // production. See ReuseBindingBaseline.
        void captureReuseBaseline();
        void restoreReuseBaseline();

        CajetaModulePtr getModule() { return module; }

        // The module this class's own IR is CREATED in (see emitModule). Falls
        // back to the resolution module when unset — production / non-reuse.
        CajetaModulePtr getEmitModule() { return emitModule ? emitModule : module; }
        void setEmitModule(CajetaModulePtr m) { emitModule = m; }

        list<CajetaClassPtr>& getSuperClasses() {
            // Lazy re-resolve: a NAMED parent (qExtended) may not have resolved
            // into superClasses at prototype time because it loaded LATER (the
            // stdlib parses a package's files alphabetically, so a sub-interface
            // sorting before its base — `AudioBackend` < `Backend` — is built
            // first, leaving its `extends Backend` link empty and cached). Once
            // the parent registers, this re-resolution picks it up so the
            // interface vtable synthesis and method-inheritance walks see the
            // inherited obligations. Idempotent + only fires while short.
            if (superClasses.size() < qExtended.size()) {
                resolveSuperClasses();
            }
            return superClasses;
        }

        // LLVM struct index for a class field. Class instances reserve LLVM
        // slot 0 for the vtable pointer, so user properties live at LLVM
        // indices 1..N even though `StructureProperty::getOrder()` is
        // 0-based. `CajetaView` overrides this to return the order
        // verbatim (no vtable slot — views are inline byte overlays).
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

        // title-tracking §5 — per-instance field ownership bits. A class-
        // reference or heap-array own field gets one bit in a hidden i64
        // appended after the class's own fields (before its vbase slots);
        // the store's spelling sets it, teardown consults it. Shared-
        // capable classes (String/Utf8/Slice) are excluded — their fields
        // transfer by share/COW and stay always-owned (§5.1.6). Views,
        // interfaces, value types, and inline arrays store inline and
        // carry no separable title.
        static bool fieldHasOwnershipBit(const StructurePropertyPtr& p);

        // title-tracking §5 (Unit 4) — per-slot ownership bits for a
        // reference array's ELEMENTS. True when `elem` is a plain vtable
        // class: the slot's store spelling marks a bit in the array local's
        // sidecar bitmap and the element walk releases marked slots via
        // __cajeta_class_virtual_drop. String keeps its own family (§5.1.6
        // share path); nested arrays, views, interfaces, and value types
        // carry no per-slot title.
        static bool arrayElementCarriesSlotBits(const CajetaTypePtr& elem);

        // Dense bit index of `p` among this class's OWN bit-carrying
        // fields (declaration order), or -1.
        int ownershipBitIndexOf(const StructurePropertyPtr& p) const {
            if (!p || p->isStatic()) return -1;
            int idx = 0;
            for (const auto& q : propertyList) {
                if (q->isStatic() || !fieldHasOwnershipBit(q)) continue;
                if (q.get() == p.get()) return idx;
                idx++;
            }
            return -1;
        }

        bool needsOwnershipWord() const {
            for (const auto& q : propertyList) {
                if (!q->isStatic() && fieldHasOwnershipBit(q)) return true;
            }
            return false;
        }

        // LLVM struct slot of this class's hidden ownership word within its
        // own layout (also valid inside a descendant's embedding, relative
        // to this class's sub-object). -1 when the class has none.
        int getOwnershipWordLlvmIndex() const {
            if (!needsOwnershipWord()) return -1;
            StructurePropertyPtr last;
            for (const auto& q : propertyList) {
                if (!q->isStatic()) last = q;
            }
            if (!last) return -1;
            int lastIdx = getFieldLlvmIndex(last);
            return lastIdx < 0 ? -1 : lastIdx + 1;
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
                    // title-tracking §5: skip cls's hidden ownership word
                    // (inserted after own properties, before vbases — must
                    // mirror embedSubObject exactly).
                    if (result < 0 && cls->needsOwnershipWord()) {
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
            // @ValueType PODs have no slot 0 vtable — fields start at index 0.
            walk(this, /*ownVtable=*/hasVtablePointerAtSlotZero());
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

        // U6.3 — per-thread side-table of codegen bindings, used only when this
        // class is frozen (shared, immutable semantic model). Keyed by class
        // identity; thread_local so each compile thread keeps its own
        // LLVMContext-bound globals/functions. Defined in CajetaClass.cpp.
        static std::unordered_map<const CajetaClass*, ClassLlvmBindings>& frozenClassBindings();

        // Frozen-aware accessors. While frozen=false they alias the inline
        // members (behaviour-identical to pre-U6.3); while frozen they redirect
        // to this thread's side-table entry. Reference-returning so call sites
        // both read and write through them. unordered_map guarantees reference
        // stability across inserts, so an `auto& b = xRef();` stays valid even
        // as other classes seed their entries.
        llvm::StructType*& vtableTypeRef() {
            return frozen ? frozenClassBindings()[this].llvmVirtualTableType : llvmVirtualTableType;
        }
        llvm::StructType*& rttiTypeRef() {
            return frozen ? frozenClassBindings()[this].llvmRttiType : llvmRttiType;
        }
        llvm::StructType*& referenceTypeRef() {
            return frozen ? frozenClassBindings()[this].llvmReferenceType : llvmReferenceType;
        }
        llvm::GlobalVariable*& vtableGlobalRef() {
            return frozen ? frozenClassBindings()[this].llvmVirtualTableGlobal : llvmVirtualTableGlobal;
        }
        llvm::GlobalVariable*& rttiGlobalRef() {
            return frozen ? frozenClassBindings()[this].llvmRttiGlobal : llvmRttiGlobal;
        }
        llvm::GlobalVariable*& classObjectGlobalRef() {
            return frozen ? frozenClassBindings()[this].llvmClassObjectGlobal : llvmClassObjectGlobal;
        }
        llvm::Function*& dropFnRef() {
            return frozen ? frozenClassBindings()[this].llvmDropFunction : llvmDropFunction;
        }
        llvm::Function*& stackDropFnRef() {
            return frozen ? frozenClassBindings()[this].llvmStackDropFunction : llvmStackDropFunction;
        }
        bool& dropFnPatchedRef() {
            return frozen ? frozenClassBindings()[this].llvmDropFunctionPatched : llvmDropFunctionPatched;
        }
        llvm::Function*& reflectInvokeFnRef() {
            return frozen ? frozenClassBindings()[this].llvmReflectInvokeFunction : llvmReflectInvokeFunction;
        }
        bool& reflectInvokeBodyEmittedRef() {
            return frozen ? frozenClassBindings()[this].llvmReflectInvokeBodyEmitted : llvmReflectInvokeBodyEmitted;
        }
        llvm::Function*& reflectNewFnRef() {
            return frozen ? frozenClassBindings()[this].llvmReflectNewFunction : llvmReflectNewFunction;
        }
        bool& reflectNewBodyEmittedRef() {
            return frozen ? frozenClassBindings()[this].llvmReflectNewBodyEmitted : llvmReflectNewBodyEmitted;
        }
        std::map<std::string, llvm::GlobalVariable*>& interfaceVTablesRef() {
            return frozen ? frozenClassBindings()[this].interfaceVTables : interfaceVTables;
        }
        const std::map<std::string, llvm::GlobalVariable*>& interfaceVTablesRef() const {
            if (frozen) {
                auto& t = frozenClassBindings();
                auto it = t.find(this);
                if (it != t.end()) return it->second.interfaceVTables;
                static const std::map<std::string, llvm::GlobalVariable*> empty;
                return empty;
            }
            return interfaceVTables;
        }
        std::map<std::string, llvm::GlobalVariable*>& staticFieldGlobalsRef() {
            return frozen ? frozenClassBindings()[this].staticFieldGlobals : staticFieldGlobals;
        }
        std::map<std::string, llvm::GlobalVariable*>& secondaryVTablesRef() {
            return frozen ? frozenClassBindings()[this].secondaryVTables : secondaryVTables;
        }

        void setVirtualTableType(llvm::StructType* llvmVirtualTableType) {
            this->vtableTypeRef() = llvmVirtualTableType;
        }

        llvm::StructType* getVirtualTableType() {
            return vtableTypeRef();
        }

        void setVirtualTableGlobal(llvm::GlobalVariable* llvmVirtualTableGlobal) {
            this->vtableGlobalRef() = llvmVirtualTableGlobal;
        }

        llvm::GlobalVariable* getVirtualTableGlobal() {
            return vtableGlobalRef();
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

        // True iff this class's stack drop does nothing observable — it would
        // emit NO calls (no user `drop()`, no owning ancestor user `drop()`, no
        // owned class-ref fields). Such a type's scope-exit drop is a no-op, so
        // registering a drop entry for a stack local of it is pure overhead
        // (the time-* 230x tax). Mirrors exactly the call-emitting branches of
        // getOrCreateStackDropFunction.
        bool hasTrivialStackDrop();

        // slice-spec §6.1 (slices Unit 6b) — the synthesized value-copy/drop
        // hook family. A VALUE type is shared-capable when it is Utf8 itself
        // or transitively embeds a shared-capable value field; such values
        // are non-trivial: copies retain, drops release, per Shared stake.
        bool isSharedCapableValue();

        // Emit the recursive per-field retain (copy hook) or release (drop
        // hook) walk over a value of this type at `valuePtr`. O(shared
        // fields); never traverses heap edges (spec §6.1 bounded copy).
        void emitValueSharedOp(llvm::IRBuilder<>& b, llvm::Value* valuePtr,
                               CajetaModulePtr cajModule,
                               llvm::Module* bodyModule, bool retain);

        // Synthesized `void(ptr)` release wrapper for drop-chain entries on
        // shared-capable value locals (obj = the value's stack slot).
        llvm::Function* getOrCreateValueReleaseFunction();

        // REFL-2: return (creating on first call) the DECLARATION of this
        // class's reflective invoke adapter — `void(ptr obj, i32 methodIndex,
        // ptr args, ptr ret)`. No body is emitted here; the #Rtti constant
        // references the declaration, and emitReflectInvokeBody fills it in
        // later. Returns null only if creation isn't possible.
        llvm::Function* getOrCreateReflectInvokeDecl();

        // REFL-2: emit the body of the reflective invoke adapter (idempotent).
        // Called once per class after the Phase-1/2 codegen loop quiesces, so
        // every method's getLlvmFunction() is available for a direct call.
        void emitReflectInvokeBody();

        // REFL-2C: deterministically-ordered list of this class's constructors
        // (sorted by canonical signature). The index space the newInstance
        // adapter switches over and the #Rtti constructor table report.
        std::vector<MethodPtr> getReflectConstructorList();

        // REFL-2C: declaration / body of the reflective newInstance adapter,
        // mirroring getOrCreateReflectInvokeDecl / emitReflectInvokeBody.
        llvm::Function* getOrCreateReflectNewDecl();
        void emitReflectNewBody();

        // REFL-8/REFL-10: post-pass that patches a deferred #ClassObject. A
        // class parsed before cajeta.reflect.Class gets a #ClassObject whose
        // slot 0 (Class#VTable) was NULL at populate time and was left out of
        // the process registry. Once the whole stdlib (Class included) is built,
        // this fills slot 0 with the real Class#VTable and registers the class,
        // so forName/allClasses/classesInPackage/classesAnnotated can reflect on
        // it without a null-vtable virtual-dispatch crash. No-op for classes
        // whose slot 0 already resolved (and were already registered).
        void finalizeClassObject();

        // REFL-1.7: cajeta.reflect.Class is a template Class<T>. Force-build the
        // canonical wildcard instantiation Class<?> (idempotent, no-op if Class
        // isn't a template or is absent). Called once after parse/prototype and
        // before Phase 1/2 codegen so its method bodies are emitted and every
        // type's #ClassObject can embed its (shared) vtable.
        static void ensureClassWildcardInstantiated();

        // Implicit destructor chaining helpers (MemoryModel.md § 140,
        // C++ semantics).
        //
        // `emitDropBodyInline` emits (1) a call to this class's user
        // `~Class() { ... }` if defined, then (2) auto-drops for each
        // OWN owned field (propertyList) in reverse declaration order.
        // No parent chain, no __cajeta_free. Used by both the heap and
        // stack drop wrappers — they call this for the most-derived
        // class, then walk transitive ancestors via
        // `collectDestructorChain` and call this for each ancestor.
        //
        // `collectDestructorChain` returns the deduped list of
        // ancestors in destruction order — reverse DFS over direct
        // parents in reverse declaration order, skipping interfaces
        // and skipping any ancestor already visited (diamond dedup).
        // Each ancestor in the returned list runs exactly once.
        void emitDropBodyInline(llvm::IRBuilder<>& b,
                                llvm::Value* instance,
                                CajetaModulePtr module);
        std::vector<CajetaClassPtr> collectDestructorChain();

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
        //
        // @ValueType PODs are true value types: a flat `{ field… }` struct
        // with NO vtable slot (final, no virtual dispatch, operators are
        // static, Copy semantics → no drop). Returning false here makes the
        // layout builder + getFieldLlvmIndex + construction skip the slot 0
        // vtable, so fields live at indices 0,1,… — register-friendly POD.
        virtual bool hasVtablePointerAtSlotZero() const { return !isValueType(); }

        // `record` declarations only (subset of value types): immutable
        // fields, intrinsic with(...). Set by the visitor / template
        // instantiator; plain @ValueType classes stay mutable.
        void setRecordType(bool v) { recordType = v; }
        bool isRecordType() const { return recordType; }

        // Synthesize a per-(class, interface) vtable global for every
        // interface this class implements. Called from generatePrototype
        // after method prototypes exist. Emission shape: flat `[N x ptr]`
        // constant in interface-declaration order, named
        // `class.<sanitized>_iface_<sanitized>_VTable`.
        void synthesizeInterfaceVTables();

        // Flat list of methods that an interface's per-(impl, iface)
        // vtable lays out, in vtable order: this interface's own
        // methods first, then each parent interface's methods (via
        // BFS over the extends chain), with first-seen-by-name
        // winning so an override declared at the leaf doesn't
        // appear twice. Constructors and statics are filtered. Used
        // by both synthesizeInterfaceVTables (writing the vtable
        // entries) and invokeMethod (looking up the methodIdx for
        // dispatch) — they MUST agree on order, hence the shared
        // helper.
        std::vector<MethodPtr> getFlattenedInterfaceMethods();

        // Lookup for the synthesized global by interface canonical name.
        // Returns nullptr if this class doesn't implement that interface.
        llvm::GlobalVariable* getInterfaceVTable(const std::string& interfaceCanonical) const {
            const auto& m = interfaceVTablesRef();
            auto it = m.find(interfaceCanonical);
            return it != m.end() ? it->second : nullptr;
        }

        const std::map<std::string, llvm::GlobalVariable*>&
        getInterfaceVTables() const { return interfaceVTablesRef(); }

        void setRttiGlobal(llvm::GlobalVariable* llvmRttiGlobal) {
            this->rttiGlobalRef() = llvmRttiGlobal;
        }

        llvm::GlobalVariable* getRttiGlobal() {
            return rttiGlobalRef();
        }

        void setClassObjectGlobal(llvm::GlobalVariable* g) {
            this->classObjectGlobalRef() = g;
        }

        llvm::GlobalVariable* getClassObjectGlobal() {
            return classObjectGlobalRef();
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

        // True iff at least one of this instantiation's type arguments
        // is the wildcard sentinel. Distinguishes `Stream<?>` from
        // `Stream<int32>`. Step 1 — template wildcards.
        bool isWildcardInstantiation() const;

        // True iff any type argument is a BOUNDED wildcard (`? extends B` /
        // `? super B`) — NOT the unbounded `?`. A bounded-wildcard instantiation
        // (`Holder<? extends Floating>`) is an abstract handle: its method bodies
        // are deliberately not codegen'd (no concrete element layout; the own-T
        // internal write would trip PECS), so code operates on it by capturing
        // back to a concrete instantiation. Unbounded `?` (e.g. `Class<?>`) is
        // excluded — reflection force-builds and dispatches it, so its bodies
        // MUST be emitted.
        bool isBoundedWildcardInstantiation() const;

        // True iff `from` (a concrete instantiation of some template)
        // is assignable to `wildcardInst` (a wildcard instantiation of
        // the same template). Identity check on `templateOrigin` —
        // `ArrayStream<int32>` is NOT assignable to `Stream<?>` here
        // because that would require walking the super-chain
        // (deferred until the chain-walk wiring lands in Step 5).
        // Variance (`? extends` / `? super`) lands with Step 6.
        static bool isAssignableToWildcard(
            CajetaClassPtr from, CajetaClassPtr wildcardInst);

        // Numeric template-bound markers (numeric-bounds-spec.md). The single
        // source of truth for "does type `arg` satisfy numeric marker `marker`"
        // (Numeric/Floating/Integral/Complex), used by both the `<T extends M>`
        // parameter-bound check (TemplateInstantiator) and the
        // `Tensor<? extends M>` wildcard-admission check (isAssignableToWildcard).
        // Dual conformance: a primitive satisfies it intrinsically via the
        // type-flag lattice (boolean excluded); a class satisfies it nominally by
        // implementing the cajeta.lang marker.
        static bool isNumericMarkerName(const string& name);
        static bool satisfiesNumericMarker(CajetaTypePtr arg, const string& marker);

        // Materialize a concrete class from this template under the given
        // arguments. Idempotent: a second call with the same args returns
        // the cached instantiation. No-op if this class is not a template
        // (returns `this`). Defined in TemplateInstantiator.cpp; declared
        // here as a member for ergonomic call sites, but implemented in a
        // separate TU to keep CajetaClass.h free of visitor / parser
        // dependencies. See MEMORY model: the template's source snippet is
        // re-parsed on each unique instantiation; the result is cached in
        // `module->getStructures()` keyed by canonical-with-args name.
        // `argOwning` (element-ownership §2) carries the per-position `#` mode;
        // empty = all borrow (the default, so existing callers are unchanged).
        // It is folded into the cache key so owning/borrow instantiations are
        // distinct, and stored on the result for later query.
        CajetaClassPtr instantiate(vector<CajetaTypePtr> args,
                                   vector<bool> argOwning = {});
        // The actual instantiation logic; `instantiate` is a thin wrapper that
        // also records cross-module instantiation obligations (incremental
        // compilation, Phase 2/3 — docs/IncrementalCompilation.md).
        CajetaClassPtr instantiateInternal(vector<CajetaTypePtr> args,
                                           vector<bool> argOwning = {});

        // Diamond-operator inference (TPL-7). Given the argument types of a
        // `new Box<>(args)` call site, examine this template's constructor
        // signatures and return the type-parameter bindings (in declaration
        // order). Throws CAJETA_ERROR_TYPE_INFERENCE on ambiguity, conflict,
        // or no match. Callers typically pass the result straight into
        // `instantiate(...)`.
        vector<CajetaTypePtr> inferDiamondArgs(const vector<CajetaTypePtr>& argTypes);
        const vector<TypeParameter>& getTypeParameters() const { return typeParameters; }
        const vector<CajetaTypePtr>& getTypeArguments() const { return typeArguments; }
        // element-ownership §2 — is type-argument position i owning (`#`)?
        bool isTypeArgumentOwning(size_t i) const {
            return i < typeArgumentOwning.size() && typeArgumentOwning[i];
        }
        const vector<bool>& getTypeArgumentOwning() const { return typeArgumentOwning; }
        // element-ownership §5.1.1 — true when this instantiation left a type
        // argument plain at a position the template author marked `#` (a
        // dissolved `#K` formal, or a `#K` extractor return), and that
        // argument has reference semantics (a borrowed value type is
        // share/copy-managed — nothing to outlive). Such a container borrows
        // elements it does not own, so its VALUE is scope-confined: no field
        // store, no `#` return, no owning containment (§8.2.2). Queried by
        // the Unit 7 confinement gates. Defined in CajetaClass.cpp.
        bool isBorrowModeContainer();
        // Transitional (element-ownership Unit 7, removed by Unit 8): the
        // confinement gates skip instantiations of STDLIB templates —
        // stdlib calling conventions (plain `Optional<T>` returns,
        // `Cache<String,..>` fields) predate the static model and are swept
        // to owning instantiations in Unit 8, alongside the 4B exemptions.
        bool isStdlibTemplateInstantiation();
        void setTypeArgumentOwning(vector<bool> v) { typeArgumentOwning = std::move(v); }
        void setTypeParameters(vector<TypeParameter> params) { typeParameters = std::move(params); }
        void setTypeArguments(vector<CajetaTypePtr> args) { typeArguments = std::move(args); }
        const string& getTemplateSource() const { return templateSource; }
        void setTemplateSource(string src) { templateSource = std::move(src); }
        CajetaClassPtr getTemplateOrigin() const { return templateOrigin; }
        void setTemplateOrigin(CajetaClassPtr origin) { templateOrigin = std::move(origin); }

        // sretTarget: for a value-returning (sret) method, the caller-owned
        // slot the result is constructed into. When null and the resolved
        // method returns a stack value, invokeMethod materializes a temp slot
        // in the caller's frame and returns a pointer to it — so the result
        // behaves like any class-instance pointer. See ValueReturns.md.
        llvm::Value* invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisInstance = nullptr,
                                   CajetaModulePtr callerModule = nullptr,
                                   bool forceDirectCall = false,
                                   const vector<CajetaTypePtr>& explicitMethodTypeArgs = {},
                                   llvm::Value* sretTarget = nullptr);

        // Construction helpers, factored out of ClassCreatorRest::generateCode
        // so synthesized codegen sites (a throwing capture cast, tryAs, ...)
        // can build a class instance without re-implementing the malloc /
        // vtable / ctor subtleties.
        //
        // initInstanceLayout: zero-init `instance` (LLVM type `structTy`) and
        // install the slot-0 primary vtable, any secondary sub-object vtables,
        // and — for heap instances (stackAlloc == false) — the virtual drop-fn
        // patch. @ValueType PODs (no slot-0 vtable) get only the zero-init.
        void initInstanceLayout(CajetaModulePtr module, llvm::Value* instance,
                                llvm::Type* structTy, bool stackAlloc);
        // heapConstruct: malloc + initInstanceLayout + invoke the matching
        // constructor with `entries`. Returns the new instance pointer. No
        // scope drop is registered — the caller owns the lifecycle (e.g.
        // throws it, or transfers it into an Optional).
        llvm::Value* heapConstruct(CajetaModulePtr module,
                                   vector<ParameterEntry>& entries);

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
        // `activeModule` (default null) is the module whose codegen is in flight
        // at the call site. When a method-template instantiation is brought to
        // life during codegen, its IRBuilder insert-point save/restore must use
        // the ACTIVE module's builder, not the instantiation's emit module (which
        // can differ — and yield a freed insert block — when the template lives in
        // a classpath .cja). resolveTypes-phase callers leave it null.
        MethodPtr resolveMethod(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs = {},
            CajetaModulePtr activeModule = nullptr);

        // Register + prototype + generate a method-template instantiation
        // exactly as a call site would (bringMethodTemplateInstantiationToLife).
        // Incremental-compilation obligation replay uses this: a skipped
        // module's call sites never fire, so the replayed instantiation must
        // be brought to life explicitly or its body never emits. Idempotent.
        void ensureMethodInstantiationAlive(MethodPtr inst);

        // Named arguments — option C (positional prefix + named suffix). When a
        // call mixes positional and named args (`f(a, b, x: 1, y: 2)`), reorder
        // `parameters` into formal declaration order and strip the labels, so the
        // ordinary positional resolution + codegen apply. Returns true if the call
        // was partial (and has been rewritten); false for all-positional or
        // all-labeled calls (left untouched — those keep their existing paths).
        // Throws (LANG-NAMEDARG) on an invalid mix: a positional arg after a named
        // one, a duplicate label, or no method whose parameter names match.
        bool normalizePartialLabeledCall(const string& methodName, bool isConstructor,
                                         vector<ParameterEntry>& parameters);

        virtual void generatePrototype();

        // U6.4.2 — pure instance-struct layout, factored out of
        // generatePrototype so a frozen class can rebuild its body in a
        // thread's own LLVMContext (the frozen getLlvmType() path). Computes
        // members + setBody and the sub-object / vbase slot maps; does NO
        // registration or method prototyping. Re-runnable byte-identically.
        void buildInstanceStructBody(llvm::LLVMContext* lctx);

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

        // Lombok-mirror synthesizers (docs/specification/reflect/Annotations.md
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

        // @ToString on class. Synthesizes `public String toString()`
        // returning `ClassName(field1=v1,field2=v2,...)`. Fields with
        // @ToString.Exclude are omitted. v1 only supports
        // format=TO_STRING_PROPERTIES (the default); JSON form is
        // deferred to cajeta.codec.json (S-1101/S-1102). See
        // SynthesizedToStringMethod for codegen.
        void synthesizeToString();

        // @NoArgsConstructor / @AllArgsConstructor /
        // @RequiredArgsConstructor — synthesize the matching ctor when
        // not already declared. Skipped silently if a same-shape ctor
        // already exists (user wins). See SynthesizedConstructorMethod
        // for codegen — body is field zero-init then per-arg store; no
        // implicit super-ctor chain in v1 (classes with non-trivial
        // parent ctors hand-write).
        void synthesizeNoArgsConstructor();
        void synthesizeAllArgsConstructor();
        void synthesizeRequiredArgsConstructor();

        // Shared helper for the three ctor synthesizers. Adds the
        // SynthesizedConstructorMethod, and — when the annotation
        // carries `staticName="..."` — pairs it with a
        // SynthesizedStaticFactoryMethod (the ctor is forced PRIVATE;
        // the factory carries the `access` modifier). See
        // CajetaClass.cpp for the Lombok-parity semantics.
        void emitCtorAndOptionalFactory(
            const AnnotationInstancePtr& ann,
            const std::string& annotationName,
            vector<StructurePropertyPtr> fields,
            Modifier access);

        // @With on class (synthesizes per-field withX(T v) methods) or
        // on individual fields (only that field). Each method allocates
        // a fresh instance, memcpys the source body (preserves vtable +
        // every field), overwrites the named slot, and returns the new
        // ptr. Naming: with for field `x` is `withX(T v)` (Lombok form
        // — Cajeta's size()-style would collide with @Setter).
        void synthesizeWith();

        // @Encoding on class — routes serialization through a user-
        // supplied codec class (`@Encoding(MyEncoder.class)`).
        // Synthesizes a `T(byte[] bytes)` ctor calling
        // `MyEncoder.decode(bytes)` and a `byte[] toBytes()` calling
        // `MyEncoder.encode(this)`. Mutually exclusive with
        // @BigEndian / @LittleEndian / @HostEndian / @Align — the
        // encoder owns the wire format. Phase A in v1 reserves the
        // surface + enforces mutual exclusion + emits "not yet
        // implemented" until Phase B lands the actual synthesis.
        // See docs/specification/reflect/Annotations.md § @Encoding for views.
        void synthesizeEncoding();

        // @Builder on class. Synthesizes a nested `Outer.Builder` class
        // (relies on the nested-class infrastructure landed alongside),
        // plus a `static Outer.Builder builder()` factory on Outer.
        // The Builder mirrors Outer's fields as private slots, adds a
        // chained setter per field (`Outer.Builder fieldName(T v)`
        // returning `this`), and provides `Outer build()` which
        // allocates Outer and calls its all-args ctor with the
        // accumulated field values.
        //
        // Implicit dependency: @Builder also triggers @AllArgsConstructor
        // synthesis on the outer (the build() body needs that ctor to
        // exist). synthesizeAllArgsConstructor checks for @Builder
        // alongside @AllArgsConstructor / @Value as triggers.
        void synthesizeBuilder();

        // @GenerateMock — generate a compile-time mock subclass `Mock<Name>`
        // of this (target) type for cajeta-unit's AoT mocking (no runtime
        // proxy). See specs/archive/mock-codegen-spec.md. Mirrors synthesizeBuilder.
        void synthesizeMock();

        // Field-level @Mock (M6) — for each `@Mock T field;` on this class,
        // rewrite the field's type to `Mock<T>` and synthesize a no-arg ctor that
        // auto-initializes it (`this.field = heap Mock<T>()`). Runs before ctor
        // synthesis so the init ctor becomes the class's no-arg ctor.
        void synthesizeMockFields();

        // Helper for the three ctor synthesizers: does the constructor
        // map already hold a ctor with `userArgs` user-visible params
        // (excluding `this`)?
        bool ctorWithArityExists(size_t userArgs) const;

        void createInheritanceMethodMap(CajetaClassPtr structure = shared_ptr<CajetaClass>(nullptr));


        void setRttiType(llvm::StructType* llvmRttiType) {
            this->rttiTypeRef() = llvmRttiType;
        }

        llvm::StructType* getRttiType() {
            return rttiTypeRef();
        }
    };
} // code