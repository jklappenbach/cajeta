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

#include <algorithm>
#include <functional>
#include <cstdint>

namespace {
    // FNV-1a 64-bit — must match the runtime's __cajeta_signature_hash
    // exactly so compile-time and runtime hashes of the same canonical
    // signature agree.
    int64_t signatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }
}

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
        // Templates aren't types — `Box` alone has no layout, no methods to
        // lower, no vtable to build. Defer the structural work until a
        // concrete `Box<int32>` is referenced and `instantiate(...)` runs.
        //
        // We DO register the template in canonicalMap so type-use sites can
        // find it by name and route through `instantiate`. We deliberately
        // do NOT add it to module->getStructures() — that's the codegen
        // worklist, and the template's body methods reference unresolved
        // `T` placeholders which can't be lowered. Concrete instantiations
        // (e.g. Box<int32>) are added to structures and codegen normally.
        if (isTemplate()) {
            canonicalMap[qName->toCanonical()] = static_pointer_cast<CajetaType>(shared_from_this());
            canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
            return;
        }
        string canonical = qName->toCanonical();

        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
        typeMap[TypeKey(llvmType)] = shared_from_this();
        // Overwrite the plain-CajetaType placeholder `getOrCreateLlvmType` put
        // in the canonical map so name lookups (e.g. `dynamic_pointer_cast<
        // CajetaClass>(receiverType)` in MethodCallExpression) actually see
        // this class. Also register the short typeName so unqualified
        // references like `new Counter()` find the right entry.
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));

        // Class instance layout: { ptr vtable, <user fields...> }. The vtable
        // pointer at LLVM index 0 is set by `new ClassName()` (see
        // ClassCreatorRest) to point at the class's #VTable global. User
        // properties get LLVM indices 1..N — `getFieldLlvmIndex` exposes
        // the +1 shift to consumers (DotExpression and friends).
        vector<llvm::Type*> llvmMembers;
        llvmMembers.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
        // Iterate propertyList (insertion-ordered) so LLVM indices are
        // deterministic; the `properties` map's iteration order is by
        // string key and would scramble field offsets.
        for (auto& property : propertyList) {
            llvmMembers.push_back(property->getType()->getLlvmType());
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        // Register self in the module's structure map BEFORE method/vtable
        // generation so any later-declared subclass that lists us in its
        // `extends` clause can find us by name. Also: resolve our own
        // parents now — they must have been registered by their own
        // prototype generation, which means declared earlier in the source.
        module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
        resolveSuperClasses();

        ensureDefaultConstructor();

        for (auto methodEntry: methods) {
            methodEntry.second->generatePrototype();
        }

        // Vtable build runs AFTER every method has its LLVM Function — the
        // constant needs `getLlvmFunction()` to return non-null for each slot.
        // Classes with only static methods/constructors produce a 2-slot
        // header-only vtable (`{ i16 version, i16 count = 0 }`); meaningful
        // dispatch arrives when inheritance and virtual calls are wired up.
        writeVirtualTable();

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

    void CajetaClass::resolveSuperClasses() {
        superClasses.clear();
        for (auto& qName : qExtended) {
            // First try the qName's full canonical (correct when the user
            // wrote `extends some.pkg.Animal`).
            auto& structures = module->getStructures();
            auto it = structures.find(qName->toCanonical());
            if (it != structures.end()) {
                superClasses.push_back(it->second);
                continue;
            }
            // Fallback for single-identifier names like `extends Animal`:
            // QualifiedName::fromContext picks "code" as the package, which
            // won't match the class's actual canonical. Walk by short name.
            bool found = false;
            for (auto& entry : structures) {
                if (entry.second->getQName()->getTypeName() == qName->getTypeName()) {
                    superClasses.push_back(entry.second);
                    found = true;
                    break;
                }
            }
            (void) found;  // unresolved parents silently skip; future versions
                           // should raise CAJETA_ERROR_UNRESOLVED_PARENT or
                           // similar once a module-level resolution pass is
                           // in place.
        }
    }

    void CajetaClass::buildVirtualTable() {
        // Build the (canonical → MethodPtr) mapping by walking the hierarchy
        // parent-first. Overrides in derived classes replace the parent's
        // entry; brand-new methods append. Statics and constructors are not
        // virtual and don't participate.
        //
        // Once the unique-method set is determined, we sort by the canonical
        // signature's FNV-1a hash. Dispatch is hash-based (see runtime's
        // __cajeta_vtable_lookup) which sidesteps the slot-index collision
        // problem that multi-inheritance would otherwise hit with a
        // position-only layout.
        virtualMethodList.clear();
        map<string, MethodPtr> uniqueByCanonical;

        std::function<void(CajetaClassPtr)> walk = [&](CajetaClassPtr c) {
            for (auto& sup : c->getSuperClasses()) walk(sup);
            for (auto& m : c->getMethodList()) {
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                string canon = m->toCanonical(/*labeled=*/false);
                uniqueByCanonical[canon] = m;   // overrides naturally replace
            }
        };
        walk(static_pointer_cast<CajetaClass>(shared_from_this()));

        // Sort by hash for binary search at dispatch time. Hash is stable
        // across runs (FNV-1a) so the compiler and runtime agree.
        vector<pair<int64_t, MethodPtr>> sorted;
        sorted.reserve(uniqueByCanonical.size());
        for (auto& entry : uniqueByCanonical) {
            sorted.emplace_back(signatureHash(entry.first), entry.second);
        }
        std::sort(sorted.begin(), sorted.end(),
            [](const pair<int64_t, MethodPtr>& a,
               const pair<int64_t, MethodPtr>& b) {
                return a.first < b.first;
            });

        int idx = 0;
        for (auto& [hash, method] : sorted) {
            method->setVirtualTableIndex(idx++);
            virtualMethodList.push_back(method);
        }
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

    MethodPtr CajetaClass::resolveMethod(string& methodName, vector<ParameterEntry>& parameters, bool isConstructor, bool floatingParams) {
        // Each class indexes its declared methods under keys built with its
        // own class name (Method::buildGeneric/buildCanonical embed the
        // parent class). Inherited methods are NOT re-keyed into derived
        // class maps; instead we walk the hierarchy at lookup time so the
        // recursive call hits the parent's maps with the right key flavor.
        string generic = Method::buildGeneric(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);
        string canonical = Method::buildCanonical(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);

        map<string, map<string, MethodPtr>>* genericMap;
        if (isConstructor) {
            genericMap = floatingParams ? &labeledConstructorMap : &unlabeledConstructorMap;
        } else {
            genericMap = floatingParams ? &labeledMethodMap : &unlabeledMethodMap;
        }

        if (genericMap->find(generic) != genericMap->end()) {
            map<string, MethodPtr>& canonicalMap = (*genericMap)[generic];
            auto it = canonicalMap.find(canonical);
            if (it != canonicalMap.end()) {
                return it->second;
            }
            MethodPtr m = getClosestMethod(methodName, parameters, canonicalMap);
            if (m) return m;
        }

        // Constructors are NOT inherited; only walk parents for instance methods.
        if (!isConstructor) {
            for (auto& parent : superClasses) {
                MethodPtr m = parent->resolveMethod(methodName, parameters, isConstructor, floatingParams);
                if (m) return m;
            }
        }
        return nullptr;
    }

    llvm::Value* CajetaClass::invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisValue) {
        bool floatingParams = true;
        for (auto &param : parameters) {
            if (param.label.empty()) {
                floatingParams &= false;
            }
        }

        if (floatingParams) {
            sort(parameters.begin(), parameters.end(), [](const ParameterEntry& a, const ParameterEntry& b) -> bool { return a.label < b.label; });
        }

        MethodPtr method = resolveMethod(methodName, parameters, isConstructor, floatingParams);
        if (!method) {
            return nullptr;
        }
        // Method::generatePrototype injects `this` as the first parameter for non-static
        // methods; prepend the instance pointer here so the call's argument list matches.
        bool isStatic = method->getModifiers().find(STATIC) != method->getModifiers().end();
        vector<llvm::Value*> methodArgs;
        if (thisValue && !isStatic) {
            methodArgs.push_back(thisValue);
        }
        for (int i = 0; i < parameters.size(); i++) {
            methodArgs.push_back(parameters[i].value);
        }

        // Dynamic dispatch via the receiver's vtable: hash the method's
        // canonical signature, load the receiver's vtable pointer (instance
        // slot 0), call __cajeta_vtable_lookup to find the function pointer,
        // and indirect-call it. Statics and constructors stay direct — they
        // don't participate in the vtable. The compile-time `method` is the
        // statically-resolved entry; the runtime hash-based lookup is what
        // actually picks the override-correct function in a subclass.
        auto* builder = module->getBuilder();
        auto& llvmCtx = *module->getLlvmContext();
        llvm::Value* callee = method->getLlvmFunction();
        bool useVtable = thisValue && !isStatic && !isConstructor;
        if (useVtable) {
            llvm::Function* lookupFn = module->getRuntimeFunction("__cajeta_vtable_lookup");
            if (lookupFn) {
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                // The instance's vtable pointer lives at slot 0 — load it.
                // `thisValue` is the receiver heap pointer; first 8 bytes
                // are the vtable* (per CajetaClass::generatePrototype's
                // layout decision).
                llvm::Value* vtable = builder->CreateLoad(ptrTy, thisValue, "vtable");
                int64_t hash = signatureHash(method->toCanonical(/*labeled=*/false));
                llvm::Value* fnPtr = builder->CreateCall(lookupFn,
                    {vtable,
                     llvm::ConstantInt::get(i64Ty, llvm::APInt(64, (uint64_t) hash, false))},
                    "vmethod_fn");
                callee = fnPtr;
            }
        }
        return builder->CreateCall(method->getLlvmFunctionType(),
            callee, llvm::ArrayRef<llvm::Value*>(methodArgs));
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