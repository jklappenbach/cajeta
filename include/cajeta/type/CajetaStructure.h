//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/StructureField.h"
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
        map<string, StructureField*> fields;
        list<StructureField*> fieldList;
        list<CajetaStructure*> superClasses;
        list<CajetaInterface*> interfaces;
        Scope* scope;
        llvm::StructType* llvmVirtualTableType;
        llvm::GlobalVariable* llvmVirtualTableGlobal;
        static llvm::StructType* llvmRttiType;
        llvm::GlobalVariable* llvmRttiGlobal;
    public:
        CajetaStructure() {
            scope = nullptr;
        }
        CajetaStructure(QualifiedName* qName);
        Scope* getScope() { return scope; }
        void addMethod(Method* method);
        void addMethods(list<Method*> methods);
        void addField(StructureField* field);
        map<string, StructureField*>& getFields() { return fields; }
        list<StructureField*>& getFieldList() { return fieldList; }
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
        void writeVirtualTable(CajetaModule* module);
        virtual void invokeMethod(string& methodName, llvm::Value* allocation, CajetaModule* module);
        virtual void generatePrototype(CajetaModule* module);
        virtual void generateCode(CajetaModule* module);
        void generateMetadata(CajetaModule* module);
        void ensureDefaultConstructor(CajetaModule* module);
        virtual void ensureDefaultDestructor(CajetaModule* module);
        static void setRttiType(llvm::StructType* llvmRttiType) {
            CajetaStructure::llvmRttiType = llvmRttiType;
        }
        static llvm::StructType* getRttiType() {
            return CajetaStructure::llvmRttiType;
        }
    };

} // cajeta