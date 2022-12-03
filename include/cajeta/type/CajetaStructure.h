//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/ClassProperty.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/Scope.h"

namespace cajeta {
    class CajetaInterface;

    class ClassBodyDeclaration;

    class CajetaModule;

    class CajetaStructure : public CajetaType {
    protected:
        map<string, Method*> methods;
        list<Method*> methodList;
        map<string, ClassProperty*> properties;
        list<ClassProperty*> propertyList;
        list<CajetaStructure*> superClasses;
        list<CajetaInterface*> interfaces;
        CajetaModule* module;
        Scope* scope;
        llvm::StructType* llvmVirtualTableType;
        llvm::GlobalVariable* llvmVirtualTableGlobal;
        static llvm::StructType* llvmRttiType;
        llvm::GlobalVariable* llvmRttiGlobal;
    public:
        CajetaStructure(CajetaModule* module) {
            this->module = module;
            scope = nullptr;
        }

        CajetaStructure(CajetaModule* module, QualifiedName* qName);

        Scope* getScope() { return scope; }

        void addMethod(Method* method);

        void addMethods(list<Method*> methods);

        void addProperty(ClassProperty* field);

        map<string, ClassProperty*>& getProperties() { return properties; }

        list<ClassProperty*>& getPropertyList() { return propertyList; }

        map<string, Method*>& getMethods() { return methods; }

        list<Method*>& getMethodList() { return methodList; }

        list<CajetaStructure*>& getSuperClasses() { return superClasses; }

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

        void setClassBody(ClassBodyDeclaration* classBody);

        void writeVirtualTable();

        virtual void invokeMethod(string& methodName, llvm::Value* allocation);

        virtual void generatePrototype();

        virtual void generateCode();

        void generateMetadata();

        void ensureDefaultConstructor();

        static void setRttiType(llvm::StructType* llvmRttiType) {
            CajetaStructure::llvmRttiType = llvmRttiType;
        }

        static llvm::StructType* getRttiType() {
            return CajetaStructure::llvmRttiType;
        }
    };

} // cajeta