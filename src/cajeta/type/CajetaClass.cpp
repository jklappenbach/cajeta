//
// Created by James Klappenbach on 10/24/22.
//

#include "CajetaClass.h"
#include "StructureMetadata.h"
#include "../field/Field.h"
#include "../method/Method.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../method/DefaultConstructorMethod.h"
#include "../field/HeapField.h"

#include <functional>

using namespace std;

namespace cajeta {
    CajetaClass::CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented) : CajetaType(qName) {
        this->qImplemented = qImplemented;
        this->module = module;
    }
    CajetaClass::CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented)
            : CajetaType(qName) {
        this->qExtended = qExtended;
        this->qImplemented = qImplemented;
        this->module = module;
    }

    llvm::Type* CajetaClass::getLlvmReferenceType() {
        if (llvmReferenceType == nullptr) {
            vector<llvm::Type*> types;
            types.push_back(llvm::Type::getInt1Ty(*module->getLlvmContext()));
            types.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
            llvmReferenceType = llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef<llvm::Type*>(types));
        }
        return llvmReferenceType;
    }

    bool CajetaClass::isParentOrKind(CajetaClassPtr source) {
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

    void CajetaClass::addMethod(MethodPtr method) {
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

    void CajetaClass::addMethods(list<MethodPtr> methods) {
        for (MethodPtr method: methods) {
            addMethod(method);
        }
    }

    void CajetaClass::addProperty(StructurePropertyPtr field) {
        properties[field->getName()] = field;
        propertyList.push_back(field);
    }

    void CajetaClass::generatePrototype() {
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
        CajetaModule::getStructureToModule()[canonical] = module;
    }

    void CajetaClass::ensureDefaultConstructor() {
        string name = qName->getTypeName();
        if (methods.find(name) == methods.end()) {
            addMethod(make_shared<DefaultConstructorMethod>(module, static_pointer_cast<CajetaClass>(shared_from_this())));
        }
    }

    void CajetaClass::setClassBody(cajeta::ClassBodyDeclarationPtr classBody) {
        for (auto memberDeclaration: classBody->getDeclarations()) {
            memberDeclaration->updateParent(static_pointer_cast<CajetaClass>(shared_from_this()));
        }
    }

    void CajetaClass::generateCode() {
        for (auto& method: methodList) {
            method->generateCode();
        }
    }

    struct MethodEntry {
        MethodPtr method;
        int score;
        MethodEntry(MethodPtr method) { this->method = method; score = 0; }
    };

    MethodPtr CajetaClass::getClosestMethod(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical) {
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
    void CajetaClass::createInheritanceMethodMap(CajetaClassPtr structure) {
        if (structure == nullptr) {
            structure = static_pointer_cast<CajetaClass>(shared_from_this());
        }

        for (auto& superClass: structure->getSuperClasses()) {
            createInheritanceMethodMap(superClass);
        }

        for (auto& method: structure->getMethodList()) {
            mapMethod(method, labeledMethodMap, true);
            mapMethod(method, unlabeledMethodMap, false);
        }
    }

    void CajetaClass::buildVirtualTable() {
        // Build the vtable slot list in parent-first order. Each unique
        // canonical (unlabeled) signature gets one slot. An override in a
        // derived class replaces the slot's MethodPtr but keeps the inherited
        // index — that's the override semantic the vtable relies on.
        //
        // We track slot order in a vector (to preserve insertion order) and
        // canonical→index in a side map for O(1) lookup.
        virtualMethodList.clear();
        vector<MethodPtr> slots;
        map<string, int> canonToIdx;

        std::function<void(CajetaClassPtr)> walk = [&](CajetaClassPtr c) {
            // Parents first so their slots are assigned the lower indices.
            for (auto& sup : c->getSuperClasses()) walk(sup);
            for (auto& m : c->getMethodList()) {
                // Statics and constructors are not virtual — they don't
                // participate in dynamic dispatch.
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                string canon = m->toCanonical(/*labeled=*/false);
                auto it = canonToIdx.find(canon);
                if (it == canonToIdx.end()) {
                    int idx = (int) slots.size();
                    canonToIdx[canon] = idx;
                    m->setVirtualTableIndex(idx);
                    slots.push_back(m);
                } else {
                    m->setVirtualTableIndex(it->second);
                    slots[it->second] = m;  // override replaces inherited
                }
            }
        };
        walk(static_pointer_cast<CajetaClass>(shared_from_this()));

        for (auto& m : slots) virtualMethodList.push_back(m);
    }

    void CajetaClass::writeVirtualTable() {
        // Idempotent: bail if we've already produced a vtable global. Callers
        // can invoke this at any point in prototype generation without
        // worrying about duplicate work.
        if (llvmVirtualTableGlobal != nullptr) return;
        buildVirtualTable();
        StructureMetadata(module).populate(
            static_pointer_cast<CajetaClass>(shared_from_this()));
    }

    llvm::Value* CajetaClass::invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisValue) {
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

        string generic = Method::buildGeneric(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);
        string canonical = Method::buildCanonical(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);

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
        }
        if (!method) {
            return nullptr;
        }
        // Method::generatePrototype injects `this` as the first parameter for non-static
        // methods; prepend the instance pointer here so the call's argument list matches.
        vector<llvm::Value*> methodArgs;
        if (thisValue && method->getModifiers().find(STATIC) == method->getModifiers().end()) {
            methodArgs.push_back(thisValue);
        }
        for (int i = 0; i < parameters.size(); i++) {
            methodArgs.push_back(parameters[i].value);
        }
        return module->getBuilder()->CreateCall(method->getLlvmFunctionType(),
            method->getLlvmFunction(), llvm::ArrayRef<llvm::Value*>(methodArgs));
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
    void CajetaClass::generateMetadata() {
        //pModule->getLlvmModule()->getOrInsertGlobal();
    }
} // code