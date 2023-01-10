//
// Created by James Klappenbach on 10/24/22.
//

#include "CajetaStructure.h"
#include "../field/Field.h"
#include "../method/Method.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../method/DefaultConstructorMethod.h"
#include "../field/HeapField.h"

using namespace std;

namespace cajeta {
    CajetaStructure::CajetaStructure(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented) : CajetaType(qName) {
        this->qImplemented = qImplemented;
        this->module = module;
    }
    CajetaStructure::CajetaStructure(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented)
            : CajetaType(qName) {
        this->qExtended = qExtended;
        this->qImplemented = qImplemented;
        this->module = module;
    }

    llvm::Type* CajetaStructure::getLlvmReferenceType() {
        if (llvmReferenceType == nullptr) {
            vector<llvm::Type*> types;
            types.push_back(llvm::Type::getInt1Ty(*module->getLlvmContext()));
            types.push_back(llvmType->getInt64PtrTy(*module->getLlvmContext()));
            llvmReferenceType = llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef<llvm::Type*>(types));
        }
        return llvmReferenceType;
    }

    bool CajetaStructure::isParentOrKind(CajetaStructurePtr source) {
        if (source->getQName()->operator==(qName)) {
            return true;
        }
        for (auto& parent : superClasses) {
            if (parent->isParentOrKind(source)) {
                return true;
            }
        }
        return false;
    }

    int getMethodCount(map<string, map<string, MethodPtr>>& map) {
        int count = 0;
        for (auto& entry : map) {
            count += entry.second.size();
        }
        return count;
    }

    void mapMethod(MethodPtr method, map<string, map<string, MethodPtr>>& map, bool labeled) {
        string generic = method->toGeneric(labeled);
        string canonical = method->toCanonical(labeled);

        auto itrGeneric = map.find(generic);
        if (itrGeneric != map.end()) {
            auto itrExact = (*itrGeneric).second.find(canonical);
            if (itrExact != (*itrGeneric).second.end()) {
                method->setVirtualTableIndex((*itrExact).second->getVirtualTableIndex());
            } else {
                int id = getMethodCount(map);
                method->setVirtualTableIndex(id);
            }
            map[generic][canonical] = method;
        } else {
            int id = getMethodCount(map);
            map[generic][canonical] = method;
            method->setVirtualTableIndex(id);
        }
    }

    void CajetaStructure::addMethod(MethodPtr method) {
        methods[method->toCanonical()] = method;

        if (method->isConstructor()) {
            map<string, MethodPtr> canonical = unlabeledConstructorMap[method->toGeneric(false)];
            if (canonical.find(method->toCanonical(false)) != canonical.end()) {
                throw "Constructor already exists";
            }
            mapMethod(method, labeledConstructorMap, true);
            mapMethod(method, unlabeledConstructorMap, false);
        } else {
            if (method->isStatic()) {
                map<string, MethodPtr> canonical = unlabeledMethodMap[method->toGeneric(false)];
                if (canonical.find(method->toCanonical(false)) != canonical.end()) {
                    throw "A static method with this signature already exists.  Static methods can not be overridden.";
                }
                staticMethods[method->toCanonical()] = method;
            }
            methodList.push_back(method);
            methods[method->toCanonical()] = method;
            mapMethod(method, labeledMethodMap, true);
            mapMethod(method, unlabeledMethodMap, false);
        }
    }

    void CajetaStructure::addMethods(list<MethodPtr> methods) {
        for (MethodPtr method: methods) {
            addMethod(method);
        }
    }

    void CajetaStructure::addProperty(StructurePropertyPtr field) {
        properties[field->getName()] = field;
        propertyList.push_back(field);
    }

    void CajetaStructure::generatePrototype() {
        string canonical = qName->toCanonical();

        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
        typeMap[TypeKey(llvmType)] = shared_from_this();
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));
        vector<llvm::Type*> llvmMembers;
        for (auto& propertyEntry: properties) {
            llvmMembers.push_back(propertyEntry.second->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        ensureDefaultConstructor();

        for (auto methodEntry: methods) {
            methodEntry.second->generatePrototype();
        }
        CajetaModule::getGlobalStructures()[canonical] = module;
    }

    void CajetaStructure::ensureDefaultConstructor() {
        string name = qName->getTypeName();
        if (methods.find(name) == methods.end()) {
            addMethod(make_shared<DefaultConstructorMethod>(module, static_pointer_cast<CajetaStructure>(shared_from_this())));
        }
    }

    void CajetaStructure::setClassBody(cajeta::ClassBodyDeclarationPtr classBody) {
        for (auto memberDeclaration: classBody->getDeclarations()) {
            memberDeclaration->updateParent(static_pointer_cast<CajetaStructure>(shared_from_this()));
        }
    }

    void CajetaStructure::generateCode() {
        for (auto& method: methodList) {
            method->generateCode();
        }
    }

    struct MethodEntry {
        MethodPtr method;
        int score;
        MethodEntry(MethodPtr method) { this->method = method; score = 0; }
    };

    MethodPtr CajetaStructure::getClosestMethod(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical) {
        vector<MethodEntry> entries;

        for (auto& entry : canonical) {
            MethodPtr method = entry.second;
            map<string, FormalParameterPtr> methodParameters = method->getParameters();
            bool valid = true;
            MethodEntry methodEntry(method);
            for (auto& parameter : parameters) {
                if (methodParameters.find(parameter.label) != methodParameters.end()) {
                    int score = methodParameters[parameter.label]->getType()->getRank() - parameter.type->getRank();
                    if (score < 0) {
                        valid = false;
                        break;
                    } else {
                        methodEntry.score += score;
                    }
                }
            }
            if (valid) {
                entries.push_back(methodEntry);
            }
        }
        if (entries.empty()) {
            return nullptr;
        }
        sort(entries.begin(), entries.end(), [](const MethodEntry& a, const MethodEntry& b) { return a.score < b.score; });
        return entries[0].method;
    }

    // TODO: Need to call this before addMethods are called, when any parent classes are loaded.  Rewrite based on parent classes already having their methods mapped.
    void CajetaStructure::createInheritanceMethodMap(CajetaStructurePtr structure) {
        if (structure == nullptr) {
            structure = static_pointer_cast<CajetaStructure>(shared_from_this());
        }

        for (auto& superClass: structure->getSuperClasses()) {
            createInheritanceMethodMap(superClass);
        }

        for (auto& method: structure->getMethodList()) {
            mapMethod(method, labeledMethodMap, true);
            mapMethod(method, unlabeledMethodMap, false);
        }
    }

    void CajetaStructure::invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisValue) {
        MethodPtr method;
        vector<CajetaTypePtr> types;
        bool floatingParams = true;
        for (auto &param : parameters) {
            if (param.label.empty()) {
                floatingParams &= false;
            }
        }

        if (floatingParams) {
            sort(parameters.begin(), parameters.end(), [](const ParameterEntry& a, const ParameterEntry& b) -> bool { return a.label < b.label; });
        }

        string generic = Method::buildGeneric(static_pointer_cast<CajetaStructure>(shared_from_this()), methodName, parameters, floatingParams);
        string canonical = Method::buildCanonical(static_pointer_cast<CajetaStructure>(shared_from_this()), methodName, parameters, floatingParams);

        map<string, map<string, MethodPtr>>* genericMap;

        if (isConstructor) {
            genericMap = floatingParams ? &labeledConstructorMap : &unlabeledConstructorMap;
        } else {
            genericMap = floatingParams ? &labeledMethodMap : &unlabeledMethodMap;
        }

        if (genericMap->find(generic) != genericMap->end()) {
            map<string, MethodPtr> canonicalMap = (*genericMap)[generic];
            if (canonicalMap.find(canonical) != canonicalMap.end()) {
                method = canonicalMap[canonical];
            } else {
                method = getClosestMethod(methodName, parameters, canonicalMap);
            }
        } else {
            // throw "Method not found.";
        }
        vector<llvm::Value*> methodArgs;
        for (int i = 0; i < parameters.size(); i++) {
            methodArgs.push_back(parameters[i].value);
        }
        // TODO: enable this after fixing bugs
        // this->module->getBuilder()->CreateCall(method->getLlvmFunctionType(), method->getLlvmFunction(), llvm::ArrayRef<llvm::Value*>(methodArgs));
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