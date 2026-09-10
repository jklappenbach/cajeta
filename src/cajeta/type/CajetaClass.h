//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "CajetaType.h"
#include "StructureProperty.h"
#include "../method/Method.h"
#include "../error/Exception.h"
#include "Scope.h"
#include "Templates.h"

#include <vector>
#include <map>
#include <unordered_map>

namespace cajeta {
    class CajetaInterface;
    typedef shared_ptr<CajetaInterface> CajetaInterfacePtr;

    // Per-class codegen bindings bound to one LLVMContext (struct types, globals,
    // functions) and its emission flags; a frozen class keeps them per-thread.
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

    // Discriminator in word 2 of an interface fat pointer, set where the pointer is
    // built and read by the kind-tag drop dispatch in __cajeta_iface_drop.
    enum InterfaceValueKind : int64_t {
        IFACE_KIND_BORROWED_CLASS = 0,
        IFACE_KIND_OWNED_CLASS = 1
    };

    // Bytes in an interface fat pointer: data_ptr + vtable_ptr + kind_tag.
    constexpr unsigned IFACE_FAT_POINTER_BYTES = 24;

    class CajetaClass : public CajetaType {
    protected:
        map<string, MethodPtr> methods;
        // Mode-erased signature -> the raw key that claimed it. A second
        // declaration matching on erased but not raw is a mode-only overload.
        map<string, string> modeErasedMethodKeys;
        map<string, MethodPtr> staticMethods;

        map<string, map<string, MethodPtr>> labeledConstructorMap;
        map<string, map<string, MethodPtr>> unlabeledConstructorMap;

        map<string, map<string, MethodPtr>> labeledMethodMap;
        map<string, map<string, MethodPtr>> unlabeledMethodMap;

        list<MethodPtr> virtualMethodList;
        // Hash per virtualMethodList entry, in lockstep. For an interface entry it
        // is the INTERFACE method's hash, so an interface-typed receiver hits it.
        vector<int64_t> virtualSlotHashList;
        list<MethodPtr> methodList;
        map<string, StructurePropertyPtr> properties;
        list<StructurePropertyPtr> propertyList;
        list<QualifiedNamePtr> qExtended;
        list<QualifiedNamePtr> qImplemented;
        // Type arguments from `implements Foo<X, Y>`, parallel to qImplemented and
        // held as raw parse names; they resolve to types at validation time.
        list<vector<QualifiedNamePtr>> qImplementedTypeArgs;

        list<CajetaClassPtr> superClasses;
        list<CajetaInterfacePtr> interfaces;
        // Implemented interfaces as CajetaClassPtr, so vtable walks treat them as supertypes.
        list<CajetaClassPtr> implementedInterfaces;
        // Interfaces have no fields; their methods are abstract markers with no LLVM function.
        bool interfaceFlag = false;
        bool annotationFlag = false;
        // Forward-reference placeholder for a name known to the archive but not yet
        // declared; visitClassDeclaration fills it. One left set after parsing is a leak.
        bool placeholderFlag = false;
        // Set once generatePrototype ran; the deferred-prototype sweep's fixed-point marker.
        bool prototypeBuilt = false;
        // A still-placeholder interface was skipped; the codegen fixed point retries it.
        bool pendingIfaceVTables = false;
        bool recordType = false;
        // True while the declaration walk is in flight. A nested compile must not
        // prototype the class: the auto-default ctor would collide with the declared one.
        bool declWalkInFlight = false;
        CajetaModulePtr module;

        // declaringFile / declLine / declColumn live on CajetaType — enums need them too.

        // Read the module's current parse file into declaringFile. Captured at
        // construction: the stdlib parses every file into one module, so the
        // module cannot answer it later.
        void captureDeclaringFile();
        // Emit target for this class's own IR; null falls back to `module`. Set only
        // on the stdlib test-reuse path, so the cached stdlib module stays pristine.
        CajetaModulePtr emitModule;
        // "" outside a session redefinition, "$g2" / "$g3" / … after one. See symbolBase().
        string generationSuffix;
        ScopePtr scope;

        // Type parameters without arguments = an unmaterialized template, not a real
        // type; both = a concrete instantiation. templateSource is the declaration text.
        vector<TypeParameter> typeParameters;
        vector<CajetaTypePtr> typeArguments;
        string templateSource;
        int dbgLineDelta = 0;
        // A concrete instantiation's back-pointer to its template; null otherwise.
        CajetaClassPtr templateOrigin;

        // Explicitly null: writeVirtualTable's already-built guard must not read garbage.
        llvm::StructType* llvmVirtualTableType = nullptr;
        llvm::GlobalVariable* llvmVirtualTableGlobal = nullptr;
        llvm::StructType* llvmRttiType = nullptr;
        llvm::StructType* llvmReferenceType = nullptr;
        llvm::GlobalVariable* llvmRttiGlobal = nullptr;
        // Cached constant cajeta.reflect.Class instance { ptr Class#VTable, ptr rtti }.
        llvm::GlobalVariable* llvmClassObjectGlobal = nullptr;
        // Synthesized drop wrapper — see getOrCreateDropFunction.
        llvm::Function* llvmDropFunction = nullptr;
        // Stack-drop wrapper: the heap drop without the free, still releasing owned fields.
        llvm::Function* llvmStackDropFunction = nullptr;
        // True once the vtable's drop_fn slot points here, keeping the patch idempotent.
        bool llvmDropFunctionPatched = false;

        // Synthesized reflection invoke adapter, `void(ptr obj, i32 idx, ptr args,
        // ptr ret)`: declared with the #Rtti constant, body emitted post-quiescence.
        llvm::Function* llvmReflectInvokeFunction = nullptr;
        bool llvmReflectInvokeBodyEmitted = false;

        // Synthesized newInstance adapter, `ptr(i32 ctorIndex, ptr args)`, switching
        // over getReflectConstructorList; body emitted post-quiescence.
        llvm::Function* llvmReflectNewFunction = nullptr;
        bool llvmReflectNewBodyEmitted = false;

        // Per-(class, interface) vtables keyed by interface canonical: `[N x ptr]`, decl order.
        std::map<std::string, llvm::GlobalVariable*> interfaceVTables;

        // One global per static property, lazily defined in this class's home module.
        std::map<std::string, llvm::GlobalVariable*> staticFieldGlobals;

        // Per ancestor (self included), the LLVM slot where its sub-object starts in
        // THIS layout; self and a first parent are 0. Drives getSubObjectByteOffset.
        std::map<const CajetaClass*, int> subObjectSlotMap;

        // Materialized secondary vtables, keyed by the parent's canonical name.
        std::map<std::string, llvm::GlobalVariable*> secondaryVTables;

        // Snapshot of this class's MODULE-BOUND llvm bindings at stdlib prime;
        // restoreReuseBaseline() resets any that drifted into a per-test module.
        struct ReuseBindingBaseline {
            bool valid = false;
            // The emit module at prime; restoring it keeps callees out of a freed module.
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

        // vbase ABI: one `ptr` slot per transitive non-self ancestor after own fields,
        // pointing at its sub-object (a diamond repoints to the canonical one).
        std::vector<CajetaClassPtr> vbaseAncestors;
        std::map<const CajetaClass*, int> vbaseSlotMap;

        // Best match in `canonical` for `parameters`, scored by rank distance; null if none.
        MethodPtr getClosestMethod(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical);
        MethodPtr getClosestConstructor(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical);

    public:
        CajetaClass(CajetaModulePtr module) {
            this->module = module;
            scope = nullptr;
            captureDeclaringFile();
        }
        CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented);

        CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented);

        /** Reference struct for a class value: an owns-instance flag plus the pointer
         *  to the heap body, freed at scope end when owned. @return The reference type. */
        llvm::Type* getLlvmReferenceType();

        // True when `source` is this class or one of its transitive superclasses.
        bool isParentOrKind(CajetaClassPtr source);

        bool isInterface() const { return interfaceFlag; }
        void setIsInterface(bool v) { interfaceFlag = v; }
        // Annotation type (`annotation Foo {}`): a resolvable type token, never prototyped.
        bool isAnnotation() const { return annotationFlag; }
        void setIsAnnotation(bool v) { annotationFlag = v; }
        bool isPlaceholder() const { return placeholderFlag; }
        void setPlaceholder(bool v) { placeholderFlag = v; }

        bool isPrototypeBuilt() const { return prototypeBuilt; }

        // Run generatePrototype once no superclass or interface is still a placeholder,
        // else defer to the buildPendingPrototypes sweep. True if it ran. Idempotent.
        bool tryGeneratePrototype();

        // Class instances flow by pointer. Returns `ptr` while this class is an unfilled
        // placeholder, so earlier layouts get a sized slot; otherwise defers to the base.
        llvm::Type* getLlvmType() override;

        // Monotone count of placeholder fills: a prototype built against a placeholder
        // can carry the wrong ABI, and Method::ensureFreshPrototype re-derives when stale.
        static uint64_t& typeFillEpoch();

        // Fill an existing placeholder with the real declaration; references stay valid.
        void fillFromDeclaration(CajetaModulePtr m,
                                  QualifiedNamePtr q,
                                  list<QualifiedNamePtr> ext,
                                  list<QualifiedNamePtr> impl) {
            this->module = m;
            this->qName = q;
            this->qExtended = std::move(ext);
            this->qImplemented = std::move(impl);
            this->placeholderFlag = false;
            typeFillEpoch()++;
            // The placeholder captured the file of its first REFERENCE, not this one.
            captureDeclaringFile();
        }
        list<CajetaClassPtr>& getImplementedInterfaces() { return implementedInterfaces; }
        // The DECLARED parent names, before resolveSuperClasses() resolves them.
        const list<QualifiedNamePtr>& getQExtended() const { return qExtended; }
        const list<QualifiedNamePtr>& getQImplemented() const { return qImplemented; }
        void setQImplemented(list<QualifiedNamePtr> q) { qImplemented = std::move(q); }
        const list<vector<QualifiedNamePtr>>& getQImplementedTypeArgs() const {
            return qImplementedTypeArgs;
        }
        void setQImplementedTypeArgs(list<vector<QualifiedNamePtr>> a) {
            qImplementedTypeArgs = std::move(a);
        }
        // Resolve `qImplemented` (with its type args) into implementedInterfaces.
        void resolveImplementedInterfaces();

        ScopePtr getScope() { return scope; }

        // Register a method into every per-class map (statics, method list, labeled and
        // unlabeled method and ctor maps). Throws on a mode-only overload.
        void addMethod(MethodPtr method);
        // Fully unregister a method from every map addMethod populates — the stdlib
        // test-reuse path only, dropping a prior test's instantiation. No production caller.
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

        // Reparent an instantiation's emit target. Call BEFORE generatePrototype.
        void setModuleForInstantiation(CajetaModulePtr m) { module = m; }

        list<MethodPtr>& getMethodList() { return methodList; }

        // Reuse-cache only: capture snapshots this class's module-bound llvm bindings at
        // stdlib prime, restore resets any that drifted. No-ops in production.
        void captureReuseBaseline();
        void restoreReuseBaseline();

        CajetaModulePtr getModule() { return module; }

        // The module this class's IR is CREATED in; falls back to the resolution module.
        CajetaModulePtr getEmitModule() { return emitModule ? emitModule : module; }
        // True only when an emit target was explicitly assigned, not defaulted.
        bool hasEmitModuleOverride() const { return emitModule != nullptr; }
        void setEmitModule(CajetaModulePtr m) { emitModule = m; }

        // A session redefinition is a DIFFERENT type sharing a source name: qName stays
        // the source name while every emitted symbol derives from symbolBase().
        const string& getGenerationSuffix() const { return generationSuffix; }
        void setGenerationSuffix(const string& s) { generationSuffix = s; }
        // Deliberately uncached: qName is not final at construction, so a cache pins the wrong base.
        string symbolBase() {
            return generationSuffix.empty()
                ? qName->toCanonical()
                : qName->toCanonical() + generationSuffix;
        }

        list<CajetaClassPtr>& getSuperClasses() {
            // Lazy re-resolve: a named parent may have loaded after this class was
            // prototyped, leaving the link empty and cached. Idempotent.
            if (superClasses.size() < qExtended.size()) {
                resolveSuperClasses();
            }
            return superClasses;
        }

        // Fields inherited from every ancestor. The layout is { vtable, inherited, own },
        // so own fields start at 1 + countInheritedFields(). Recursive per parent.
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

        // vbase accessors.
        const std::vector<CajetaClassPtr>& getVbaseAncestors() const {
            return vbaseAncestors;
        }
        // Slot index of `ancestor`'s vbase pointer, or -1 when it has none here.
        int getVbaseSlotIndex(const CajetaClass* ancestor) const {
            if (!ancestor) return -1;
            auto it = vbaseSlotMap.find(ancestor);
            if (it == vbaseSlotMap.end()) return -1;
            return it->second;
        }

        // True when a field takes a bit in the hidden ownership word: a class-ref or
        // heap-array field. Shared-capable classes, views and value types are excluded.
        static bool fieldHasOwnershipBit(const StructurePropertyPtr& p);

        // True when a reference array's ELEMENTS carry per-slot ownership bits — a plain
        // vtable class element, released via __cajeta_class_virtual_drop.
        static bool arrayElementCarriesSlotBits(const CajetaTypePtr& elem);

        // True when the ELEMENT is itself a heap array (int8[][]), so a slot owns
        // a whole inner buffer, served by the __cajeta_tail_arrelem_* family.
        static bool arrayElementCarriesArraySlotBits(const CajetaTypePtr& elem);

        // Inner drop kind for an array-typed element at depth 1: 0 = plain free, 1 = the
        // inner array carries a class-element tail bitmap (walk, then free).
        static int arrayElementInnerDropKind(const CajetaTypePtr& elem);

        // True when the element is a value-type class carrying the hidden ownership word:
        // each inline slot replicates it, so teardown walks slots x members. No tail bitmap.
        static bool arrayElementCarriesMemberBits(const CajetaTypePtr& elem);

        // Synthesize (or fetch) `void <fn>(ptr hdr)`, the per-element member walk that
        // releases each slot's owned members. `withFree` fuses in __cajeta_free_array.
        static llvm::Function* getOrCreateMemberWalk(
            const CajetaModulePtr& module, llvm::Module* targetModule,
            const shared_ptr<CajetaClass>& elemCls, uint64_t headerBytes,
            uint64_t elemStride, bool withFree);

        // The llvm::Function behind a closure argument, IFF it is a directly-supplied (or
        // once-forwarded) non-capturing lambda; null for a captured or runtime closure.
        static llvm::Function* extractClosureTarget(
            llvm::Value* closureArg, llvm::Constant** outRecord = nullptr);

        // Dense bit index of `p` among this class's OWN bit-carrying fields, or -1.
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

        // LLVM slot of this class's hidden ownership word in its own layout, or -1.
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

        // Slot index of `prop` in this class's LLVM struct, walking the layout in the SAME
        // order generatePrototype builds it. -1 for a static property or a foreign one.
        virtual int getFieldLlvmIndex(const StructurePropertyPtr& prop) const {
            if (prop && prop->isStatic()) return -1;
            int slot = 0;
            int result = -1;
            std::function<void(const CajetaClass*, bool)> walk =
                [&](const CajetaClass* cls, bool ownVtable) {
                    if (result >= 0) return;
                    if (ownVtable) slot++;
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
                    // Skip the hidden ownership word — mirrors embedSubObject.
                    if (result < 0 && cls->needsOwnershipWord()) {
                        slot++;
                    }
                    // Skip cls's vbase pointer slots, appended after its own fields.
                    if (result < 0) {
                        slot += (int) cls->vbaseAncestors.size();
                    }
                };
            walk(this, /*ownVtable=*/hasVtablePointerAtSlotZero());
            return result;
        }

        // Byte offset of an ancestor's sub-object in this layout: 0 for self and the
        // first-parent chain. Call sites adjust `this` with it before a parent's IR.
        uint64_t getSubObjectByteOffset(const CajetaClass* ancestor) const;

        // Every non-first-parent sub-object in this layout, as
        // (ancestor, slot, byteOffset) — the secondary vptrs `new` must install.
        struct NonFirstSubObject {
            CajetaClassPtr ancestor;
            int slot;
            uint64_t byteOffset;
        };
        std::vector<NonFirstSubObject> getNonFirstSubObjects();

        // Secondary vtable for the (this class, parent) pair: parent's shape, this class's
        // overrides, through an offset thunk for a cross-class impl. Cached per pair.
        llvm::GlobalVariable* getOrCreateSecondaryVTable(
            CajetaClassPtr parent);

        // Shift a class value to the dstType sub-object when dstType is an ancestor at a
        // non-zero offset in srcType; returns srcValue unchanged otherwise.
        static llvm::Value* adjustForUpcast(
            CajetaModulePtr module,
            llvm::Value* srcValue,
            CajetaTypePtr srcType,
            CajetaTypePtr dstType);

        // Synthesize the this-offset thunk a cross-class secondary-vtable entry needs: it
        // subtracts `parentOffsetInThis` and tail-calls `impl`. Idempotent by symbol name.
        llvm::Function* synthesizeOffsetThunk(
            CajetaClassPtr parent,
            MethodPtr impl,
            uint64_t parentOffsetInThis);

        // Per-thread side-table of codegen bindings, used only while this class is frozen.
        // Keyed by class identity; thread_local, one context-bound set per compile thread.
        static std::unordered_map<const CajetaClass*, ClassLlvmBindings>& frozenClassBindings();

        // Frozen-aware accessors: they alias the inline members while frozen=false, and this
        // thread's side-table entry while frozen. The returned references stay stable.
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

        // Synthesize (or return the cached) per-class drop wrapper: `void(ptr instance)`
        // running the user drop() then __cajeta_free. Virtual — Task joins its fiber first.
        virtual llvm::Function* getOrCreateDropFunction();

        // Cached stack-drop wrapper: getOrCreateDropFunction's contract without the
        // trailing __cajeta_free, for a class local whose body the frame owns.
        llvm::Function* getOrCreateStackDropFunction();

        // True iff this class's stack drop would emit no calls (no user drop() here or on
        // an ancestor, no owned class-ref fields), so a stack local needs no drop entry.
        bool hasTrivialStackDrop();

        // True when a VALUE type is Utf8 or transitively embeds a shared-capable
        // value field: its copies retain and its drops release, per Shared stake.
        bool isSharedCapableValue();

        // Emit the recursive per-field retain (`retain`) or release walk over a
        // value of this type at `valuePtr`. Never traverses heap edges.
        void emitValueSharedOp(llvm::IRBuilder<>& b, llvm::Value* valuePtr,
                               CajetaModulePtr cajModule,
                               llvm::Module* bodyModule, bool retain);

        // Synthesized `void(ptr)` release wrapper for a shared-capable value local's slot.
        llvm::Function* getOrCreateValueReleaseFunction();

        // The DECLARATION of this class's reflective invoke adapter,
        // `void(ptr obj, i32 methodIndex, ptr args, ptr ret)`; the body lands later.
        llvm::Function* getOrCreateReflectInvokeDecl();

        // Emit the reflective invoke adapter's body (idempotent). Runs after the
        // codegen loop quiesces, when every method has an llvm::Function.
        void emitReflectInvokeBody();

        // This class's constructors sorted by canonical signature — the index
        // space the newInstance adapter switches over and #Rtti reports.
        std::vector<MethodPtr> getReflectConstructorList();

        // Declaration and body of the reflective newInstance adapter, mirroring
        // getOrCreateReflectInvokeDecl / emitReflectInvokeBody.
        llvm::Function* getOrCreateReflectNewDecl();
        void emitReflectNewBody();

        // Patch a deferred #ClassObject: a class parsed before cajeta.reflect.Class has a
        // null vtable in slot 0 and no registry entry. Fills and registers it once built.
        void finalizeClassObject();

        // Force-build the canonical wildcard instantiation Class<?> (idempotent). Runs
        // after prototyping, before codegen, so #ClassObject can embed its vtable.
        static void ensureClassWildcardInstantiated();

        // emitDropBodyInline emits the user `~Class()` then auto-drops OWN owned fields in
        // reverse order — no parent chain, no free; collectDestructorChain gives the order.
        void emitDropBodyInline(llvm::IRBuilder<>& b,
                                llvm::Value* instance,
                                CajetaModulePtr module);
        std::vector<CajetaClassPtr> collectDestructorChain();

        // Point the vtable's drop_fn slot at this class's heap-drop wrapper, so
        // __cajeta_class_virtual_drop reaches the dynamic type's destructor. Idempotent.
        void patchVirtualTableDropFn();

        // Lazily define (or fetch) the global backing static property `prop`, defined in
        // the home module; a differing `callerModule` gets an extern decl. Null if invalid.
        llvm::GlobalVariable* getOrCreateStaticFieldGlobal(
            StructurePropertyPtr prop,
            CajetaModulePtr callerModule = nullptr);

        // True iff the layout has a vtable pointer at slot 0, the convention
        // __cajeta_class_virtual_drop needs. @ValueType PODs and CajetaTask return false.
        virtual bool hasVtablePointerAtSlotZero() const { return !isValueType(); }

        // `record` declarations only, a subset of value types: immutable fields and
        // an intrinsic with(...). Plain @ValueType classes stay mutable.
        void setRecordType(bool v) { recordType = v; }
        bool isRecordType() const { return recordType; }
        void setDeclWalkInFlight(bool v) { declWalkInFlight = v; }
        bool isDeclWalkInFlight() const { return declWalkInFlight; }

        // Synthesize a per-(class, interface) vtable global for every implemented
        // interface: a flat `[N x ptr]` in interface-declaration order.
        void synthesizeInterfaceVTables();

        // The methods an interface's per-(impl, iface) vtable lays out, in vtable order:
        // own first, then parents by BFS. synthesizeInterfaceVTables and invokeMethod agree.
        std::vector<MethodPtr> getFlattenedInterfaceMethods();

        bool hasPendingIfaceVTables() const { return pendingIfaceVTables; }

        // The synthesized vtable global for `interfaceCanonical`, or null when not implemented.
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

        // Re-parent every member declaration in `classBody` to this class.
        void setClassBody(ClassBodyDeclarationPtr classBody);

        // Resolve `qExtended` into `superClasses` from the module's structures map: full
        // canonical first, then short type name (a bare identifier picks the wrong package).
        void resolveSuperClasses();

        // Single-pass hierarchy walk filling `virtualMethodList` in slot order; an override
        // replaces its ancestor's slot. Sets Method::virtualTableIndex. Idempotent.
        void buildVirtualTable();

        // Full vtable build: the slot list, then StructureMetadata::populate for the
        // LLVM vtable type and global. No-ops after the first success.
        void writeVirtualTable();

        // isTemplate(): type parameters declared and none bound — a recipe, not a
        // type. isInstantiation(): concrete arguments have been supplied.
        bool isTemplate() const { return !typeParameters.empty() && typeArguments.empty(); }
        bool isInstantiation() const { return !typeArguments.empty(); }

        // True iff any type argument is the wildcard sentinel (`Stream<?>`).
        bool isWildcardInstantiation() const;

        // True iff any argument is a BOUNDED wildcard (`? extends B` / `? super B`), an
        // abstract handle whose bodies are not codegen'd. Unbounded `?` is excluded.
        bool isBoundedWildcardInstantiation() const;

        // True iff `from`, a concrete instantiation, is assignable to `wildcardInst`. An
        // identity check on templateOrigin only — no super-chain walk yet.
        static bool isAssignableToWildcard(
            CajetaClassPtr from, CajetaClassPtr wildcardInst);

        // Numeric template-bound markers (Numeric/Floating/Integral/Complex): one source of
        // truth for the `<T extends M>` bound and wildcard admission. A primitive fits by flag.
        static bool isNumericMarkerName(const string& name);
        static bool satisfiesNumericMarker(CajetaTypePtr arg, const string& marker);

        // Materialize a concrete class from this template under `args`; idempotent and
        // cached, returning `this` when not a template. Defined in TemplateInstantiator.cpp.
        CajetaClassPtr instantiate(vector<CajetaTypePtr> args);
        // The actual instantiation logic; `instantiate` wraps it and also records
        // cross-module instantiation obligations for incremental compilation.
        CajetaClassPtr instantiateInternal(vector<CajetaTypePtr> args);

        // Deferred instantiation: instantiating from a still-placeholder template yields a
        // wrong class, so a placeholder is registered per canonical and filled in place.
        struct DeferredInstantiation {
            CajetaClassPtr templateClass;
            vector<CajetaTypePtr> args;
            CajetaClassPtr target;             // placeholder to fill in place
            string canonical;
        };
        static vector<DeferredInstantiation>& deferredInstantiations();
        // Complete every deferred instantiation whose template has materialized.
        // True if any completed, so the caller's fixpoint re-runs on progress.
        static bool drainDeferredInstantiations();
        // Set while a deferred instantiation is completing: the instantiation path
        // fills THIS object, preserving the identity earlier references captured.
        static CajetaClassPtr& instantiationReuseTarget();
        // Clear the deferred queue and the reuse target: both are thread_local shared_ptrs
        // into ONE compile's graph, and a stale entry resolves against the wrong registries.
        static void resetDeferredInstantiationState();


        // Diamond inference: the type-parameter bindings, in declaration order, for a
        // `new Box<>(args)` site. Throws CAJETA_ERROR_TYPE_INFERENCE when it cannot.
        vector<CajetaTypePtr> inferDiamondArgs(const vector<CajetaTypePtr>& argTypes);
        const vector<TypeParameter>& getTypeParameters() const { return typeParameters; }
        const vector<CajetaTypePtr>& getTypeArguments() const { return typeArguments; }
        void setTypeParameters(vector<TypeParameter> params) { typeParameters = std::move(params); }
        void setTypeArguments(vector<CajetaTypePtr> args) { typeArguments = std::move(args); }
        // Snippet-line to file-line correction for a template-instantiated body,
        // set at instantiation and read by safepoint emission.
        int getDbgLineDelta() const { return dbgLineDelta; }
        void setDbgLineDelta(int d) { dbgLineDelta = d; }

        const string& getTemplateSource() const { return templateSource; }
        void setTemplateSource(string src) { templateSource = std::move(src); }
        CajetaClassPtr getTemplateOrigin() const { return templateOrigin; }
        void setTemplateOrigin(CajetaClassPtr origin) { templateOrigin = std::move(origin); }

        // Invoke a resolved method. `sretTarget` is the caller-owned slot a value
        // return is built into; null materializes a temp. `errorIfUnresolved` is
        // opt-in — speculative probes (operator lookup) rely on the null return.
        llvm::Value* invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisInstance = nullptr,
                                   CajetaModulePtr callerModule = nullptr,
                                   bool forceDirectCall = false,
                                   const vector<CajetaTypePtr>& explicitMethodTypeArgs = {},
                                   llvm::Value* sretTarget = nullptr,
                                   llvm::Value* transferWord = nullptr,
                                   bool errorIfUnresolved = false,
                                   int callLine = -1, int callColumn = -1);

        // Diagnostic for a named member that did not resolve: MEMBER_NOT_FOUND for
        // an unknown name, NO_MATCHING_OVERLOAD (with candidates) for a bad match.
        Exception memberNotFoundException(const string& methodName,
            const vector<ParameterEntry>& parameters, int line, int column);

        // The nearest real member name to `typo` (methods and properties, this
        // class and ancestors), or "" when nothing is close. Never offers `__` members.
        string suggestMemberName(const string& typo);

        // Zero-init `instance` of type `structTy` and install the slot-0 vtable, any
        // secondary sub-object vtables and, for a heap instance, the virtual drop-fn patch.
        void initInstanceLayout(CajetaModulePtr module, llvm::Value* instance,
                                llvm::Type* structTy, bool stackAlloc);
        // malloc + initInstanceLayout + the constructor matching `entries`, returning the
        // new instance. No scope drop is registered — the caller owns the lifecycle.
        llvm::Value* heapConstruct(CajetaModulePtr module,
                                   vector<ParameterEntry>& entries);

        // Resolve a callee on this class or an ancestor (re-keyed at each level);
        // null when not found. `explicitMethodTypeArgs` supplies args inference
        // cannot reach. Wraps resolveMethodImpl so the xref edge is noted once.
        MethodPtr resolveMethod(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs = {},
            CajetaModulePtr activeModule = nullptr);

        // The recording half of resolveMethod, callable alone so the lint path
        // records edges through the same computation. Takes a resolved callee.
        static void noteResolvedCallXref(const MethodPtr& resolved,
            bool isConstructor, CajetaModulePtr activeModule);

    private:
        MethodPtr resolveMethodImpl(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs,
            CajetaModulePtr activeModule);
    public:

        // Register + prototype + generate a method-template instantiation as a call site
        // would; incremental replay needs it, since a skipped module's call sites never fire.
        void ensureMethodInstantiationAlive(MethodPtr inst);

        // Reorder a mixed positional/named call into formal order and strip the labels;
        // true when it was partial. Throws on a bad mix, a duplicate label, or no match.
        bool normalizePartialLabeledCall(const string& methodName, bool isConstructor,
                                         vector<ParameterEntry>& parameters);

        // Lay this class out: instance struct, vtable, methods, synthesized members.
        // Idempotent, and deferred while any parent is still a placeholder.
        virtual void generatePrototype();

        // Pure instance-struct layout (members, setBody, sub-object and vbase slot maps),
        // so a frozen class can rebuild its body in a thread's own LLVMContext.
        void buildInstanceStructBody(llvm::LLVMContext* lctx);

        // Emit every method body, then the static-field initializers.
        virtual void generateCode();

        // Emit a clinit-style function for any static-field initializer that did not
        // constant-fold, registered with llvm.global_ctors. No-op when every one folded.
        void generateStaticInitializers();

        void generateMetadata();

        // Add the synthesized default constructor unless a constructor is already
        // declared (asked of the constructor map, which `methods` cannot answer).
        void ensureDefaultConstructor();

        // Inject a structural hash() override when the class carries @AutoHash and declares
        // none. Runs after ensureDefaultConstructor; SynthesizedHashMethod walks the fields.
        void synthesizeAutoHash();

        // Lombok-mirror synthesizers: each is gated on an annotation, runs during
        // generatePrototype, and yields to a user method of the same name and arity.

        // @Getter on the class or a field: synthesizes `public T <fieldName>()`.
        void synthesizeGetters();

        // @Setter on the class or a field: synthesizes
        // `public void <fieldName>(T v)`. Skipped for a `final` field.
        void synthesizeSetters();

        // @ToString: synthesizes `public String toString()` returning
        // `ClassName(field1=v1,...)`, omitting @ToString.Exclude fields.
        void synthesizeToString();

        // @NoArgsConstructor / @AllArgsConstructor / @RequiredArgsConstructor — synthesize
        // the ctor unless a same-shape one exists (user wins). No implicit super-ctor chain.
        void synthesizeNoArgsConstructor();
        void synthesizeAllArgsConstructor();
        void synthesizeRequiredArgsConstructor();

        // Shared helper for the three ctor synthesizers: adds the ctor and, when the
        // annotation carries `staticName`, a static factory (the ctor turns PRIVATE).
        void emitCtorAndOptionalFactory(
            const AnnotationInstancePtr& ann,
            const std::string& annotationName,
            vector<StructurePropertyPtr> fields,
            Modifier access);

        // @With on the class or a field: synthesizes `withX(T v)`, which memcpys the source
        // body into a fresh instance, overwrites the named slot, and returns it.
        void synthesizeWith();

        // @Encoding(MyEncoder.class): synthesizes a `T(byte[] bytes)` ctor and a
        // `byte[] toBytes()`. Mutually exclusive with the endian and @Align annotations.
        void synthesizeEncoding();

        // @Builder: synthesizes the nested `Outer.Builder` (field slots, chained setters,
        // `Outer build()`) plus `static Builder builder()`. Also triggers @AllArgsConstructor.
        void synthesizeBuilder();

        // @GenerateMock — generate the compile-time mock subclass `Mock<Name>` of
        // this type for cajeta-unit's AoT mocking (no runtime proxy).
        void synthesizeMock();

        // Field-level @Mock: rewrite each `@Mock T field` to `Mock<T>` and synthesize a
        // no-arg ctor initializing it. Runs before ctor synthesis, so it becomes the no-arg.
        void synthesizeMockFields();

        // Does a constructor with `userArgs` user-visible params already exist?
        bool ctorWithArityExists(size_t userArgs) const;

        // Index every method of this class and its ancestors into the labeled and
        // unlabeled method maps, parents first, so an override wins the key.
        void createInheritanceMethodMap(CajetaClassPtr structure = shared_ptr<CajetaClass>(nullptr));


        void setRttiType(llvm::StructType* llvmRttiType) {
            this->rttiTypeRef() = llvmRttiType;
        }

        llvm::StructType* getRttiType() {
            return rttiTypeRef();
        }
    };
} // code