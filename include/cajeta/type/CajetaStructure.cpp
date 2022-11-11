//
// Created by James Klappenbach on 10/24/22.
//

#include <cajeta/type/CajetaStructure.h>
#include <cajeta/type/Field.h>
#include <cajeta/type/Method.h>
#include <cajeta/asn/ClassBodyDeclaration.h>

namespace cajeta {
    CajetaStructure::CajetaStructure(CajetaModule* module, QualifiedName* qName) : CajetaType(qName) {
            llvmType = llvm::StructType::create(*module->getLlvmContext(), qName->getTypeName());
            scope = new Scope(module);
    }

    void CajetaStructure::addField(Field* field) {
        this->fields[field->getName()] = field;
    }
    void CajetaStructure::addFields(list<Field*> fields) {
        for (Field* field : fields) {
            this->fields[field->getName()] = field;
        }
    }
    void CajetaStructure::addMethod(Method* method) {
        this->methods[method->getName()] = method;
    }
    void CajetaStructure::addMethods(list<Method*> methods) {
        for (Method* method : methods) {
            this->methods[method->getName()] = method;
        }
    }

    void CajetaStructure::generateSignature(CajetaModule* module) {
        vector<llvm::Type*> llvmMembers;
        for (auto &fieldEntry : fields) {
            llvmMembers.push_back(fieldEntry.second->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        ensureDefaultConstructor(module);

        for (auto methodEntry : methods) {
            methodEntry.second->generatePrototype(module);
        }
    }

    void CajetaStructure::ensureDefaultConstructor(CajetaModule* module) {
        string name = qName->getTypeName();
        if (methods.find(name) == methods.end()) {
            methods[name] = new Method(name, CajetaType::of("void"), this);
        }
    }


    void CajetaStructure::setClassBody(cajeta::ClassBodyDeclaration* classBody) {
        for (auto memberDeclaration : classBody->getDeclarations()) {
            memberDeclaration->updateParent(this);
        }
        delete classBody;
    }

    void CajetaStructure::generateCode(CajetaModule* module) {
        module->setCurrentStructure(this);
        for (auto methodEntry : methods) {
            methodEntry.second->generateCode(module);
        }
    }

} // cajeta