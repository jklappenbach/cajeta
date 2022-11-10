//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/Field.h"
#include "cajeta/type/Method.h"
#include "cajeta/type/Scope.h"

namespace cajeta {

    class ClassBodyDeclaration;

    class CajetaStructure : public CajetaType {
    private:
        map<string, Method*> methods;
        map<string, Field*> fields;
        CajetaModule* module;
        Scope* scope;
    public:
        CajetaStructure(llvm::LLVMContext* llvmContext, QualifiedName* qName) : CajetaType(qName) {
            llvmType = llvm::StructType::create(*llvmContext, qName->getTypeName());
            scope = new Scope();
        }
        virtual bool isPrimitive() { return false; }
        Scope* getScope() { return scope; }
        void addField(Field* field);
        void addFields(list<Field*> fields);
        void addMethod(Method* method);
        void addMethods(list<Method*> methods);
        void setClassBody(ClassBodyDeclaration* classBody);

        void generateSignature(CajetaModule* module) {
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
        void generateCode(CajetaModule* module);

        void ensureDefaultConstructor(CajetaModule* module) {
            string name = qName->getTypeName();
            if (methods.find(name) == methods.end()) {
                methods[name] = new Method(name, CajetaType::of("void"), this);
            }
        }

        map<string, Field*>& getFields() { return fields; }
        map<string, Method*>& getMethods() { return methods; }
    };

} // cajeta