//
// Created by James Klappenbach on 10/24/22.
//

#include <cajeta/type/CajetaStructure.h>
#include <cajeta/type/Field.h>
#include "cajeta/method/Method.h"
#include <cajeta/asn/ClassBodyDeclaration.h>
#include <cajeta/method/DefaultConstructorMethod.h>
#include <cajeta/method/DefaultDestructorMethod.h>

namespace cajeta {
    llvm::StructType* CajetaStructure::llvmRttiType = nullptr;

    CajetaStructure::CajetaStructure(QualifiedName* qName) : CajetaType(qName) { }

    void CajetaStructure::addMethod(Method* method) {
        methods[method->getName()] = method;
        methodList.push_back(method);
    }

    void CajetaStructure::addMethods(list<Method*> methods) {
        for (Method* method : methods) {
            this->methods[method->getName()] = method;
            methodList.push_back(method);
        }
    }

    void CajetaStructure::addField(StructureField* field) {
        fields[field->getName()] = field;
        fieldList.push_back(field);
    }

    void CajetaStructure::generatePrototype(CajetaModule* module) {
        llvmType = llvm::StructType::create(*module->getLlvmContext(), qName->toCanonical());
        scope = new Scope(module);

        vector<llvm::Type*> llvmMembers;
        for (auto &fieldEntry : fields) {
            llvmMembers.push_back(fieldEntry.second->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        ensureDefaultConstructor(module);
        ensureDefaultDestructor(module);

        for (auto methodEntry : methods) {
            methodEntry.second->generatePrototype(module);
        }
    }

    void CajetaStructure::ensureDefaultConstructor(CajetaModule* module) {
        string name = qName->getTypeName();
        if (methods.find(name) == methods.end()) {
            addMethod(new DefaultConstructorMethod(this));
        }
    }

    void CajetaStructure::ensureDefaultDestructor(CajetaModule* module) {
        string name = "~";
        name.append(qName->getTypeName());
        if (methods.find(name) == methods.end()) {
            addMethod(new DefaultDestructorMethod(this));
        }
    }

    void CajetaStructure::setClassBody(cajeta::ClassBodyDeclaration* classBody) {
        for (auto memberDeclaration : classBody->getDeclarations()) {
            memberDeclaration->updateParent(this);
        }
        delete classBody;
    }

    void CajetaStructure::generateCode(CajetaModule* module) {
        for (auto &method : methodList) {
            method->generateCode(module);
        }
    }

    void CajetaStructure::invokeMethod(string& methodName, llvm::Value* allocation, CajetaModule* module) {
        Method* method = methods[methodName];
        vector<llvm::Value*> args;
        args.push_back(allocation);
        llvm::CallInst::Create(method->getLlvmFunctionType(), method->getLlvmFunction(), args, "", module->getBuilder()->GetInsertBlock());
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
    void CajetaStructure::generateMetadata(cajeta::CajetaModule* module) {
        //module->getLlvmModule()->getOrInsertGlobal();
    }
} // cajeta