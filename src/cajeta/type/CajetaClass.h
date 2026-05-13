//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "CajetaType.h"
#include "StructureProperty.h"
#include "../method/Method.h"
#include "Scope.h"
#include "Generics.h"

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
        list<MethodPtr> methodList;
        map<string, StructurePropertyPtr> properties;
        list<StructurePropertyPtr> propertyList;
        list<QualifiedNamePtr> qExtended;
        list<QualifiedNamePtr> qImplemented;

        list<CajetaClassPtr> superClasses;
        list<CajetaInterfacePtr> interfaces;
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
        virtual int getFieldLlvmIndex(const StructurePropertyPtr& prop) const {
            return prop->getOrder() + 1;
        }

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
        // have been supplied. See Generics.h and CajetaClass::instantiate.
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

        llvm::Value* invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisInstance = nullptr);

        // Look up a method on this class or any of its ancestors. Each class
        // indexes methods under keys that embed its own class name, so the
        // recursion re-keys at each level. Returns nullptr if not found.
        MethodPtr resolveMethod(string& methodName, vector<ParameterEntry>& parameters, bool isConstructor, bool floatingParams);

        virtual void generatePrototype();

        virtual void generateCode();

        void generateMetadata();

        void ensureDefaultConstructor();

        void createInheritanceMethodMap(CajetaClassPtr structure = shared_ptr<CajetaClass>(nullptr));


        void setRttiType(llvm::StructType* llvmRttiType) {
            this->llvmRttiType = llvmRttiType;
        }

        llvm::StructType* getRttiType() {
            return llvmRttiType;
        }
    };
} // code