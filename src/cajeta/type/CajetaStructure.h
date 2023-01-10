//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "CajetaType.h"
#include "StructureProperty.h"
#include "../method/Method.h"
#include "Scope.h"

namespace cajeta {
    class CajetaInterface;
    typedef shared_ptr<CajetaInterface> CajetaInterfacePtr;

    class ClassBodyDeclaration;
    typedef shared_ptr<ClassBodyDeclaration> ClassBodyDeclarationPtr;

    class CajetaModule;
    typedef shared_ptr<CajetaModule> CajetaModulePtr;

    class CajetaStructure;
    typedef shared_ptr<CajetaStructure> CajetaStructurePtr;

    class CajetaStructure : public CajetaType {
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

        list<CajetaStructurePtr> superClasses;
        list<CajetaInterfacePtr> interfaces;
        CajetaModulePtr module;
        ScopePtr scope;

        llvm::StructType* llvmVirtualTableType;
        llvm::GlobalVariable* llvmVirtualTableGlobal;
        llvm::StructType* llvmRttiType;
        llvm::StructType* llvmReferenceType;
        llvm::GlobalVariable* llvmRttiGlobal;

        MethodPtr getClosestMethod(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical);
        MethodPtr getClosestConstructor(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical);

    public:
        CajetaStructure(CajetaModulePtr module) {
            this->module = module;
            scope = nullptr;
        }
        CajetaStructure(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented);

        CajetaStructure(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented);

        /**
         * Create a structure that provides for a boolean type for whether the reference owns the instance and should delete at scope-end,
         * as well as a pointer to the struct, which was allocated from the heap.
         * @return The type defining the reference.
         */
        llvm::Type* getLlvmReferenceType();

        bool isParentOrKind(CajetaStructurePtr source);

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

        list<CajetaStructurePtr>& getSuperClasses() { return superClasses; }

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

        void writeVirtualTable();

        void invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisInstance = nullptr);

        virtual void generatePrototype();

        virtual void generateCode();

        void generateMetadata();

        void ensureDefaultConstructor();

        void createInheritanceMethodMap(CajetaStructurePtr structure = shared_ptr<CajetaStructure>(nullptr));


        void setRttiType(llvm::StructType* llvmRttiType) {
            this->llvmRttiType = llvmRttiType;
        }

        llvm::StructType* getRttiType() {
            return llvmRttiType;
        }
    };
} // cajeta