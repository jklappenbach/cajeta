//
// Created by James Klappenbach on 10/24/22.
//

#include <cajeta/type/CajetaStructure.h>
#include <cajeta/type/Field.h>
#include "cajeta/method/Method.h"
#include <cajeta/asn/ClassBodyDeclaration.h>
#include <cajeta/method/DefaultConstructorMethod.h>

namespace cajeta {
    llvm::StructType* CajetaStructure::llvmRttiType = nullptr;

    CajetaStructure::CajetaStructure(CajetaModule* module, QualifiedName* qName) : CajetaType(qName) {
        this->module = module;
    }

    void CajetaStructure::addMethod(Method* method) {
        methods[method->getName()] = method;
        methodList.push_back(method);
    }

    void CajetaStructure::addMethods(list<Method*> methods) {
        for (Method* method: methods) {
            this->methods[method->getName()] = method;
            methodList.push_back(method);
        }
    }

    void CajetaStructure::addProperty(ClassProperty* field) {
        properties[field->getName()] = field;
        propertyList.push_back(field);
    }

    void CajetaStructure::generatePrototype() {
        llvmType = llvm::StructType::create(*module->getLlvmContext(), qName->toCanonical());
        scope = new Scope(module);

        vector<llvm::Type*> llvmMembers;
        for (auto& fieldEntry: properties) {
            llvmMembers.push_back(fieldEntry.second->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        ensureDefaultConstructor();

        for (auto methodEntry: methods) {
            methodEntry.second->generatePrototype();
        }
    }

    void CajetaStructure::ensureDefaultConstructor() {
        string name = qName->getTypeName();
        if (methods.find(name) == methods.end()) {
            addMethod(new DefaultConstructorMethod(module, this));
        }
    }

    void CajetaStructure::setClassBody(cajeta::ClassBodyDeclaration* classBody) {
        for (auto memberDeclaration: classBody->getDeclarations()) {
            memberDeclaration->updateParent(this);
        }
        delete classBody;
    }

    void CajetaStructure::generateCode() {
        for (auto& method: methodList) {
            method->generateCode();
        }
    }

    void CajetaStructure::invokeMethod(string& methodName, llvm::Value* allocation) {
        Method* method = methods[methodName];
        module->addToGenerate(method);
        vector<llvm::Value*> args;
        args.push_back(allocation);
        llvm::CallInst::Create(method->getLlvmFunctionType(), method->getLlvmFunction(), args, "",
            module->getBuilder()->GetInsertBlock());
    }

    /**
     * Structure of metadata:
     * MetaData[]
     * First, start with the root level classes and work our way up:
     * - ([2B String Length] + [Canonical Class Name UTF8])
     * - Number of methods * ([8B Method Ptr] + [2B String Length] + [Method Name UTF8] + [2B Argument Count] + (A)
     *
     * @param module
     */
    void CajetaStructure::generateMetadata() {
        //module->getLlvmModule()->getOrInsertGlobal();
    }
} // cajeta