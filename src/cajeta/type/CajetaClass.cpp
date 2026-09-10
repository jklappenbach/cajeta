//
// Created by James Klappenbach on 10/24/22.
//

#include "CajetaClass.h"
#include "cajeta/xref/XrefIndex.h"
#include "CajetaFunctionType.h"
#include "CajetaView.h"
#include "llvm/TargetParser/Triple.h"
#include "StructureMetadata.h"
#include "../error/Diagnostics.h"
#include "../field/Field.h"
#include "../method/Method.h"
#include "../util/MemoryManager.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../method/DefaultConstructorMethod.h"
#include "../method/SynthesizedHashMethod.h"
#include "../method/SynthesizedGetterMethod.h"
#include "../method/SynthesizedSetterMethod.h"
#include "../method/SynthesizedToStringMethod.h"
#include "../method/SynthesizedConstructorMethod.h"
#include "../method/SynthesizedStaticFactoryMethod.h"
#include "../method/SynthesizedWithMethod.h"
#include "../method/SynthesizedBuilderMethods.h"
#include "../method/SynthesizedEncodingMethods.h"
#include "../method/SynthesizedMockClass.h"
#include "CajetaArray.h"
#include "../compile/CompilationContext.h"
#include "../compile/SessionState.h"
#include "../field/HeapField.h"
#include "../error/Exception.h"
#include "../asn/expression/LiteralExpression.h"
#include "../asn/VariableDeclarator.h"
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <cstdlib>

#include <algorithm>
#include <functional>
#include <cstdint>
#include <limits>

namespace {
    // FNV-1a 64-bit over a canonical signature; must match the runtime's
    // __cajeta_signature_hash exactly. `#` is erased (dispatch is mode-blind)
    // unless it is part of an operator name (`operator#[]`), which is identity.
    int64_t signatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        static const char* kOp = "operator";
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = (unsigned char) s[i];
            if (c == '#') {
                bool afterOperator = i >= 8
                    && s.compare(i - 8, 8, kOp) == 0;
                if (!afterOperator) continue;
            }
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }
}

using namespace std;

namespace cajeta {
    // Per-thread side table of a frozen (shared) class's LLVMContext-bound
    // codegen bindings; see ClassLlvmBindings and the *Ref() accessors.
    std::unordered_map<const CajetaClass*, ClassLlvmBindings>& CajetaClass::frozenClassBindings() {
        static thread_local std::unordered_map<const CajetaClass*, ClassLlvmBindings> tbl;
        return tbl;
    }

    // Defined below; used by generateStaticInitializers, which precedes it.
    static llvm::Constant* foldStaticInitializer(
        AbstractSyntaxNodePtr init, llvm::Type* storedType);
    void CajetaClass::captureDeclaringFile() {
        if (module) declaringFile = module->currentSourceFile();
    }

    CajetaClass::CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented) : CajetaType(qName) {
        this->qImplemented = qImplemented;
        this->module = module;
        captureDeclaringFile();
    }
    CajetaClass::CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented)
            : CajetaType(qName) {
        this->qExtended = qExtended;
        this->qImplemented = qImplemented;
        this->module = module;
        captureDeclaringFile();
    }

    // Per-thread type-fill epoch counter, by reference so callers can bump it.
    uint64_t& CajetaClass::typeFillEpoch() {
        static thread_local uint64_t epoch = 0;
        return epoch;
    }

    // This class's LLVM type, built on demand. Placeholders, wildcard proxies and
    // bare templates lower to `ptr`; a frozen class rebuilds its named struct in
    // the calling thread's own context before returning.
    llvm::Type* CajetaClass::getLlvmType() {
        if (llvm::Type* cur = rawLlvmType()) return cur;
        if (placeholderFlag && module) {
            return llvm::PointerType::get(*module->getLlvmContext(), 0);
        }
        if (isWildcardInstantiation() && module) {
            return llvm::PointerType::get(*module->getLlvmContext(), 0);
        }
        if (isTemplate() && module) {
            return llvm::PointerType::get(*module->getLlvmContext(), 0);
        }
        if (isFrozen() && module) {
            llvm::LLVMContext* ctx = currentLlvmContext();
            if (!ctx) ctx = module->getLlvmContext();
            if (ctx) {
                string canonical = qName->toCanonical();
                llvm::StructType* st =
                    CajetaType::getOrCreateLlvmStructNoRegister(ctx, canonical);
                setLlvmType(st);
                if (st->isOpaque()) {
                    if (isInterface()) {
                        llvm::Type* ptrTy = llvm::PointerType::get(*ctx, 0);
                        llvm::Type* i64Ty = llvm::Type::getInt64Ty(*ctx);
                        vector<llvm::Type*> members{ ptrTy, ptrTy, i64Ty };
                        st->setBody(llvm::ArrayRef<llvm::Type*>(members), false);
                    } else {
                        buildInstanceStructBody(ctx);
                    }
                }
                return rawLlvmType();
            }
        }
        // Gate on a populated field list: building mid-parse would set an EMPTY
        // body that the opacity guard would later refuse to replace.
        if (isValueType() && !placeholderFlag && module && qName
                && !properties.empty()) {
            llvm::LLVMContext* ctx = module->getLlvmContext();
            if (ctx) {
                string canonical = qName->toCanonical();
                llvm::StructType* st =
                    CajetaType::getOrCreateLlvmStructNoRegister(ctx, canonical);
                setLlvmType(st);
                if (st->isOpaque()) {
                    buildInstanceStructBody(ctx);
                }
                return rawLlvmType();
            }
        }
        return rawLlvmType();
    }

    // The `{ i1 present, ptr }` reference wrapper struct for this class, created
    // once and cached.
    llvm::Type* CajetaClass::getLlvmReferenceType() {
        auto& refType = referenceTypeRef();
        if (refType == nullptr) {
            vector<llvm::Type*> types;
            types.push_back(llvm::Type::getInt1Ty(*module->getLlvmContext()));
            types.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
            refType = llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef<llvm::Type*>(types));
        }
        return refType;
    }

    // True when `source` is this class or one of its transitive superclasses.
    // Walks `extends` only; implemented interfaces are not consulted.
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

    // True when any type argument is a wildcard (`Box<?>`, `Box<? extends T>`).
    bool CajetaClass::isWildcardInstantiation() const {
        for (auto& a : typeArguments) {
            if (a && a->isWildcard()) return true;
        }
        return false;
    }

    // True when any type argument is a BOUNDED wildcard (`? extends` / `? super`).
    bool CajetaClass::isBoundedWildcardInstantiation() const {
        for (auto& a : typeArguments) {
            if (a && a->isWildcard()
                    && a->wildcardKind() != CajetaType::WildcardKind::Unbounded) {
                return true;
            }
        }
        return false;
    }

    // True for the cajeta.lang numeric marker interface names.
    bool CajetaClass::isNumericMarkerName(const string& name) {
        return name == "Numeric" || name == "Floating"
            || name == "Integral" || name == "Complex";
    }

    // True when `arg` satisfies the named cajeta.lang numeric marker, either
    // intrinsically (a primitive, via the type-flag lattice) or nominally (a class
    // implementing it, transitively through both extends and implements).
    bool CajetaClass::satisfiesNumericMarker(CajetaTypePtr arg,
                                             const string& marker) {
        if (!arg) return false;
        int flags = arg->getTypeFlags();
        bool isPrim = (flags & PRIMITIVE_FLAG) != 0;
        if (isPrim) {
            bool isBool = arg->getQName()
                && arg->getQName()->getTypeName() == "boolean";
            if (marker == "Numeric")  return (flags & NUMBER_FLAG) != 0 && !isBool;
            if (marker == "Integral") return (flags & INT_FLAG) != 0 && !isBool;
            if (marker == "Floating") return (flags & FLOAT_FLAG) != 0;
            if (marker == "Complex")  return false;
            return false;
        }
        auto cls = dynamic_pointer_cast<CajetaClass>(arg);
        if (cls) {
            auto& cmap = CajetaType::getCanonicalMap();
            CajetaTypePtr markerType;
            auto it = cmap.find(string("cajeta.lang.") + marker);
            if (it != cmap.end()) {
                markerType = it->second;
            } else {
                auto n = cmap.find(marker);
                if (n != cmap.end()) markerType = n->second;
            }
            auto markerClass = dynamic_pointer_cast<CajetaClass>(markerType);
            if (markerClass) {
                if (cls->isParentOrKind(markerClass)) return true;
                std::function<bool(CajetaClassPtr)> implWalk =
                    [&](CajetaClassPtr c) -> bool {
                        if (!c) return false;
                        for (auto& iface : c->getImplementedInterfaces()) {
                            if (!iface) continue;
                            if (iface->isParentOrKind(markerClass)) return true;
                            if (implWalk(iface)) return true;
                        }
                        for (auto& sup : c->getSuperClasses()) {
                            if (implWalk(sup)) return true;
                        }
                        return false;
                    };
                if (implWalk(cls)) return true;
            }
        }
        return false;
    }

    // Zero-fills a fresh instance and installs its vtable pointers: primary at slot
    // 0, one secondary per non-first-parent sub-object. @ValueType PODs have no
    // slot-0 vtable and skip it; a heap instance also patches drop_fn.
    void CajetaClass::initInstanceLayout(CajetaModulePtr module,
                                         llvm::Value* instance,
                                         llvm::Type* structTy, bool stackAlloc) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();
        const llvm::DataLayout& dataLayout =
            module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(llvmCtx),
            dataLayout.getTypeAllocSize(structTy));

        builder->CreateMemSet(instance,
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvmCtx), 0),
            allocSize, llvm::MaybeAlign(8));

        if (!hasVtablePointerAtSlotZero()) return;

        if (llvm::GlobalVariable* vtable = getVirtualTableGlobal()) {
            llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                module->emitTargetLlvmModule(), vtable);
            llvm::Value* vtablePtrSlot = builder->CreateStructGEP(
                structTy, instance, /*idx=*/0, "vtable_slot");
            builder->CreateStore(vtableRef, vtablePtrSlot);
        }
        for (const auto& sub : getNonFirstSubObjects()) {
            llvm::GlobalVariable* secVT = getOrCreateSecondaryVTable(sub.ancestor);
            if (!secVT) continue;
            llvm::Constant* secRef = CajetaModule::ensureGlobalInModule(
                module->emitTargetLlvmModule(), secVT);
            llvm::Value* secSlot = builder->CreateStructGEP(
                structTy, instance, (unsigned) sub.slot,
                std::string("sec_vtable_slot_")
                    + sub.ancestor->getQName()->getTypeName());
            builder->CreateStore(secRef, secSlot);
        }
        if (!stackAlloc) {
            patchVirtualTableDropFn();
        }
    }

    // Mallocs an instance, initializes its layout, and runs the constructor matching
    // `entries`. A template instantiation uses the origin's constructor name.
    llvm::Value* CajetaClass::heapConstruct(CajetaModulePtr module,
                                            vector<ParameterEntry>& entries) {
        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();
        llvm::Type* structTy = getLlvmType();
        const llvm::DataLayout& dataLayout =
            module->getLlvmModule()->getDataLayout();
        llvm::Constant* allocSize = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(llvmCtx),
            dataLayout.getTypeAllocSize(structTy));
        llvm::Value* instance = MemoryManager::createMallocInstruction(
            module, allocSize, builder->GetInsertBlock());
        initInstanceLayout(module, instance, structTy, /*stackAlloc=*/false);
        std::string ctorName = getQName()->getTypeName();
        if (getTemplateOrigin()) {
            ctorName = getTemplateOrigin()->getQName()->getTypeName();
        }
        invokeMethod(ctorName, entries, /*isConstructor=*/true, instance,
                     /*callerModule=*/module);
        return instance;
    }

    // True when `from` is assignable to the wildcard instantiation `wildcardInst`:
    // same template origin, identical concrete argument positions, and every
    // wildcard position's extends/super bound satisfied.
    bool CajetaClass::isAssignableToWildcard(
            CajetaClassPtr from, CajetaClassPtr wildcardInst) {
        if (!from || !wildcardInst) return false;
        if (!wildcardInst->isWildcardInstantiation()) return false;
        auto fromOrigin = from->getTemplateOrigin();
        auto destOrigin = wildcardInst->getTemplateOrigin();
        if (!fromOrigin || !destOrigin) return false;
        if (fromOrigin.get() != destOrigin.get()) return false;
        auto& fromArgs = from->getTypeArguments();
        auto& destArgs = wildcardInst->getTypeArguments();
        if (fromArgs.size() != destArgs.size()) return false;
        for (size_t i = 0; i < destArgs.size(); ++i) {
            auto destArg = destArgs[i];
            auto fromArg = fromArgs[i];
            if (!destArg || !fromArg) return false;
            if (!destArg->isWildcard()) {
                if (fromArg.get() != destArg.get()) return false;
                continue;
            }
            auto kind = destArg->wildcardKind();
            if (kind == CajetaType::WildcardKind::Unbounded) continue;
            auto bound = destArg->wildcardBound();
            if (!bound) return false;
            string boundName =
                bound->getQName() ? bound->getQName()->getTypeName() : string();
            if (isNumericMarkerName(boundName)) {
                if (kind == CajetaType::WildcardKind::Extends) {
                    if (!satisfiesNumericMarker(fromArg, boundName)) return false;
                    continue;
                }
                return false;
            }
            auto fromArgClass = dynamic_pointer_cast<CajetaClass>(fromArg);
            auto boundClass = dynamic_pointer_cast<CajetaClass>(bound);
            if (!fromArgClass || !boundClass) {
                return false;
            }
            if (kind == CajetaType::WildcardKind::Extends) {
                if (!fromArgClass->isParentOrKind(boundClass)) return false;
            } else if (kind == CajetaType::WildcardKind::Super) {
                if (!boundClass->isParentOrKind(fromArgClass)) return false;
            }
        }
        return true;
    }

    // Total number of methods across every bucket of a two-layer method map.
    int getMethodCount(map<string, map<string, MethodPtr>>& map) {
        int count = 0;
        for (auto& entry : map) {
            count += entry.second.size();
        }
        return count;
    }

    // Indexes `method` in a two-layer (generic -> canonical) map and assigns its
    // vtable index, reusing an existing same-key entry's index when there is one.
    void mapMethod(MethodPtr method, map<string, map<string, MethodPtr>>& map, bool labeled) {
        string generic = method->toGeneric(labeled);
        string canonical = method->getMapKey(labeled);

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

    // Registers a method on this class, rejecting mode-only overloads and duplicate
    // constructors or statics. Constructors land only in the constructor maps.
    void CajetaClass::addMethod(MethodPtr method) {
    // title-tracking 5.2.4 — a mode-only overload is rejected AT DECLARATION:
    // dispatch erases `#`, so `f(Cell)` and `f(#Cell)` hash to the same slot.
    // The canonical key already erases it, hence the parallel mode fingerprint.
        {
            const std::string erased = method->getMapKey();
            std::string rawKey;
            for (auto& fp : method->getParameterList()) {
                rawKey.push_back(fp && fp->isTransferred() ? '#' : '.');
            }
            auto prior = modeErasedMethodKeys.find(erased);
            if (prior != modeErasedMethodKeys.end() && prior->second != rawKey) {
                throw Exception(
                    "method `" + method->getName() + "` is declared twice in `"
                        + (getQName() ? getQName()->toCanonical() : std::string("<anonymous>"))
                        + "` with signatures that differ only in transfer mode "
                          "(`#`). Transfer mode is a per-call decision made by "
                          "the CALLER, not part of the signature — dispatch "
                          "erases `#`, so these two declarations collide. Fix: "
                          "keep ONE declaration (a plain formal already accepts "
                          "both a lend and a `#`-transfer at the call site); "
                          "spell `#T` only when the method MUST own its "
                          "argument. See docs/specification/MemoryModel.md "
                          "§ Function signatures.",
                    "CAJETA_ERROR_TRANSFER_MODE_OVERLOAD");
            }
            modeErasedMethodKeys[erased] = rawKey;
        }
        methods[method->getMapKey()] = method;

        if (method->isConstructor()) {
            map<string, MethodPtr> canonical = unlabeledConstructorMap[method->toGeneric(false)];
            if (canonical.find(method->getMapKey(false)) != canonical.end()) {
                throw Exception(
                    "constructor `" + method->getMapKey(false)
                        + "` already exists on `" + toCanonical()
                        + "` — the class declaration was processed twice",
                    "CAJETA_ERROR_DUPLICATE_CONSTRUCTOR");
            }
            mapMethod(method, labeledConstructorMap, true);
            mapMethod(method, unlabeledConstructorMap, false);
        } else {
            if (method->isStatic()) {
                map<string, MethodPtr> canonical = unlabeledMethodMap[method->toGeneric(false)];
                if (canonical.find(method->getMapKey(false)) != canonical.end()) {
                    throw Exception(
                        "static method `" + method->getMapKey(false)
                            + "` already exists on `" + toCanonical()
                            + "` — static methods cannot be overridden",
                        "CAJETA_ERROR_DUPLICATE_STATIC_METHOD");
                }
                staticMethods[method->getMapKey()] = method;
            }
            methodList.push_back(method);
            methods[method->getMapKey()] = method;
            mapMethod(method, labeledMethodMap, true);
            mapMethod(method, unlabeledMethodMap, false);
        }
    }

    void CajetaClass::addMethods(list<MethodPtr> methods) {
        for (MethodPtr method: methods) {
            addMethod(method);
        }
    }

    // Removes a method from every index it was registered in: methods, statics,
    // methodList, and its generic/canonical bucket.
    void CajetaClass::removeMethod(const MethodPtr& method) {
        if (!method) return;
        methods.erase(method->getMapKey());
        staticMethods.erase(method->getMapKey());
        for (auto it = methodList.begin(); it != methodList.end(); ) {
            if (*it == method) it = methodList.erase(it); else ++it;
        }
        auto eraseFromGenericMap =
            [&](map<string, map<string, MethodPtr>>& gmap, bool labeled) {
                auto git = gmap.find(method->toGeneric(labeled));
                if (git == gmap.end()) return;
                git->second.erase(method->getMapKey(labeled));
                if (git->second.empty()) gmap.erase(git);
            };
        if (method->isConstructor()) {
            eraseFromGenericMap(labeledConstructorMap, true);
            eraseFromGenericMap(unlabeledConstructorMap, false);
        } else {
            eraseFromGenericMap(labeledMethodMap, true);
            eraseFromGenericMap(unlabeledMethodMap, false);
        }
    }

    // Snapshots every module-bound binding (vtables, RTTI, drops, static-field
    // globals, per-method llvm::Functions) so restoreReuseBaseline can undo
    // whatever a reusing test's module generated on top of it.
    void CajetaClass::captureReuseBaseline() {
        reuseBaseline.valid = true;
        reuseBaseline.emitModule = emitModule;
        reuseBaseline.vtableGlobal = llvmVirtualTableGlobal;
        reuseBaseline.rttiGlobal = llvmRttiGlobal;
        reuseBaseline.dropFunction = llvmDropFunction;
        reuseBaseline.stackDropFunction = llvmStackDropFunction;
        reuseBaseline.dropFunctionPatched = llvmDropFunctionPatched;
        reuseBaseline.interfaceVTables = interfaceVTables;
        reuseBaseline.staticFieldGlobals = staticFieldGlobals;
        reuseBaseline.secondaryVTables = secondaryVTables;
        reuseBaseline.methodFns.clear();
        for (auto& m : methodList)
            if (m) reuseBaseline.methodFns[m.get()] = m->getLlvmFunction();
    }

    // Restores the prime snapshot so the next reusing test regenerates into its own
    // module. emitModule is reset FIRST: a stale one makes getEmitModule() resolve
    // this class's regenerated drop callees into a freed module.
    void CajetaClass::restoreReuseBaseline() {
        if (!reuseBaseline.valid) return;
        emitModule = reuseBaseline.emitModule;
        llvmVirtualTableGlobal = reuseBaseline.vtableGlobal;
        llvmRttiGlobal = reuseBaseline.rttiGlobal;
        llvmDropFunction = reuseBaseline.dropFunction;
        llvmStackDropFunction = reuseBaseline.stackDropFunction;
        llvmDropFunctionPatched = reuseBaseline.dropFunctionPatched;
        interfaceVTables = reuseBaseline.interfaceVTables;
        staticFieldGlobals = reuseBaseline.staticFieldGlobals;
        secondaryVTables = reuseBaseline.secondaryVTables;
        for (auto& m : methodList) {
            if (!m) continue;
            auto it = reuseBaseline.methodFns.find(m.get());
            llvm::Function* base =
                (it != reuseBaseline.methodFns.end()) ? it->second : nullptr;
            if (m->getLlvmFunction() != base) m->setLlvmFunction(base);
        }
    }

    void CajetaClass::addProperty(StructurePropertyPtr field) {
        properties[field->getName()] = field;
        propertyList.push_back(field);
    }

    // Byte offset of `ancestor`'s sub-object within this class's instance layout;
    // 0 for self, for an unknown ancestor, or for a shared first parent.
    uint64_t CajetaClass::getSubObjectByteOffset(const CajetaClass* ancestor) const {
        if (!ancestor || ancestor == this) return 0;
        auto it = subObjectSlotMap.find(ancestor);
        if (it == subObjectSlotMap.end() || it->second == 0) return 0;
        llvm::Type* lt = rawLlvmType();
        if (!lt || !llvm::isa<llvm::StructType>(lt)) return 0;
        auto* st = llvm::cast<llvm::StructType>(lt);
        if (!st->isSized()) return 0;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getStructLayout(st)->getElementOffset((unsigned) it->second);
    }

    // Every sub-object carrying its own vtable slot (i.e. not the primary chain) as
    // (ancestor, slot, byteOffset). Walks the layout in embedSubObject's exact
    // order; subObjectSlotMap cannot be reused, since diamonds alias its entries.
    std::vector<CajetaClass::NonFirstSubObject>
    CajetaClass::getNonFirstSubObjects() {
        std::vector<NonFirstSubObject> result;
        llvm::Type* lt = rawLlvmType();
        if (!lt || !llvm::isa<llvm::StructType>(lt)) return result;
        auto* st = llvm::cast<llvm::StructType>(lt);
        if (!st->isSized()) return result;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        const auto* layout = dl.getStructLayout(st);
        int slot = 0;
        bool isSelf = true;
        std::function<void(CajetaClassPtr, bool)> walk =
            [&](CajetaClassPtr cls, bool ownVtable) {
                int subObjectStart = -1;
                if (ownVtable) {
                    subObjectStart = slot;
                    if (!isSelf) {
                        uint64_t off = layout->getElementOffset(
                            (unsigned) subObjectStart);
                        result.push_back({cls, subObjectStart, off});
                    }
                    isSelf = false;
                    slot++;
                }
                int idx = 0;
                for (auto& parent : cls->superClasses) {
                    walk(parent, /*ownVtable=*/(idx != 0));
                    idx++;
                }
                for (auto& p : cls->propertyList) {
                    if (p->isStatic()) continue;
                    slot++;
                }
                // Mirror the hidden per-instance ownership word embedSubObject
                // emits, or every later sub-object's slot index is too low.
                {
                    bool wantsWord = false;
                    for (auto& p : cls->propertyList) {
                        if (!p->isStatic() && fieldHasOwnershipBit(p)) {
                            wantsWord = true;
                            break;
                        }
                    }
                    if (wantsWord) slot++;
                }
                slot += (int) cls->getVbaseAncestors().size();
            };
        walk(std::static_pointer_cast<CajetaClass>(shared_from_this()),
            /*ownVtable=*/true);
        return result;
    }

    // Shifts a derived-class pointer to the `dstType` sub-object's byte offset for
    // an upcast. Identity for interfaces, which use the fat-pointer path, and for
    // a zero offset.
    llvm::Value* CajetaClass::adjustForUpcast(
            CajetaModulePtr module,
            llvm::Value* srcValue,
            CajetaTypePtr srcType,
            CajetaTypePtr dstType) {
        if (!srcValue || !srcType || !dstType) return srcValue;
        auto srcClass = std::dynamic_pointer_cast<CajetaClass>(srcType);
        auto dstClass = std::dynamic_pointer_cast<CajetaClass>(dstType);
        if (!srcClass || !dstClass) return srcValue;
        if (srcClass.get() == dstClass.get()) return srcValue;
        if (dstClass->isInterface() || srcClass->isInterface()) return srcValue;
        uint64_t off = srcClass->getSubObjectByteOffset(dstClass.get());
        if (off == 0) return srcValue;
        auto* builder = module->getBuilder();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        return builder->CreateInBoundsGEP(i8Ty, srcValue,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), off),
            "upcast_subobj");
    }

    // Replaces the separators that are illegal in a symbol name with underscores.
    static std::string sanitizeSymbol(std::string s) {
        for (char& c : s) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                    || c == ',' || c == ' ' || c == '(' || c == ')') {
                c = '_';
            }
        }
        return s;
    }

    // Emits (or reuses) a private thunk that biases `this` back by
    // `parentOffsetInThis` and tail-calls `impl` — the entry a secondary vtable uses
    // so a parent-typed view dispatches into this class's override.
    llvm::Function* CajetaClass::synthesizeOffsetThunk(
            CajetaClassPtr parent,
            MethodPtr impl,
            uint64_t parentOffsetInThis) {
        if (!impl || !impl->getLlvmFunctionType()) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        llvm::FunctionType* fnTy = impl->getLlvmFunctionType();

        std::string name = sanitizeSymbol(
            "__cajeta_thunk_" + qName->toCanonical()
            + "_via_" + parent->getQName()->toCanonical()
            + "_to_" + impl->toCanonical(/*labeled=*/false));
        if (auto* existing = lmod->getFunction(name)) return existing;

        llvm::Function* implFn = CajetaModule::ensureFunctionInModule(
            lmod, impl->getLlvmFunction());

        llvm::Function* thunk = llvm::Function::Create(fnTy,
            llvm::Function::PrivateLinkage, name, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", thunk);
        llvm::IRBuilder<> b(bb);

        std::vector<llvm::Value*> args;
        args.reserve(thunk->arg_size());
        unsigned idx = 0;
        for (auto& a : thunk->args()) {
            if (idx == 0 && parentOffsetInThis != 0) {
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                llvm::Value* off = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx),
                    (uint64_t) -(int64_t) parentOffsetInThis, /*isSigned=*/true);
                args.push_back(b.CreateInBoundsGEP(i8Ty, &a, off, "thunk_this"));
            } else {
                args.push_back(&a);
            }
            idx++;
        }
        llvm::CallInst* call = b.CreateCall(fnTy, implFn, args);
        call->setTailCall(true);
        if (fnTy->getReturnType()->isVoidTy()) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(call);
        }
        return thunk;
    }

    // Builds (or returns the cached) per-(this, parent) vtable, so dispatch through
    // a non-first-parent-typed binding finds this class's overrides. Reuses the
    // parent's vtable struct type to keep the two layouts compatible.
    llvm::GlobalVariable* CajetaClass::getOrCreateSecondaryVTable(
            CajetaClassPtr parent) {
        if (!parent) return nullptr;
        std::string parentCanon = parent->getQName()->toCanonical();
        auto& secondaryVTables = secondaryVTablesRef();
        auto cached = secondaryVTables.find(parentCanon);
        if (cached != secondaryVTables.end()) return cached->second;

        if (!parent->getVirtualTableGlobal()) {
            parent->writeVirtualTable();
        }
        llvm::StructType* parentVtableType = parent->getVirtualTableType();
        if (!parentVtableType) return nullptr;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        std::string vtName = sanitizeSymbol(
            qName->toCanonical() + "$as$" + parentCanon + "#VTable");
        if (auto* existing = lmod->getGlobalVariable(vtName)) {
            secondaryVTables[parentCanon] = existing;
            return existing;
        }

        uint64_t parentOffset = getSubObjectByteOffset(parent.get());

        llvm::Type* i16Ty = llvm::IntegerType::getInt16Ty(ctx);
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        const auto& parentSlots = parent->getVirtualMethodList();
        const auto& parentHashes = parent->getVirtualSlotHashList();

        std::map<int64_t, MethodPtr> ourByHash;
        {
            auto mIt = virtualMethodList.begin();
            for (size_t i = 0; i < virtualSlotHashList.size()
                    && mIt != virtualMethodList.end(); ++i, ++mIt) {
                ourByHash[virtualSlotHashList[i]] = *mIt;
            }
        }

        // Entries live at index 5, after version, count, parent_vtable, drop_fn
        // and classObject (StructureMetadata::createVirtualTableType).
        llvm::ArrayType* entriesArrTy = llvm::cast<llvm::ArrayType>(
            parentVtableType->getTypeAtIndex(5));
        llvm::StructType* entryTy = llvm::cast<llvm::StructType>(
            entriesArrTy->getElementType());

        std::vector<llvm::Constant*> entryConstants;
        entryConstants.reserve(parentSlots.size());
        auto pIt = parentSlots.begin();
        for (size_t i = 0; i < parentHashes.size() && pIt != parentSlots.end();
                ++i, ++pIt) {
            int64_t hash = parentHashes[i];
            MethodPtr parentMethod = *pIt;
            MethodPtr ourImpl;
            auto found = ourByHash.find(hash);
            if (found != ourByHash.end()) ourImpl = found->second;

            llvm::Function* fn = nullptr;
            bool isOverride = ourImpl && parentMethod
                && ourImpl.get() != parentMethod.get()
                && ourImpl->getParent()
                && ourImpl->getParent().get() != parent.get();
            if (isOverride) {
                fn = synthesizeOffsetThunk(parent, ourImpl, parentOffset);
            } else if (parentMethod && parentMethod->getLlvmFunction()) {
                fn = CajetaModule::ensureFunctionInModule(
                    lmod, parentMethod->getLlvmFunction());
            }
            llvm::Constant* fnConst = fn
                ? (llvm::Constant*) fn
                : llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrTy));
            entryConstants.push_back(llvm::ConstantStruct::get(entryTy, {
                llvm::ConstantInt::get(i64Ty,
                    llvm::APInt(64, (uint64_t) hash, false)),
                fnConst,
            }));
        }

        llvm::Constant* entriesArr = llvm::ConstantArray::get(
            entriesArrTy, llvm::ArrayRef<llvm::Constant*>(entryConstants));

        llvm::Constant* parentVtableRef =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        if (auto pv = parent->getVirtualTableGlobal()) {
            parentVtableRef = CajetaModule::ensureGlobalInModule(lmod, pv);
        }
        llvm::Constant* dropFnConst =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));

        // Slot 4 is classObject: point at THIS class's #ClassObject so getClass()
        // reports the dynamic type through a parent-subobject view.
        llvm::Constant* classObjConst =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        if (auto* co = getClassObjectGlobal()) {
            classObjConst = CajetaModule::ensureGlobalInModule(lmod, co);
        }

        std::vector<llvm::Constant*> initArgs{
            llvm::ConstantInt::get(i16Ty, llvm::APInt(16, 0, false)),
            llvm::ConstantInt::get(i16Ty,
                llvm::APInt(16, parentSlots.size(), false)),
            parentVtableRef,
            dropFnConst,
            classObjConst,
            entriesArr,
        };
        llvm::Constant* initializer = llvm::ConstantStruct::get(parentVtableType,
            llvm::ArrayRef<llvm::Constant*>(initArgs));

        auto* g = (llvm::GlobalVariable*) lmod->getOrInsertGlobal(
            vtName, parentVtableType);
        g->setInitializer(initializer);
        secondaryVTables[parentCanon] = g;
        return g;
    }

    // True when a non-static field of type `p` carries a per-instance ownership
    // bit: heap arrays, function-typed (closure) fields, and vtable-bearing class
    // fields, String included. Views, interfaces and value types do not.
    bool CajetaClass::fieldHasOwnershipBit(const StructurePropertyPtr& p) {
        if (!p || p->isStatic()) return false;
        auto t = p->getType();
        if (!t) return false;
        if (auto arr = dynamic_pointer_cast<CajetaArray>(t)) {
            return !arr->isInlineArray();
        }
        if (dynamic_pointer_cast<CajetaFunctionType>(t)) return true;
        auto cls = dynamic_pointer_cast<CajetaClass>(t);
        if (!cls) return false;
        if (dynamic_pointer_cast<CajetaView>(t)) return false;
        if (cls->isInterface() || cls->isValueType()) return false;
        if (cls->isSharedCapableValue()) return false;
        return true;
    }

    // True when an array element of type `elem` owns a droppable object per slot: a
    // vtable-bearing class that is not a String, a view, or a value type.
    bool CajetaClass::arrayElementCarriesSlotBits(const CajetaTypePtr& elem) {
        if (!elem) return false;
        if (dynamic_pointer_cast<CajetaArray>(elem)) return false;
        if (dynamic_pointer_cast<CajetaView>(elem)) return false;
        auto cls = dynamic_pointer_cast<CajetaClass>(elem);
        if (!cls) return false;
        if (cls->isInterface() || cls->isValueType()) return false;
        if (cls->isSharedCapableValue()) return false;
        if (cls->getQName()
                && cls->getQName()->getTypeName() == "String"
                && cls->getQName()->getPackageName() == "cajeta.lang") {
            return false;
        }
        return cls->hasVtablePointerAtSlotZero();
    }

    // True when an array element is itself a heap array (a jagged array), so each
    // owned slot holds a separately freeable buffer. Inline arrays do not.
    bool CajetaClass::arrayElementCarriesArraySlotBits(const CajetaTypePtr& elem) {
        if (!elem) return false;
        if (dynamic_pointer_cast<CajetaView>(elem)) return false;
        auto arr = dynamic_pointer_cast<CajetaArray>(elem);
        if (!arr) return false;
        if (arr->isInlineArray()) return false;
        return true;
    }

    // Kind code the jagged-array drop walk uses for the inner element type: 1 when
    // those inner elements carry slot bits, 0 otherwise.
    int CajetaClass::arrayElementInnerDropKind(const CajetaTypePtr& elem) {
        auto arr = dynamic_pointer_cast<CajetaArray>(elem);
        if (!arr) return 0;
        return CajetaClass::arrayElementCarriesSlotBits(arr->getElementType())
            ? 1 : 0;
    }

    // A member's release family inside a value-struct slot: bit-guarded members
    // release under their word bit, String members unconditionally, and shared-
    // capable value members per Shared stake.
    namespace {
        enum class SlotMemberKind { BitGuarded, StringDrop, ValueShared };
        struct SlotMemberRel {
            uint64_t off;
            int bit;
            bool isArray;
            SlotMemberKind kind;
            shared_ptr<CajetaClass> cls;
        };
        // True for cajeta.lang.String.
        bool slotMemberIsString(const shared_ptr<CajetaClass>& c) {
            return c && c->getQName()
                && c->getQName()->getTypeName() == "String"
                && c->getQName()->getPackageName() == "cajeta.lang";
        }
        // The releasable members of one value-struct slot, as byte offsets into the
        // element layout `sl` paired with their release family.
        std::vector<SlotMemberRel> collectSlotMembers(
                const shared_ptr<CajetaClass>& elemCls,
                const llvm::StructLayout* sl) {
            std::vector<SlotMemberRel> out;
            for (const auto& p : elemCls->getPropertyList()) {
                if (p->isStatic()) continue;
                int fi = elemCls->getFieldLlvmIndex(p);
                if (fi < 0) continue;
                uint64_t off = sl->getElementOffset((unsigned) fi);
                if (CajetaClass::fieldHasOwnershipBit(p)) {
                    out.push_back({off, elemCls->ownershipBitIndexOf(p),
                        (bool) dynamic_pointer_cast<CajetaArray>(p->getType()),
                        SlotMemberKind::BitGuarded, nullptr});
                    continue;
                }
                auto mc = dynamic_pointer_cast<CajetaClass>(p->getType());
                if (!mc || dynamic_pointer_cast<CajetaView>(p->getType())) {
                    continue;
                }
                if (slotMemberIsString(mc)) {
                    out.push_back({off, -1, false,
                        SlotMemberKind::StringDrop, nullptr});
                } else if (mc->isValueType() && mc->isSharedCapableValue()) {
                    out.push_back({off, -1, false,
                        SlotMemberKind::ValueShared, mc});
                }
            }
            return out;
        }
    }

    // True when a value-type array element needs the per-slot member teardown walk:
    // it has an ownership word, or a String / shared-value member.
    bool CajetaClass::arrayElementCarriesMemberBits(const CajetaTypePtr& elem) {
        if (!elem) return false;
        if (dynamic_pointer_cast<CajetaArray>(elem)) return false;
        if (dynamic_pointer_cast<CajetaView>(elem)) return false;
        auto cls = dynamic_pointer_cast<CajetaClass>(elem);
        if (!cls) return false;
        if (cls->isInterface() || !cls->isValueType()) return false;
        if (cls->isSharedCapableValue()) return false;
        if (cls->needsOwnershipWord()) return true;
        for (const auto& p : cls->getPropertyList()) {
            if (p->isStatic()) continue;
            auto mc = dynamic_pointer_cast<CajetaClass>(p->getType());
            if (!mc || dynamic_pointer_cast<CajetaView>(p->getType())) {
                continue;
            }
            if (slotMemberIsString(mc)
                    || (mc->isValueType() && mc->isSharedCapableValue())) {
                return true;
            }
        }
        return false;
    }

    // Emits (or reuses) a function walking every slot of an array of value structs
    // and releasing each slot's members, optionally freeing the buffer afterwards.
    // `headerBytes` and `elemStride` are the array header size and element stride.
    llvm::Function* CajetaClass::getOrCreateMemberWalk(
            const CajetaModulePtr& module, llvm::Module* m,
            const shared_ptr<CajetaClass>& elemCls, uint64_t headerBytes,
            uint64_t elemStride, bool withFree) {
        if (!m || !elemCls) return nullptr;
        auto* declTy = llvm::dyn_cast_or_null<llvm::StructType>(
            elemCls->getLlvmType());
        int wordIdx = elemCls->getOwnershipWordLlvmIndex();
        if (!declTy || declTy->isOpaque()) return nullptr;
        std::string name = std::string(withFree ? "cajeta_member_dropfree$"
                                                : "cajeta_member_walk$")
            + elemCls->toCanonical();
        if (llvm::Function* f = m->getFunction(name)) return f;

        const llvm::DataLayout& dl = m->getDataLayout();
        const llvm::StructLayout* sl = dl.getStructLayout(declTy);
        std::vector<SlotMemberRel> members = collectSlotMembers(elemCls, sl);
        if (members.empty()) return nullptr;
        bool anyBitGuarded = false;
        for (const auto& mr : members) {
            if (mr.kind == SlotMemberKind::BitGuarded) anyBitGuarded = true;
        }
        if (anyBitGuarded && wordIdx < 0) return nullptr;
        uint64_t wordOff = wordIdx >= 0
            ? sl->getElementOffset((unsigned) wordIdx) : 0;

        auto& ctx = m->getContext();
        auto* i64 = llvm::Type::getInt64Ty(ctx);
        auto* i8 = llvm::Type::getInt8Ty(ctx);
        auto* ptrTy = llvm::PointerType::get(ctx, 0);
        auto* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {ptrTy}, false);
        auto* f = llvm::Function::Create(
            fnTy, llvm::Function::InternalLinkage, name, m);
        llvm::Function* dropFn = module->getRuntimeFunction(
            "__cajeta_class_virtual_drop", m);
        llvm::Function* freeFn = module->getRuntimeFunction(
            "__cajeta_free_array", m);

        auto* entry = llvm::BasicBlock::Create(ctx, "entry", f);
        auto* head = llvm::BasicBlock::Create(ctx, "slot_head", f);
        auto* body = llvm::BasicBlock::Create(ctx, "slot_body", f);
        auto* next = llvm::BasicBlock::Create(ctx, "slot_next", f);
        auto* done = llvm::BasicBlock::Create(ctx, "done", f);
        llvm::IRBuilder<> b(entry);
        llvm::Value* hdr = f->getArg(0);
        llvm::Value* isNull = b.CreateICmpEQ(
            hdr, llvm::ConstantPointerNull::get(ptrTy));
        auto* haveHdr = llvm::BasicBlock::Create(ctx, "have_hdr", f);
        b.CreateCondBr(isNull, done, haveHdr);
        b.SetInsertPoint(haveHdr);
        llvm::Value* count = b.CreateAnd(
            b.CreateLoad(i64, hdr, "count"),
            llvm::ConstantInt::get(i64, 0x7fffffffffffffffULL));
        llvm::Value* idxSlot = b.CreateAlloca(i64, nullptr, "i");
        b.CreateStore(llvm::ConstantInt::get(i64, 0), idxSlot);
        b.CreateBr(head);

        b.SetInsertPoint(head);
        llvm::Value* i = b.CreateLoad(i64, idxSlot);
        b.CreateCondBr(b.CreateICmpSLT(i, count), body, done);

        b.SetInsertPoint(body);
        llvm::Value* slotBase = b.CreateInBoundsGEP(i8, hdr,
            b.CreateAdd(llvm::ConstantInt::get(i64, headerBytes),
                        b.CreateMul(i, llvm::ConstantInt::get(
                            i64, elemStride))), "slot_base");
        llvm::Value* wordPtr = nullptr;
        llvm::Value* word = nullptr;
        if (anyBitGuarded) {
            wordPtr = b.CreateInBoundsGEP(i8, slotBase,
                llvm::ConstantInt::get(i64, wordOff), "word_ptr");
            word = b.CreateLoad(i64, wordPtr, "word");
        }
        for (const auto& mr : members) {
            llvm::Value* mp = b.CreateInBoundsGEP(i8, slotBase,
                llvm::ConstantInt::get(i64, mr.off), "member_ptr");
            if (mr.kind == SlotMemberKind::ValueShared) {
                mr.cls->emitValueSharedOp(b, mp, module, m,
                                          /*retain=*/false);
                continue;
            }
            if (mr.kind == SlotMemberKind::StringDrop) {
                llvm::Value* sv = b.CreateLoad(ptrTy, mp, "member_str");
                if (dropFn) b.CreateCall(dropFn, {sv});
                b.CreateStore(llvm::ConstantPointerNull::get(ptrTy), mp);
                continue;
            }
            llvm::Value* bit = b.CreateAnd(
                b.CreateLShr(word, llvm::ConstantInt::get(i64, mr.bit)),
                llvm::ConstantInt::get(i64, 1));
            llvm::Function* fn = b.GetInsertBlock()->getParent();
            auto* relBB = llvm::BasicBlock::Create(ctx, "member_rel", fn);
            auto* contBB = llvm::BasicBlock::Create(ctx, "member_cont", fn);
            b.CreateCondBr(b.CreateICmpNE(bit,
                llvm::ConstantInt::get(i64, 0)), relBB, contBB);
            b.SetInsertPoint(relBB);
            llvm::Value* mv = b.CreateLoad(ptrTy, mp, "member_val");
            llvm::Function* rel = mr.isArray ? freeFn : dropFn;
            if (rel) b.CreateCall(rel, {mv});
            b.CreateBr(contBB);
            b.SetInsertPoint(contBB);
        }
        // Zero the word so a second pass (dtor plus auto field drop) cannot
        // re-release these members.
        if (wordPtr) {
            b.CreateStore(llvm::ConstantInt::get(i64, 0), wordPtr);
        }
        b.CreateBr(next);

        b.SetInsertPoint(next);
        b.CreateStore(b.CreateAdd(i, llvm::ConstantInt::get(i64, 1)),
                      idxSlot);
        b.CreateBr(head);

        b.SetInsertPoint(done);
        if (withFree && freeFn) b.CreateCall(freeFn, {hdr});
        b.CreateRetVoid();
        return f;
    }

    // Fills this class's opaque named struct with the instance layout: per-parent
    // sub-object embedding, hidden ownership words, vbase slots. PURE layout with
    // no registration, so a frozen class can rebuild it in any context.
    void CajetaClass::buildInstanceStructBody(llvm::LLVMContext* lctx) {
        string canonical = qName->toCanonical();
        auto fieldLayoutType = [&](const StructurePropertyPtr& p) -> llvm::Type* {
            CajetaTypePtr t = p->getType();
            if (!t) {
                throw Exception(
                    "unknown type for field '" + p->getName()
                        + "' in '" + toCanonical()
                        + "'; not a primitive, native, or user-defined type",
                    "CAJETA_ERROR_UNKNOWN_TYPE");
            }
            if (auto arr = dynamic_pointer_cast<CajetaArray>(t)) {
                if (arr->isInlineArray()) {
                    return arr->getInlineLlvmType(lctx);
                }
                return llvm::PointerType::get(*lctx, 0);
            }
            if (auto cls = dynamic_pointer_cast<CajetaClass>(t)) {
                if (dynamic_pointer_cast<CajetaView>(t)) {
                    return t->getLlvmType();
                }
                if (cls->isInterface()) {
                    return t->getLlvmType();
                }
                if (cls->isValueType()) {
                    return t->getLlvmType();
                }
                return llvm::PointerType::get(*lctx, 0);
            }
            return t->getLlvmType();
        };

        // Layout for `C extends A, B`: { vtable, A's content (shares that vtable),
        // secondary vtable for B, B's content, C's own fields }. Only the FIRST
        // parent shares the primary vtable; subObjectSlotMap records each start.
        vector<llvm::Type*> llvmMembers;
        subObjectSlotMap.clear();
        llvm::PointerType* vptrTy = llvm::PointerType::get(*lctx, 0);

        vbaseAncestors.clear();
        vbaseSlotMap.clear();
        auto selfRaw = static_cast<const CajetaClass*>(this);

        auto isObject = [](const CajetaClass* c) {
            if (!c) return false;
            auto qn = c->getQName();
            return qn && qn->getTypeName() == "Object"
                && qn->getPackageName() == "cajeta.lang";
        };
        auto collectAncestors = [&](CajetaClassPtr cls) {
            std::vector<CajetaClassPtr> result;
            std::set<const CajetaClass*> seen;
            std::function<void(CajetaClassPtr)> walk;
            walk = [&](CajetaClassPtr c) {
                if (!c) return;
                for (auto& parent : c->superClasses) {
                    if (!parent) continue;
                    if (isObject(parent.get())) continue;
                    if (seen.insert(parent.get()).second) {
                        result.push_back(parent);
                    }
                    walk(parent);
                }
            };
            walk(cls);
            return result;
        };

        std::function<void(CajetaClassPtr, bool, int)> embedSubObject;
        embedSubObject = [&](CajetaClassPtr cls, bool ownVtable, int enclosingStart) {
            int subObjectStart;
            if (ownVtable) {
                subObjectStart = (int) llvmMembers.size();
                llvmMembers.push_back(vptrTy);
            } else {
                subObjectStart = enclosingStart;
            }
            if (subObjectSlotMap.find(cls.get()) == subObjectSlotMap.end()) {
                subObjectSlotMap[cls.get()] = subObjectStart;
            }

            int idx = 0;
            for (auto& parent : cls->superClasses) {
                embedSubObject(parent, /*ownVtable=*/(idx != 0), subObjectStart);
                idx++;
            }
            for (auto& p : cls->propertyList) {
                if (p->isStatic()) continue;
                llvmMembers.push_back(fieldLayoutType(p));
            }
            // Hidden per-instance ownership word: after cls's own properties and
            // before its vbase slots. getFieldLlvmIndex mirrors this position.
            {
                bool wantsWord = false;
                for (auto& p : cls->propertyList) {
                    if (!p->isStatic() && fieldHasOwnershipBit(p)) {
                        wantsWord = true;
                        break;
                    }
                }
                if (wantsWord) {
                    llvmMembers.push_back(llvm::Type::getInt64Ty(*lctx));
                }
            }
            // One vbase pointer per transitive non-self ancestor, after cls's own
            // properties so field GEP indices stay stable. Value types get none.
            if (cls->isValueType()) return;
            auto ancestors = collectAncestors(cls);
            for (auto& anc : ancestors) {
                if (cls.get() == selfRaw) {
                    vbaseSlotMap[anc.get()] = (int) llvmMembers.size();
                    vbaseAncestors.push_back(anc);
                }
                llvmMembers.push_back(vptrTy);
            }
        };
        if (!dynamic_pointer_cast<CajetaView>(shared_from_this())) {
            for (auto& p : propertyList) {
                if (!p || p->isStatic()) continue;
                CajetaTypePtr t = p->getType();
                if (!t) continue;
                CajetaTypePtr violatingView;
                if (dynamic_pointer_cast<CajetaView>(t)) {
                    violatingView = t;
                } else if (auto arr = dynamic_pointer_cast<CajetaArray>(t)) {
                    if (dynamic_pointer_cast<CajetaView>(arr->getElementType())) {
                        violatingView = arr->getElementType();
                    }
                }
                if (violatingView) {
                    std::string viewName = violatingView->getQName()
                        ? violatingView->getQName()->toCanonical()
                        : "<unnamed-view>";
                    throw Exception(
                        "view type '" + viewName + "' cannot be used as a "
                        "class field (class '" + canonical + "', field '"
                        + p->getName() + "'). Views are buffer overlays with "
                        "borrowed lifetime — see docs/specification/lang/Views.md "
                        "(Errors caught statically). Workaround: store the "
                        "underlying byte[] in the class and construct the "
                        "view per access; or pass the view by value across "
                        "method boundaries without storing it.",
                        "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
                }
            }
        }

        embedSubObject(static_pointer_cast<CajetaClass>(shared_from_this()),
            /*ownVtable=*/hasVtablePointerAtSlotZero(), /*enclosingStart=*/0);

        // getLlvmType()'s on-demand build may have filled this body already, and
        // setBody on a non-opaque struct asserts, so the first fill wins.
        auto* bodyStruct = (llvm::StructType*) rawLlvmType();
        if (bodyStruct->isOpaque()) {
            bodyStruct->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);
        }
    }

    namespace {

        // Structural fingerprint of ONE class declaration: fields in declaration
        // order with their type canonicals, then the sorted method-signature set.
        // The parent prefix through the first "::" is stripped (it holds the suffix).
        std::string sessionDeclarationShape(CajetaClass* klass) {
            std::string out = "f:";
            for (auto& prop : klass->getPropertyList()) {
                if (!prop) continue;
                out += prop->getName();
                out += ':';
                CajetaTypePtr pt = prop->getType();
                out += (pt && pt->getQName()) ? pt->getQName()->toCanonical()
                                              : std::string("<?>");
                out += ';';
            }
            std::vector<std::string> signatures;
            for (auto& entry : klass->getMethods()) {
                const std::string& key = entry.first;
                size_t sep = key.find("::");
                signatures.push_back(
                    sep == std::string::npos ? key : key.substr(sep + 2));
            }
            std::sort(signatures.begin(), signatures.end());
            out += "m:";
            for (auto& s : signatures) {
                out += s;
                out += ';';
            }
            return out;
        }

    }

    // Builds this class as a type: registers it, lays out its instance struct, runs
    // the annotation synthesizers, prototypes every method, then writes the vtables.
    // Idempotent; templates and interfaces take short paths and return early.
    void CajetaClass::generatePrototype() {
        if (prototypeBuilt) return;
        CajetaModulePtr* moduleSlot = &module;
        CajetaModulePtr savedModule = module;
        if (emitModule && emitModule != module) module = emitModule;
        struct RestoreModule {
            CajetaModulePtr* slot; CajetaModulePtr saved;
            ~RestoreModule() { *slot = saved; }
        } restoreModule{moduleSlot, savedModule};

        if (isTemplate()) {
            canonicalMap[qName->toCanonical()] = static_pointer_cast<CajetaType>(shared_from_this());
            canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
            return;
        }
        string canonical = qName->toCanonical();

        if (isInterface()) {
            // Interface fat pointer: { ptr data, ptr vtable, i64 kind } = 24 bytes.
            // kind is IFACE_KIND_BORROWED_CLASS / OWNED_CLASS and drives the drop.
            llvm::Type* lt = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
            setLlvmType(lt);
            typeMap[TypeKey(lt)] = shared_from_this();
            if (((llvm::StructType*) lt)->isOpaque()) {
                llvm::Type* ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(*module->getLlvmContext());
                vector<llvm::Type*> members{ ptrTy, ptrTy, i64Ty };
                ((llvm::StructType*) lt)->setBody(llvm::ArrayRef<llvm::Type*>(members), false);
            }

            canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
            canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
            typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
            module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
            resolveSuperClasses();
            for (auto& m : methods) {
                m.second->generatePrototype();
            }
            CajetaModule::getStructureToModule()[canonical] = module;
            prototypeBuilt = true;
            return;
        }

        string structName = canonical;
        if (SessionState* session = module ? module->getSessionState() : nullptr) {
            const string shape = sessionDeclarationShape(this);
            const DeclaredClass* prior = session->declaredClass(canonical);
            bool singleVTable = qImplemented.empty();
            for (auto& sup : qExtended) {
                if (sup && sup->getTypeName() != "Object") {
                    singleVTable = false;
                    break;
                }
            }
            if (singleVTable) {
                const string shortName = qName->getTypeName();
                for (auto& declared : session->declaredClassNames()) {
                    if (declared == canonical) continue;
                    auto other = std::dynamic_pointer_cast<CajetaClass>(
                        CajetaType::find(declared));
                    if (!other || other.get() == this) continue;
                    for (auto& sup : other->getQExtended()) {
                        if (!sup) continue;
                        if (sup->toCanonical() == canonical
                                || sup->getTypeName() == shortName) {
                            singleVTable = false;
                            break;
                        }
                    }
                    if (!singleVTable) break;
                }
            }
            if (prior && prior->shape == shape && singleVTable) {
                structName = canonical + prior->suffix;
                setGenerationSuffix(prior->suffix);
                session->noteBodyOnlyRedefinition(canonical);
            } else if (prior) {
                auto* ctx = module->getLlvmContext();
                for (int generation = 2; ; ++generation) {
                    string candidate =
                        canonical + "$g" + std::to_string(generation);
                    llvm::StructType* st =
                        llvm::StructType::getTypeByName(*ctx, candidate);
                    if (!st || st->isOpaque()) {
                        structName = candidate;
                        setGenerationSuffix("$g" + std::to_string(generation));
                        break;
                    }
                }
            }
            session->noteDeclaredClass(canonical, shape, getGenerationSuffix());
        }
        setLlvmType(CajetaType::getOrCreateLlvmType(module->getLlvmContext(), structName));
        typeMap[TypeKey(rawLlvmType())] = shared_from_this();
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
        if (findAnnotation("ValueType")) {
            typeFlags |= VALUE_TYPE_FLAG | BY_VALUE_FLAG;
        }
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));

        module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
        resolveSuperClasses();
        resolveImplementedInterfaces();

        buildInstanceStructBody(module->getLlvmContext());

        // The ctor synthesizers must run BEFORE ensureDefaultConstructor, so a
        // synthesized constructor suppresses the implicit default.
        synthesizeMockFields();
        synthesizeNoArgsConstructor();
        synthesizeAllArgsConstructor();
        synthesizeRequiredArgsConstructor();
        ensureDefaultConstructor();
        synthesizeAutoHash();
        synthesizeGetters();
        synthesizeSetters();
        synthesizeToString();
        synthesizeWith();
        // BEFORE the method-prototype loop and writeVirtualTable, so the ctor and
        // toBytes() @Encoding adds are prototyped and reach the vtable.
        synthesizeEncoding();

        for (auto methodEntry: methods) {
            methodEntry.second->generatePrototype();
        }

        // AFTER every method has its LLVM Function: each vtable slot's constant
        // needs getLlvmFunction() to be non-null.
        writeVirtualTable();

        synthesizeInterfaceVTables();

        CajetaModule::getStructureToModule()[canonical] = module;

        bool ownerIsStdlib = qName
            && (qName->getPackageName() == "cajeta"
                || qName->getPackageName().rfind("cajeta.", 0) == 0);
        if (!dynamic_pointer_cast<CajetaView>(shared_from_this())
                && !isWildcardInstantiation()
                && !ownerIsStdlib
                && !isLintSuppressed("wildcard-field-in-small-class")) {
            for (auto& prop : propertyList) {
                auto propType = prop->getType();
                auto propClass = dynamic_pointer_cast<CajetaClass>(propType);
                if (propClass && propClass->isWildcardInstantiation()) {
                    std::ostringstream w;
                    w << "warning: [wildcard-field-in-small-class] "
                        << "class " << qName->toCanonical()
                        << " declares wildcard-typed field '"
                        << prop->getName() << "' of type "
                        << propClass->toCanonical()
                        << " — every drop of an instance routes through "
                        << "virtual-drop dispatch; if instances of this "
                        << "class are constructed in a hot path, pick a "
                        << "concrete element type or push the wildcard "
                        << "outward. Suppress with "
                        << "@SuppressLint(\"wildcard-field-in-small-class\").\n";
                    logLine("warn", w.str());
                }
            }
        }

        prototypeBuilt = true;

        // LAST: both synthesizers walk a FRESH CajetaClass through generatePrototype,
        // which would re-enter this class's half-built state if run earlier.
        synthesizeBuilder();

        synthesizeMock();
    }

    // @GenerateMock: fill (or create) `Mock<SimpleName>` extending this class,
    // synthesize its body, prototype it, and add it to the module's roster.
    void CajetaClass::synthesizeMock() {
        auto ann = findAnnotation("GenerateMock");
        if (!ann) return;

        std::string mockTypeName = std::string("Mock") + qName->getTypeName();
        auto mockQName = QualifiedName::getOrInsert(
            mockTypeName, qName->getPackageName());

        CajetaClassPtr mock;
        auto& canonicalMap = CajetaType::getCanonicalMap();
        auto it = canonicalMap.find(mockQName->toCanonical());
        if (it != canonicalMap.end()) {
            mock = std::dynamic_pointer_cast<CajetaClass>(it->second);
        }
        if (!mock) {
            std::list<QualifiedNamePtr> mockExtends{ qName };
            std::list<QualifiedNamePtr> mockImplements;
            mock = std::make_shared<CajetaClass>(
                module, mockQName, mockExtends, mockImplements);
            canonicalMap[mockQName->toCanonical()] =
                std::static_pointer_cast<CajetaType>(mock);
        }

        std::list<QualifiedNamePtr> mockExtends{ qName };
        std::list<QualifiedNamePtr> mockImplements;
        mock->fillFromDeclaration(module, mockQName, mockExtends, mockImplements);

        fillMockClassBody(
            mock, std::static_pointer_cast<CajetaClass>(shared_from_this()),
            module);

        mock->generatePrototype();

        module->getStructures()[mockQName->toCanonical()] = mock;
        CajetaModule::getStructureToModule()[mockQName->toCanonical()] = module;
    }

    // Field-level @Mock: retype each annotated field to Mock<T> and add an init
    // constructor that populates them.
    void CajetaClass::synthesizeMockFields() {
        std::vector<std::pair<std::string, std::string>> inits;
        auto& canonicalMap = CajetaType::getCanonicalMap();

        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            if (!prop->findAnnotation("Mock")) continue;

            auto origType = prop->getType();
            if (!origType || !origType->getQName()) continue;
            auto origQ = origType->getQName();

            std::string mockSimple = std::string("Mock") + origQ->getTypeName();
            auto mockQName = QualifiedName::getOrInsert(
                mockSimple, origQ->getPackageName());
            std::string mockCanon = mockQName->toCanonical();

            CajetaClassPtr mockType;
            auto it = canonicalMap.find(mockCanon);
            if (it != canonicalMap.end()) {
                mockType = std::dynamic_pointer_cast<CajetaClass>(it->second);
            }
            if (!mockType) {
                std::list<QualifiedNamePtr> ext{ origQ };
                std::list<QualifiedNamePtr> impl;
                mockType = std::make_shared<CajetaClass>(
                    module, mockQName, ext, impl);
                canonicalMap[mockCanon] =
                    std::static_pointer_cast<CajetaType>(mockType);
            }

            prop->setType(std::static_pointer_cast<CajetaType>(mockType));
            inits.push_back({ prop->getName(), mockCanon });
        }

        if (!inits.empty()) {
            addMockFieldInitCtor(
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                inits, module);
        }
    }

    // This interface's methods followed by its parents', in BFS order, the first
    // occurrence of a name winning (the leaf override). Constructors and statics are
    // filtered. synthesizeInterfaceVTables emits its slots in exactly this order.
    std::vector<MethodPtr> CajetaClass::getFlattenedInterfaceMethods() {
        std::vector<MethodPtr> ordered;
        std::set<std::string> seenNames;
        std::set<CajetaClass*> visited;
        std::vector<CajetaClassPtr> frontier{
            std::static_pointer_cast<CajetaClass>(shared_from_this()) };
        size_t cursor = 0;
        while (cursor < frontier.size()) {
            auto iface = frontier[cursor++];
            if (!iface || !visited.insert(iface.get()).second) continue;
            for (auto& m : iface->getMethodList()) {
                if (!m || m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                if (!seenNames.insert(m->getName()).second) continue;
                ordered.push_back(m);
            }
            for (auto& parent : iface->getSuperClasses()) {
                if (parent && parent->isInterface()) frontier.push_back(parent);
            }
        }
        return ordered;
    }

    // Emits one flat `[N x ptr]` vtable global per (this class, interface) pair over
    // the transitive implements closure. Slot 0 holds this class's drop function and
    // the method slots follow in getFlattenedInterfaceMethods order.
    void CajetaClass::synthesizeInterfaceVTables() {
        if (implementedInterfaces.empty()) return;

        auto& interfaceVTables = interfaceVTablesRef();
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        std::string classCanonical = qName->toCanonical();

        auto sanitize = [](std::string s) {
            for (char& c : s) {
                if (c == ':' || c == '.' || c == '<' || c == '>'
                        || c == ',' || c == ' ') {
                    c = '_';
                }
            }
            return s;
        };

        std::set<CajetaClass*> visited;
        std::vector<CajetaClassPtr> allIfaces;
        std::function<void(CajetaClassPtr)> collect = [&](CajetaClassPtr iface) {
            if (!iface || !visited.insert(iface.get()).second) return;
            allIfaces.push_back(iface);
            for (auto& parent : iface->getSuperClasses()) {
                if (parent && parent->isInterface()) collect(parent);
            }
        };
        for (auto& direct : implementedInterfaces) collect(direct);

        pendingIfaceVTables = false;
        for (auto& iface : allIfaces) {
            if (iface->isPlaceholder()) {
                pendingIfaceVTables = true;
                continue;
            }
            std::string ifaceCanonical = iface->getQName()->toCanonical();

            auto findByName = [&](const std::string& name) -> MethodPtr {
                for (auto& [canon, m] : methods) {
                    if (!m || m->isConstructor()) continue;
                    if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                    if (m->getName() == name) return m;
                }
                for (auto& parent : superClasses) {
                    for (auto& [canon, m] : parent->getMethods()) {
                        if (!m || m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                        if (m->getName() == name) return m;
                    }
                }
                return nullptr;
            };

            std::vector<llvm::Constant*> entries;
            if (llvm::Function* dropFn = CajetaModule::ensureFunctionInModule(
                    lmod, this->getOrCreateDropFunction())) {
                entries.push_back(dropFn);
            } else {
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            for (auto& ifaceMethod : iface->getFlattenedInterfaceMethods()) {
                if (!ifaceMethod || ifaceMethod->isConstructor()) continue;
                if (ifaceMethod->getModifiers().find(STATIC)
                        != ifaceMethod->getModifiers().end()) continue;
                MethodPtr concrete = findByName(ifaceMethod->getName());
                if (!concrete || !concrete->getLlvmFunction()) {
                    std::ostringstream w;
                    w << "warning: [iface-vtable-null-slot] "
                      << classCanonical << " has no built implementation for "
                      << ifaceCanonical << "::" << ifaceMethod->getName()
                      << (concrete ? " (found, LLVM function unbuilt)"
                                   : " (no same-name concrete method)")
                      << " — dispatch through this interface slot will crash\n";
                    logLine("warn", w.str());
                    entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
                    continue;
                }
                entries.push_back(CajetaModule::ensureFunctionInModule(
                    lmod, concrete->getLlvmFunction()));
            }

            llvm::ArrayType* arrTy = llvm::ArrayType::get(
                ptrTy, entries.size());
            std::string globalName = std::string("class.")
                + sanitize(classCanonical) + "_iface_"
                + sanitize(ifaceCanonical) + "_VTable";
            if (llvm::GlobalVariable* existing = lmod->getNamedGlobal(globalName)) {
                if (!existing->hasInitializer()
                        && existing->getValueType() == arrTy) {
                    existing->setInitializer(
                        llvm::ConstantArray::get(arrTy, entries));
                    existing->setConstant(true);
                }
                interfaceVTables[ifaceCanonical] = existing;
                continue;
            }

            llvm::Constant* init = llvm::ConstantArray::get(arrTy, entries);
            auto* gv = new llvm::GlobalVariable(
                *lmod, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage, init, globalName);
            interfaceVTables[ifaceCanonical] = gv;
        }
    }

    // @AutoHash, also implied by @Data / @Value: add a structural hash() that walks
    // the fields. A user-declared zero-arg hash() wins and skips synthesis.
    void CajetaClass::synthesizeAutoHash() {
        if (!findAnnotation("AutoHash")
                && !findAnnotation("Data")
                && !findAnnotation("Value")) return;

        for (auto& m : methodList) {
            if (m->getName() == "hash" && m->getParameters().size() == 0) {
                return;
            }
        }
        addMethod(std::make_shared<SynthesizedHashMethod>(
            module,
            std::static_pointer_cast<CajetaClass>(shared_from_this())));
    }

    // Resolves an annotation's `access="..."` to a Modifier; PUBLIC by default.
    // Throws CAJETA_ERROR_ACCESSOR_BAD_ACCESS on an unrecognized value.
    static Modifier resolveAccessModifier(
            const AnnotationInstancePtr& ann,
            const std::string& annotationName,
            const std::string& classCanonical) {
        if (!ann) return PUBLIC;
        std::string v = ann->getString("access");
        if (v.empty()) return PUBLIC;
        if (v == "public")    return PUBLIC;
        if (v == "private")   return PRIVATE;
        if (v == "protected") return PROTECTED;
        if (v == "package")   return PACKAGE;
        throw Exception(
            "@" + annotationName + "(access=\"" + v + "\") on `"
            + classCanonical + "` is not a recognized access level; "
            "supported: \"public\" (default), \"private\", "
            "\"protected\", \"package\"",
            "CAJETA_ERROR_ACCESSOR_BAD_ACCESS");
    }

    // @Getter at class or field level, also implied by @Data / @Value: add a `name()`
    // accessor per non-static field. A user-declared zero-arg method of that name
    // wins, and a field-level `access` overrides the class-level default.
    void CajetaClass::synthesizeGetters() {
        auto classAnn = findAnnotation("Getter");
        bool classLevel = classAnn != nullptr
                       || findAnnotation("Data")   != nullptr
                       || findAnnotation("Value")  != nullptr;

        Modifier classAccess = resolveAccessModifier(
            classAnn, "Getter", qName->toCanonical());

        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            auto fieldAnn = prop->findAnnotation("Getter");
            bool fieldLevel = fieldAnn != nullptr;
            if (!classLevel && !fieldLevel) continue;

            bool exists = false;
            for (auto& m : methodList) {
                if (!m || m->isConstructor()) continue;
                if (m->getName() != prop->getName()) continue;
                if (m->getParameters().size() != 0) continue;
                exists = true;
                break;
            }
            if (exists) continue;

            Modifier access = fieldLevel
                ? resolveAccessModifier(fieldAnn, "Getter", qName->toCanonical())
                : classAccess;

            auto getter = std::make_shared<SynthesizedGetterMethod>(
                module,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                prop);
            getter->addModifier(access);
            addMethod(getter);
        }
    }

    // @Setter at class or field level, also implied by @Data: add a one-argument
    // setter per non-static, non-final field. @Value suppresses setters entirely,
    // and a user-declared same-name one-argument method wins.
    void CajetaClass::synthesizeSetters() {
        if (findAnnotation("Value")) return;
        auto classAnn = findAnnotation("Setter");
        bool classLevel = classAnn != nullptr
                       || findAnnotation("Data")   != nullptr;
        Modifier classAccess = resolveAccessModifier(
            classAnn, "Setter", qName->toCanonical());

        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            if (prop->getModifiers().find(FINAL) != prop->getModifiers().end()) continue;
            auto fieldAnn = prop->findAnnotation("Setter");
            bool fieldLevel = fieldAnn != nullptr;
            if (!classLevel && !fieldLevel) continue;

            bool exists = false;
            for (auto& m : methodList) {
                if (!m || m->isConstructor()) continue;
                if (m->getName() != prop->getName()) continue;
                auto params = m->getParameterList();
                size_t userArgs = params.size();
                if (!params.empty() && params.front()
                        && params.front()->getName() == "this") {
                    userArgs--;
                }
                if (userArgs != 1) continue;
                exists = true;
                break;
            }
            if (exists) continue;

            Modifier access = fieldLevel
                ? resolveAccessModifier(fieldAnn, "Setter", qName->toCanonical())
                : classAccess;

            auto setter = std::make_shared<SynthesizedSetterMethod>(
                module,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                prop);
            setter->addModifier(access);
            // initParameter needs the shared_ptr to exist: FormalParameter.parent
            // is read during RTTI build, and a null there segfaults.
            setter->initParameter();
            addMethod(setter);
        }
    }

    // @ToString, also implied by @Data / @Value: add a `String toString()` over the
    // non-static, non-excluded fields. `format=` picks PROPERTIES or JSON and
    // `of={...}` pins the rendered set; a user-declared toString() wins.
    void CajetaClass::synthesizeToString() {
        auto ann = findAnnotation("ToString");
        bool bundleEnabled = findAnnotation("Data") || findAnnotation("Value");
        if (!ann && !bundleEnabled) return;

        std::string format = ann ? ann->getString("format") : std::string();
        ToStringFormat resolvedFormat = ToStringFormat::PROPERTIES;
        if (format == "TO_STRING_JSON") {
            resolvedFormat = ToStringFormat::JSON;
        } else if (!format.empty() && format != "TO_STRING_PROPERTIES") {
            throw Exception(
                "@ToString(format=" + format + ") on `"
                + qName->toCanonical()
                + "` is not a recognized format; supported in v1: "
                "TO_STRING_PROPERTIES, TO_STRING_JSON",
                "CAJETA_ERROR_TOSTRING_BAD_FORMAT");
        }

        for (auto& m : methodList) {
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "toString") continue;
            auto params = m->getParameterList();
            size_t userArgs = params.size();
            if (!params.empty() && params.front()
                    && params.front()->getName() == "this") {
                userArgs--;
            }
            if (userArgs != 0) continue;
            return;
        }

        std::vector<StructurePropertyPtr> selectedFields;
        bool hasOfList = ann && ann->findArg("of") != nullptr;
        if (hasOfList) {
            const auto& ofNames = ann->getStringList("of");
            for (auto& name : ofNames) {
                StructurePropertyPtr match;
                for (auto& prop : propertyList) {
                    if (!prop || prop->isStatic()) continue;
                    if (prop->getName() == name) { match = prop; break; }
                }
                if (!match) {
                    std::string hint;
                    if (module && module->getFlags().diagHints) {
                        std::vector<std::string> candidates;
                        candidates.reserve(propertyList.size());
                        for (auto& prop : propertyList) {
                            if (!prop || prop->isStatic()) continue;
                            candidates.push_back(prop->getName());
                        }
                        auto suggestions = pickSimilar(name, candidates);
                        hint = formatDidYouMean(suggestions);
                    }
                    throw Exception(
                        "@ToString(of={...}) on `"
                        + qName->toCanonical()
                        + "` names unknown field `" + name + "`."
                        + hint
                        + " Allowlisted field names must match a "
                        "non-static field declared on the class.",
                        "CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD");
                }
                selectedFields.push_back(match);
            }
        }

        bool callSuper = ann ? ann->getBool("callSuper", false) : false;

        addMethod(std::make_shared<SynthesizedToStringMethod>(
            module,
            std::static_pointer_cast<CajetaClass>(shared_from_this()),
            resolvedFormat,
            std::move(selectedFields),
            hasOfList,
            callSuper));
    }

    // True when the unlabeled constructor map already holds a constructor taking
    // `userArgs` user-visible arguments (the implicit `this` excluded).
    bool CajetaClass::ctorWithArityExists(size_t userArgs) const {
        for (auto& bucket : unlabeledConstructorMap) {
            for (auto& entry : bucket.second) {
                MethodPtr m = entry.second;
                if (!m) continue;
                auto params = m->getParameterList();
                size_t got = params.size();
                if (!params.empty() && params.front()
                        && params.front()->getName() == "this") {
                    got--;
                }
                if (got == userArgs) return true;
            }
        }
        return false;
    }

    // Adds a synthesized constructor over `fields`. With `staticName` set the ctor
    // is forced PRIVATE and `access` applies instead to a static factory of that
    // name emitted beside it (Lombok parity).
    void CajetaClass::emitCtorAndOptionalFactory(
            const AnnotationInstancePtr& ann,
            const std::string& annotationName,
            std::vector<StructurePropertyPtr> fields,
            Modifier access) {
        std::string staticName = ann ? ann->getString("staticName") : "";
        Modifier ctorAccess = staticName.empty() ? access : PRIVATE;
        Modifier factoryAccess = staticName.empty() ? access : access;
        (void) annotationName;

        auto self = std::static_pointer_cast<CajetaClass>(shared_from_this());
        auto fieldsForFactory = fields;
        auto ctor = std::make_shared<SynthesizedConstructorMethod>(
            module, self, std::move(fields));
        ctor->addModifier(ctorAccess);
        ctor->initParameters();
        addMethod(ctor);

        if (!staticName.empty()) {
            auto factory = std::make_shared<SynthesizedStaticFactoryMethod>(
                module, self, ctor, staticName,
                std::move(fieldsForFactory));
            factory->addModifier(factoryAccess);
            factory->initParameters();
            addMethod(factory);
        }
    }

    // @NoArgsConstructor: add a zero-argument constructor unless one already exists.
    void CajetaClass::synthesizeNoArgsConstructor() {
        auto ann = findAnnotation("NoArgsConstructor");
        if (!ann) return;
        if (ctorWithArityExists(0)) return;
        Modifier access = resolveAccessModifier(
            ann, "NoArgsConstructor", qName->toCanonical());
        emitCtorAndOptionalFactory(ann, "NoArgsConstructor",
            std::vector<StructurePropertyPtr>{}, access);
    }

    // @AllArgsConstructor, also implied by @Value / @Builder: add a constructor over
    // every non-static field, unless one of that arity already exists.
    void CajetaClass::synthesizeAllArgsConstructor() {
        auto ann = findAnnotation("AllArgsConstructor");
        if (!ann
                && !findAnnotation("Value")
                && !findAnnotation("Builder")) return;
        Modifier access = resolveAccessModifier(
            ann, "AllArgsConstructor", qName->toCanonical());
        std::vector<StructurePropertyPtr> fields;
        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            fields.push_back(prop);
        }
        if (ctorWithArityExists(fields.size())) return;
        emitCtorAndOptionalFactory(ann, "AllArgsConstructor",
            std::move(fields), access);
    }

    // @RequiredArgsConstructor, also implied by @Data: add a constructor over the
    // `final` and @NonNull fields, unless one of that arity already exists.
    void CajetaClass::synthesizeRequiredArgsConstructor() {
        auto ann = findAnnotation("RequiredArgsConstructor");
        if (!ann && !findAnnotation("Data")) return;
        Modifier access = resolveAccessModifier(
            ann, "RequiredArgsConstructor", qName->toCanonical());
        std::vector<StructurePropertyPtr> fields;
        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            bool isFinal = prop->getModifiers().find(FINAL)
                != prop->getModifiers().end();
            bool isNonNull = prop->findAnnotation("NonNull") != nullptr;
            if (!isFinal && !isNonNull) continue;
            fields.push_back(prop);
        }
        if (ctorWithArityExists(fields.size())) return;
        emitCtorAndOptionalFactory(ann, "RequiredArgsConstructor",
            std::move(fields), access);
    }

    // @With at class or field level: add a `withField(v)` copy-setter per non-static
    // field. A user-declared same-name one-argument method wins.
    void CajetaClass::synthesizeWith() {
        bool classLevel = findAnnotation("With") != nullptr;

        auto buildName = [](const std::string& fieldName) {
            if (fieldName.empty()) return std::string("with");
            std::string out = "with";
            out += (char) std::toupper((unsigned char) fieldName[0]);
            if (fieldName.size() > 1) out.append(fieldName.substr(1));
            return out;
        };

        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            bool fieldLevel = prop->findAnnotation("With") != nullptr;
            if (!classLevel && !fieldLevel) continue;

            std::string methodName = buildName(prop->getName());

            bool exists = false;
            for (auto& m : methodList) {
                if (!m || m->isConstructor()) continue;
                if (m->getName() != methodName) continue;
                auto params = m->getParameterList();
                size_t got = params.size();
                if (!params.empty() && params.front()
                        && params.front()->getName() == "this") {
                    got--;
                }
                if (got != 1) continue;
                exists = true;
                break;
            }
            if (exists) continue;

            auto with = std::make_shared<SynthesizedWithMethod>(
                module,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                prop);
            with->initParameter();
            addMethod(with);
        }
    }

    // @Encoding(E.class): add the `T(byte[])` constructor and `byte[] toBytes()` that
    // delegate to E. Rejects packed-layout annotations alongside it, and an
    // `implements Encoder<U>` on E whose U is not this class.
    void CajetaClass::synthesizeEncoding() {
        auto encAnn = findAnnotation("Encoding");
        if (!encAnn) return;

        const char* conflictingAnns[] = {
            "BigEndian", "LittleEndian", "HostEndian", "Align"
        };
        for (const char* name : conflictingAnns) {
            if (findAnnotation(name)) {
                throw Exception(
                    "@Encoding on `" + qName->toCanonical()
                    + "` cannot coexist with @" + std::string(name)
                    + " — @Encoding controls the wire layout entirely; "
                    "remove the endianness/alignment annotations.",
                    "CAJETA_ERROR_ENCODING_CONFLICT");
            }
        }

        std::string encoderName = encAnn->getClassRef("value");
        if (encoderName.empty()) encoderName = encAnn->getString("value");
        if (encoderName.empty()) {
            throw Exception(
                "@Encoding on `" + qName->toCanonical()
                + "` is missing the encoder class arg: write "
                "`@Encoding(MyEncoder.class)`",
                "CAJETA_ERROR_ENCODING_NO_ARG");
        }

        CajetaClassPtr encoder;
        for (auto& [canon, t] : CajetaType::getCanonicalMap()) {
            if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                auto qn = cls->getQName();
                if (qn && (qn->getTypeName() == encoderName
                        || qn->toCanonical() == encoderName)) {
                    encoder = cls;
                    break;
                }
            }
        }
        if (!encoder) {
            throw Exception(
                "@Encoding on `" + qName->toCanonical()
                + "`: encoder class `" + encoderName
                + "` not found",
                "CAJETA_ERROR_ENCODING_ENCODER_NOT_FOUND");
        }

        encoder->generatePrototype();

        {
            const auto& encQImpl = encoder->getQImplemented();
            const auto& encQImplArgs = encoder->getQImplementedTypeArgs();
            auto qit = encQImpl.begin();
            auto ait = encQImplArgs.begin();
            std::string parentShort = qName->getTypeName();
            std::string parentCanon = qName->toCanonical();
            for (; qit != encQImpl.end() && ait != encQImplArgs.end();
                    ++qit, ++ait) {
                auto& qn = *qit;
                auto& args = *ait;
                if (!qn) continue;
                bool isEncoderImpl =
                    qn->getTypeName() == "Encoder"
                    || qn->toCanonical() == "cajeta.wire.Encoder";
                if (!isEncoderImpl) continue;
                if (args.size() != 1 || !args.front()) {
                    throw Exception(
                        "@Encoding on `" + qName->toCanonical()
                        + "`: encoder class `"
                        + encoder->getQName()->toCanonical()
                        + "` declares `implements Encoder` without a "
                        "single type argument. Write `implements "
                        "Encoder<"
                        + parentShort + ">`.",
                        "CAJETA_ERROR_ENCODING_ENCODER_BAD_ARITY");
                }
                auto& arg = args.front();
                std::string argShort = arg->getTypeName();
                std::string argCanon = arg->toCanonical();
                bool matches = (argShort == parentShort)
                            || (argCanon == parentCanon)
                            || (argShort == parentCanon)
                            || (argCanon == parentShort);
                if (!matches) {
                    throw Exception(
                        "@Encoding on `" + qName->toCanonical()
                        + "`: encoder class `"
                        + encoder->getQName()->toCanonical()
                        + "` declares `implements Encoder<" + argShort
                        + ">` but is being attached as the encoder for `"
                        + parentShort
                        + "`. The type argument must match the annotated "
                        "class — change either the `@Encoding` target "
                        "or the encoder's `implements` clause.",
                        "CAJETA_ERROR_ENCODING_ENCODER_T_MISMATCH");
                }
                break;
            }
        }

        bool ctorExists = ctorWithArityExists(1);
        bool toBytesExists = false;
        for (auto& m : methodList) {
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "toBytes") continue;
            auto params = m->getParameterList();
            size_t userArgs = params.size();
            if (!params.empty() && params.front()
                    && params.front()->getName() == "this") {
                userArgs--;
            }
            if (userArgs == 0) { toBytesExists = true; break; }
        }

        if (!ctorExists) {
            auto ctor = std::make_shared<SynthesizedEncodingCtor>(
                module,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                encoder);
            ctor->initParameter();
            addMethod(ctor);
        }
        if (!toBytesExists) {
            auto tb = std::make_shared<SynthesizedEncodingToBytes>(
                module,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                encoder);
            addMethod(tb);
        }
    }

    // @Builder: create the nested `Builder` class — mirrored fields, no-arg ctor,
    // chained setters, build() — register it, and add the outer's `builder()` static
    // factory. Names come from builderMethodName / buildMethodName / setterPrefix.
    void CajetaClass::synthesizeBuilder() {
        auto ann = findAnnotation("Builder");
        if (!ann) return;

        std::string builderMethodName = ann->getString("builderMethodName");
        if (builderMethodName.empty()) builderMethodName = "builder";
        std::string buildMethodName = ann->getString("buildMethodName");
        if (buildMethodName.empty()) buildMethodName = "build";
        std::string setterPrefix = ann->getString("setterPrefix");

        auto prefixedSetterName = [&](const std::string& fieldName) {
            if (setterPrefix.empty() || fieldName.empty()) return fieldName;
            std::string out = setterPrefix;
            out += (char) std::toupper((unsigned char) fieldName[0]);
            if (fieldName.size() > 1) out.append(fieldName.substr(1));
            return out;
        };

        std::vector<StructurePropertyPtr> outerFields;
        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            outerFields.push_back(prop);
        }

        auto builderQName = QualifiedName::getOrInsert(
            "Builder", qName->toCanonical());
        std::list<QualifiedNamePtr> builderExtends{
            QualifiedName::getOrInsert("Object", "cajeta.lang")
        };
        std::list<QualifiedNamePtr> builderImplements;
        auto builder = std::make_shared<CajetaClass>(
            module, builderQName, builderExtends, builderImplements);

        CajetaType::getCanonicalMap()[builderQName->toCanonical()] =
            std::static_pointer_cast<CajetaType>(builder);

        int order = 0;
        for (auto& prop : outerFields) {
            auto mirror = std::make_shared<StructureProperty>(
                prop->getName(), prop->getType(), order++);
            mirror->addModifier(PRIVATE);
            builder->addProperty(mirror);
        }

        {
            auto ctor = std::make_shared<SynthesizedConstructorMethod>(
                module, builder, std::vector<StructurePropertyPtr>{});
            ctor->initParameters();
            builder->addMethod(ctor);
        }

        for (auto& mirror : builder->getPropertyList()) {
            if (!mirror) continue;
            auto setter = std::make_shared<SynthesizedBuilderSetterMethod>(
                module, builder, mirror,
                prefixedSetterName(mirror->getName()));
            setter->initParameter();
            builder->addMethod(setter);
        }

        {
            auto buildMethod = std::make_shared<SynthesizedBuildMethod>(
                module, builder,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                buildMethodName);
            builder->addMethod(buildMethod);
        }

        builder->generatePrototype();

        auto isBuilderDefault = [](const AnnotationInstancePtr& a) {
            if (!a) return false;
            auto qn = a->getName();
            if (!qn) return false;
            const std::string& tn = qn->getTypeName();
            const std::string& pn = qn->getPackageName();
            if (tn == "Default" && pn == "Builder") return true;
            if (tn == "BuilderDefault") return true;
            return false;
        };
        std::vector<SynthesizedBuilderFactoryMethod::DefaultEntry> defaults;
        {
            auto mirrorList = builder->getPropertyList();
            auto outerIt = outerFields.begin();
            auto mirrorIt = mirrorList.begin();
            for (; outerIt != outerFields.end() && mirrorIt != mirrorList.end();
                    ++outerIt, ++mirrorIt) {
                auto& outerProp = *outerIt;
                auto& mirror = *mirrorIt;
                if (!outerProp || !mirror) continue;
                bool hasDefault = false;
                for (auto& ann : outerProp->getAnnotationInstances()) {
                    if (isBuilderDefault(ann)) { hasDefault = true; break; }
                }
                if (!hasDefault) continue;
                auto init = outerProp->getInitializer();
                if (!init) {
                    throw Exception(
                        "@Builder.Default on field `"
                        + outerProp->getName() + "` of `"
                        + qName->toCanonical()
                        + "` requires a field initializer "
                        "(`@Builder.Default fieldName = expr`).",
                        "CAJETA_ERROR_BUILDER_DEFAULT_NO_INIT");
                }
                defaults.push_back({mirror, init});
            }
        }

        auto factory = std::make_shared<SynthesizedBuilderFactoryMethod>(
            module,
            std::static_pointer_cast<CajetaClass>(shared_from_this()),
            builder,
            builderMethodName,
            std::move(defaults));
        addMethod(factory);
        factory->generatePrototype();
        module->getStructures()[builderQName->toCanonical()] = builder;
        CajetaModule::getStructureToModule()
            [builderQName->toCanonical()] = module;
    }

    // Adds the implicit no-argument constructor when the class declares none.
    // Consults the constructor map, not `methods`, which is keyed by canonical.
    void CajetaClass::ensureDefaultConstructor() {
        if (!unlabeledConstructorMap.empty()) return;
        addMethod(make_shared<DefaultConstructorMethod>(
            module, static_pointer_cast<CajetaClass>(shared_from_this())));
    }

    // Re-parents every member declaration in `classBody` onto this class.
    void CajetaClass::setClassBody(cajeta::ClassBodyDeclarationPtr classBody) {
        for (auto memberDeclaration: classBody->getDeclarations()) {
            memberDeclaration->updateParent(static_pointer_cast<CajetaClass>(shared_from_this()));
        }
    }

    // Emits every method body, then the class initializer for any static field whose
    // initializer did not constant-fold.
    void CajetaClass::generateCode() {
        for (auto& method: methodList) {
            method->generateCode();
        }
        generateStaticInitializers();
    }

    // Emits `__cajeta_clinit_<class>` storing the static-field initializers that
    // foldStaticInitializer could not bake into the global, and registers it in
    // llvm.global_ctors. Returns early when every initializer folded.
    void CajetaClass::generateStaticInitializers() {
        CajetaModulePtr* moduleSlot = &module;
        CajetaModulePtr savedModule = module;
        if (emitModule && emitModule != module) module = emitModule;
        struct RestoreModule {
            CajetaModulePtr* slot; CajetaModulePtr saved;
            ~RestoreModule() { *slot = saved; }
        } restoreModule{moduleSlot, savedModule};

        std::vector<StructurePropertyPtr> needsClinit;
        for (auto& prop : propertyList) {
            if (!prop || !prop->isStatic()) continue;
            if (!prop->getInitializer()) continue;
            if (!prop->getType() || !prop->getType()->getLlvmType()) continue;
            llvm::Type* storedType = prop->getType()->getLlvmType();
            if (!(prop->getType()->getTypeFlags() & PRIMITIVE_FLAG)) {
                // An INTERFACE value IS the 24-byte fat struct, stored by value, so
                // a `ptr` global under-allocates it. Both derivation sites must agree.
                auto ifaceCls = std::dynamic_pointer_cast<CajetaClass>(
                    prop->getType());
                bool isInterfaceValue = ifaceCls && ifaceCls->isInterface()
                    && storedType && storedType->isStructTy();
                if (!isInterfaceValue) {
                    storedType = llvm::PointerType::get(
                        *module->getLlvmContext(), 0);
                }
            }
            if (foldStaticInitializer(prop->getInitializer(), storedType)) {
                continue;
            }
            needsClinit.push_back(prop);
        }
        if (needsClinit.empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();

        std::string fnName = std::string("__cajeta_clinit_")
            + qName->toCanonical();
        for (char& c : fnName) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                    || c == ',' || c == ' ') {
                c = '_';
            }
        }
        if (lmod->getFunction(fnName)) return;

        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), /*isVarArg=*/false);
        llvm::Function* clinit = llvm::Function::Create(fnTy,
            llvm::Function::PrivateLinkage, fnName, lmod);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(
            ctx, "entry", clinit);

        // A FRESH local builder: after the method-codegen pass the module's
        // leftover builder pointer can dangle, and GetInsertBlock would fault.
        llvm::IRBuilder<> clinitBuilder(entry);
        llvm::IRBuilder<>* builder = &clinitBuilder;
        llvm::IRBuilder<>* prevModuleBuilder = module->getBuilder();
        module->setBuilder(builder);

        bool pushedSelf = false;
        if (qName) {
            module->getStructureStack().push_back(
                std::static_pointer_cast<CajetaClass>(shared_from_this()));
            pushedSelf = true;
        }

        for (auto& prop : needsClinit) {
            llvm::GlobalVariable* g = getOrCreateStaticFieldGlobal(prop);
            if (!g) continue;
            auto initAst = prop->getInitializer();
            ExpressionPtr expr;
            if (auto vi = std::dynamic_pointer_cast<VariableInitializer>(initAst)) {
                auto& kids = vi->getChildren();
                if (!kids.empty()) {
                    expr = std::dynamic_pointer_cast<Expression>(kids[0]);
                }
            } else {
                expr = std::dynamic_pointer_cast<Expression>(initAst);
            }
            if (!expr) continue;

            if (!expr->getResolvedType()) {
                expr->resolveTypes(module);
            }
            llvm::Value* val = expr->generateCode(module);
            if (!val) continue;

            // loadIfLValue, not a GlobalVariable test: a String literal (and
            // `T.class`) is an r-value whose ADDRESS is the value.
            val = loadIfLValue(module, val, expr);

            llvm::Type* storedType = g->getValueType();
            if (val->getType() != storedType) {
                if (val->getType()->isIntegerTy() && storedType->isIntegerTy()) {
                    val = builder->CreateIntCast(val, storedType,
                        /*isSigned=*/true);
                } else if (val->getType()->isFloatingPointTy()
                        && storedType->isFloatingPointTy()) {
                    val = builder->CreateFPCast(val, storedType);
                } else if (val->getType()->isIntegerTy()
                        && storedType->isFloatingPointTy()) {
                    val = builder->CreateSIToFP(val, storedType);
                } else if (val->getType()->isFloatingPointTy()
                        && storedType->isIntegerTy()) {
                    val = builder->CreateFPToSI(val, storedType);
                }
            }
            if (val->getType() != storedType) {
                auto declTy = prop->getType();
                auto initTy = expr->getResolvedType();
                std::string declName = (declTy && declTy->getQName())
                    ? declTy->getQName()->toCanonical() : "<unknown>";
                std::string initName = (initTy && initTy->getQName())
                    ? initTy->getQName()->toCanonical() : "<unresolved>";
                throw locatedException(
                    expr->getSourceLine(), expr->getSourceColumn() + 1,
                    "initializer for static field '" + prop->getName()
                        + "' has type '" + initName
                        + "', which is not assignable to '" + declName + "'",
                    "CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH");
            }
            builder->CreateStore(val, g);
        }
        builder->CreateRetVoid();

        if (pushedSelf) {
            module->getStructureStack().pop_back();
        }
        module->setBuilder(prevModuleBuilder);

        llvm::appendToGlobalCtors(*lmod, clinit, /*Priority=*/65535);
    }

    // Folds a static-field initializer to an llvm::Constant when its shape is a
    // compile-time literal (integer or float, optionally negated or wrapped in a
    // same-category cast). Null for anything else; the caller emits a clinit.
    static llvm::Constant* foldStaticInitializer(
            AbstractSyntaxNodePtr init, llvm::Type* storedType) {
        if (!init || !storedType) return nullptr;
        if (auto vi = dynamic_pointer_cast<VariableInitializer>(init)) {
            auto& kids = vi->getChildren();
            if (kids.empty()) return nullptr;
            init = kids[0];
        }
        // Fold a cast's operand against the stored type: an avoidable clinit
        // becomes a global_ctors entry, which is a linker GC root.
        if (auto ce = dynamic_pointer_cast<CastExpression>(init)) {
            auto& kids = ce->getChildren();
            if (kids.empty()) return nullptr;
            return foldStaticInitializer(kids[0], storedType);
        }
        bool negate = false;
        if (auto pe = dynamic_pointer_cast<PrefixExpression>(init)) {
            if (pe->getOp() != PREFIX_OP_NEGATIVE) return nullptr;
            auto& kids = pe->getChildren();
            if (kids.empty()) return nullptr;
            init = kids[0];
            negate = true;
        }
        if (auto il = dynamic_pointer_cast<IntegerLiteralExpression>(init)) {
            if (!storedType->isIntegerTy()) return nullptr;
            uint8_t radix;
            switch (il->getIntegerLiteralType()) {
                case INTEGER_LITERAL_TYPE_BINARY: radix = 2;  break;
                case INTEGER_LITERAL_TYPE_OCT:    radix = 8;  break;
                case INTEGER_LITERAL_TYPE_HEX:    radix = 16; break;
                default:                          radix = 10; break;
            }
            std::string raw = il->getRawValue();
            if (raw.size() >= 2 && raw[0] == '0'
                    && (raw[1] == 'b' || raw[1] == 'B'
                        || raw[1] == 'x' || raw[1] == 'X')) {
                raw = raw.substr(2);
            }
            if (!raw.empty() && (raw.back() == 'L' || raw.back() == 'l')) {
                raw = raw.substr(0, raw.size() - 1);
            }
            unsigned bits = storedType->getIntegerBitWidth();
            llvm::APInt apint(64, raw, radix);
            if (negate) apint = -apint;
            if (bits < 64) {
                apint = apint.trunc(bits);
            } else if (bits > 64) {
                apint = apint.sext(bits);
            }
            return llvm::ConstantInt::get(storedType, apint);
        }
        if (auto fl = dynamic_pointer_cast<FloatLiteralExpression>(init)) {
            if (!storedType->isFloatingPointTy()) return nullptr;
            double v = std::strtod(fl->getRawValue().c_str(), nullptr);
            if (negate) v = -v;
            return llvm::ConstantFP::get(storedType, v);
        }
        return nullptr;
    }

    // The LLVM global backing a static field, created on first use in the class's
    // emit module and named `<canonical>.<field>`. A caller emitting into another
    // module gets an extern declaration there instead.
    llvm::GlobalVariable* CajetaClass::getOrCreateStaticFieldGlobal(
            StructurePropertyPtr prop, CajetaModulePtr callerModule) {
        if (!prop || !prop->isStatic()) return nullptr;
        if (!prop->getType() || !prop->getType()->getLlvmType()) return nullptr;

        auto* lmod = getEmitModule()->getLlvmModule();
        const std::string globalName =
            qName->toCanonical() + "." + prop->getName();

        auto& staticFieldGlobals = staticFieldGlobalsRef();
        auto it = staticFieldGlobals.find(prop->getName());
        llvm::GlobalVariable* g;
        if (it != staticFieldGlobals.end()) {
            g = it->second;
        } else if (auto* existing = lmod->getNamedGlobal(globalName)) {
            g = existing;
        } else {
            llvm::Type* storedType = prop->getType()->getLlvmType();
            if (!(prop->getType()->getTypeFlags() & PRIMITIVE_FLAG)) {
                // An INTERFACE value IS the 24-byte fat struct, stored by value, so
                // a `ptr` global under-allocates it. Both derivation sites must agree.
                auto ifaceCls = std::dynamic_pointer_cast<CajetaClass>(
                    prop->getType());
                bool isInterfaceValue = ifaceCls && ifaceCls->isInterface()
                    && storedType && storedType->isStructTy();
                if (!isInterfaceValue) {
                    storedType = llvm::PointerType::get(
                        *module->getLlvmContext(), 0);
                }
            }
            llvm::Constant* init = foldStaticInitializer(
                prop->getInitializer(), storedType);
            if (!init) init = llvm::Constant::getNullValue(storedType);
            g = new llvm::GlobalVariable(
                *lmod, storedType, /*isConstant=*/false,
                llvm::GlobalValue::ExternalLinkage,
                init, globalName);
            staticFieldGlobals[prop->getName()] = g;
        }

        if (callerModule && callerModule->getLlvmModule() != lmod) {
            CajetaModule::noteCrossModuleStaticFieldRef(
                callerModule,
                static_pointer_cast<CajetaClass>(shared_from_this()),
                prop->getName());
            llvm::Constant* shim = CajetaModule::ensureGlobalInModule(
                callerModule->getLlvmModule(), g);
            return llvm::cast<llvm::GlobalVariable>(shim);
        }
        return g;
    }

    // Points the vtable's drop_fn slot (index 3) at this class's heap drop wrapper
    // so __cajeta_class_virtual_drop reaches it. Idempotent, and a no-op until the
    // vtable, the LLVM struct and the drop function all exist.
    void CajetaClass::patchVirtualTableDropFn() {
        bool& dropFnPatched = dropFnPatchedRef();
        if (dropFnPatched) return;
        // Cheap readiness checks BEFORE getOrCreateDropFunction, which EMITS the
        // drop body: forcing that mid-prototype re-enters here and faults.
        llvm::GlobalVariable* vtGlobal = vtableGlobalRef();
        llvm::StructType* vtType = vtableTypeRef();
        if (!vtGlobal || !vtType) return;
        if (!rawLlvmType()) return;
        llvm::Function* dropFn = getOrCreateDropFunction();
        if (!dropFn) return;
        if (!vtGlobal->hasInitializer()) return;
        llvm::Constant* init = vtGlobal->getInitializer();
        if (!init) return;
        auto* structInit = llvm::dyn_cast<llvm::ConstantStruct>(init);
        if (!structInit) return;
        std::vector<llvm::Constant*> elems;
        elems.reserve(structInit->getNumOperands());
        for (unsigned i = 0; i < structInit->getNumOperands(); ++i) {
            elems.push_back(structInit->getOperand(i));
        }
        if (elems.size() < 4) return;
        // The cached drop handle may live in another llvm::Module, and referencing
        // it from this vtable's initializer is invalid IR. Localize it first.
        llvm::Function* localDrop = CajetaModule::ensureFunctionInModule(
            vtGlobal->getParent(), dropFn);
        elems[3] = localDrop ? (llvm::Constant*) localDrop : (llvm::Constant*) dropFn;
        vtGlobal->setInitializer(
            llvm::ConstantStruct::get(vtType,
                llvm::ArrayRef<llvm::Constant*>(elems)));
        dropFnPatched = true;
    }

    // Emits (or returns) `__cajeta_stack_<class>_drop`: this class's drop body plus
    // every ancestor's at its sub-object offset, with NO trailing free, since the
    // frame reclaims the body. LinkOnceODR so it merges across TUs.
    llvm::Function* CajetaClass::getOrCreateStackDropFunction() {
        auto& llvmStackDropFunction = stackDropFnRef();
        if (llvmStackDropFunction) return llvmStackDropFunction;
        if (interfaceFlag) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);

        std::string dropName = std::string("__cajeta_stack_")
            + qName->toCanonical() + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }
        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmStackDropFunction = existing;
            return existing;
        }

        llvmStackDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::LinkOnceODRLinkage, dropName, lmod);
        {
            llvm::Triple dropTriple(lmod->getTargetTriple());
            if (!dropTriple.isOSBinFormatMachO()) {
                llvmStackDropFunction->setComdat(lmod->getOrInsertComdat(dropName));
            }
        }
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmStackDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* instance = llvmStackDropFunction->getArg(0);

        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmStackDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmStackDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(instance,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);
        b.SetInsertPoint(doDrop);

        emitDropBodyInline(b, instance, module);

        for (auto& ancestor : collectDestructorChain()) {
            uint64_t off = getSubObjectByteOffset(ancestor.get());
            llvm::Value* ancestorThis = instance;
            if (off != 0) {
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                ancestorThis = b.CreateInBoundsGEP(i8Ty, instance,
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(ctx), off),
                    std::string("stack_dtor_subobj_")
                        + ancestor->getQName()->getTypeName());
            }
            ancestor->emitDropBodyInline(b, ancestorThis, module);
        }


        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmStackDropFunction;
    }

    // True for the balanced line-info, debug and profiler probe calls. They
    // reclaim nothing, so a drop that only calls these is still a no-op drop.
    static bool isInstrumentationCall(const llvm::Function* f) {
        if (!f) return false;
        llvm::StringRef n = f->getName();
        return n == "__cajeta_line_enter" || n == "__cajeta_line_mark"
            || n == "__cajeta_line_leave" || n == "__cajeta_dbg_frame_enter"
            || n == "__cajeta_dbg_frame_leave" || n == "__cajeta_dbg_local"
            || n == "__cajeta_dbg_safepoint"
            || n == "__cajeta_prof_instr_enter"
            || n == "__cajeta_prof_instr_exit";
    }

    // True when `f` does nothing observable: every call it transitively makes is to
    // another such function. A declaration (a runtime free) returns false, and
    // indirect calls or invokes are conservatively non-trivial.
    static bool isNoOpDropFn(llvm::Function* f,
                             std::unordered_set<llvm::Function*>& seen) {
        if (!f || f->isDeclaration()) return false;
        if (!seen.insert(f).second) return true;
        for (auto& bb : *f) {
            for (auto& inst : bb) {
                if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (isInstrumentationCall(ci->getCalledFunction())) continue;
                    if (!isNoOpDropFn(ci->getCalledFunction(), seen)) return false;
                } else if (llvm::isa<llvm::InvokeInst>(&inst)) {
                    return false;
                }
            }
        }
        return true;
    }

    // True when registering a scope-exit drop for a stack local of this class would
    // be pure overhead. Forces the stack drop function, which is emitted anyway for
    // the vtable slot; CAJETA_DROP_ELISION_DISABLE forces the answer off.
    bool CajetaClass::hasTrivialStackDrop() {
        if (interfaceFlag) return false;
        if (std::getenv("CAJETA_DROP_ELISION_DISABLE")) return false;
        std::unordered_set<llvm::Function*> seen;
        return isNoOpDropFn(getOrCreateStackDropFunction(), seen);
    }

    // Ancestors in destruction order: reverse DFS over direct parents in reverse
    // declaration order, interfaces skipped and each shared ancestor visited exactly
    // once (MemoryModel.md section 140).
    std::vector<CajetaClassPtr> CajetaClass::collectDestructorChain() {
        std::vector<CajetaClassPtr> chain;
        std::set<CajetaClass*> visited;
        std::function<void(CajetaClass*)> walk = [&](CajetaClass* klass) {
            auto& parents = klass->superClasses;
            for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
                CajetaClassPtr parent = *it;
                if (!parent) continue;
                if (parent->isInterface()) continue;
                if (visited.count(parent.get())) continue;
                visited.insert(parent.get());
                chain.push_back(parent);
                walk(parent.get());
            }
        };
        walk(this);
        return chain;
    }

    // Inlines one class's drop contribution at `instance`: the user `drop()` body,
    // then own-field auto-drops in reverse declaration order — arrays, interfaces,
    // class references and closures, each under its ownership bit.
    void CajetaClass::emitDropBodyInline(llvm::IRBuilder<>& b,
                                          llvm::Value* instance,
                                          CajetaModulePtr cajModule) {
        // Callees must be co-resident with the function the builder is inserting
        // into, which can differ from cajModule's own llvm::Module.
        llvm::Module* bodyModule = b.GetInsertBlock()
            ? b.GetInsertBlock()->getModule()
            : cajModule->getLlvmModule();

        MethodPtr userDrop;
        for (auto& entry : methods) {
            MethodPtr m = entry.second;
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "drop") continue;
            if (m->getParameterList().size() == 1) {
                userDrop = m;
                break;
            }
        }
        if (userDrop && userDrop->getLlvmFunction()) {
            llvm::Function* fn = CajetaModule::ensureFunctionInModule(
                bodyModule, userDrop->getLlvmFunction());
            b.CreateCall(userDrop->getLlvmFunctionType(), fn, {instance});
        }

        auto& ctx = *cajModule->getLlvmContext();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        std::vector<StructurePropertyPtr> reversed(
            propertyList.begin(), propertyList.end());
        std::reverse(reversed.begin(), reversed.end());

        llvm::Function* freeArrayFn = nullptr;
        llvm::Function* virtualDropFn = nullptr;
        llvm::Function* ifaceDropFn = nullptr;

        for (auto& property : reversed) {
            if (property->isStatic()) continue;
            auto fieldType = property->getType();
            if (!fieldType) continue;
            // A `T`-origin scalar field may have been LENT, which no type can
            // declare. Skip it unless it carries a runtime ownership bit, and let
            // the guarded drop below decide per instance.
            int scalarOrigin = property->getOriginTypeParamIndex();
            if (scalarOrigin >= 0) {
                auto fc = dynamic_pointer_cast<CajetaClass>(fieldType);
                bool bitGuarded = fc
                    && !dynamic_pointer_cast<CajetaView>(fieldType)
                    && !(fc->isValueType() && fc->isSharedCapableValue())
                    && !fc->isInterface()
                    && fc->hasVtablePointerAtSlotZero()
                    && ownershipBitIndexOf(property) >= 0
                    && getOwnershipWordLlvmIndex() >= 0;
                if (!bitGuarded) continue;
            }
            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);

            if (auto arrField = dynamic_pointer_cast<CajetaArray>(fieldType)) {
                // Inline `T[N]` storage is not a heap allocation; freeing the slot
                // would free the inline bytes read as an address.
                if (arrField->isInlineArray()) continue;
                if (!freeArrayFn) {
                    freeArrayFn = cajModule->getRuntimeFunction("__cajeta_free_array", bodyModule);
                }
                if (!freeArrayFn) continue;
                llvm::BasicBlock* ownCont = nullptr;
                {
                    int bitIdx = ownershipBitIndexOf(property);
                    int wordIdx = getOwnershipWordLlvmIndex();
                    if (bitIdx >= 0 && wordIdx >= 0 && b.GetInsertBlock()) {
                        llvm::Type* gi64 = llvm::Type::getInt64Ty(ctx);
                        llvm::Function* fn = b.GetInsertBlock()->getParent();
                        llvm::Value* wordSlot = b.CreateStructGEP(
                            rawLlvmType(), instance, (unsigned) wordIdx,
                            "own_bits_slot");
                        llvm::Value* w = b.CreateLoad(gi64, wordSlot);
                        llvm::Value* bit = b.CreateAnd(
                            b.CreateLShr(w, llvm::ConstantInt::get(gi64, bitIdx)),
                            llvm::ConstantInt::get(gi64, 1));
                        llvm::Value* owned = b.CreateICmpNE(
                            bit, llvm::ConstantInt::get(gi64, 0));
                        llvm::BasicBlock* dropBB =
                            llvm::BasicBlock::Create(ctx, "own_drop", fn);
                        ownCont = llvm::BasicBlock::Create(ctx, "own_cont", fn);
                        b.CreateCondBr(owned, dropBB, ownCont);
                        b.SetInsertPoint(dropBB);
                    }
                }
                llvm::Value* slot = b.CreateStructGEP(
                    rawLlvmType(), instance, fieldIdx,
                    std::string("drop_arr_slot_") + property->getName());
                llvm::Value* arrPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_arr_ptr_") + property->getName());
                if (CajetaClass::arrayElementCarriesSlotBits(
                        arrField->getElementType())) {
                    if (llvm::Function* walkFn = cajModule->getRuntimeFunction(
                            "__cajeta_tail_elem_drop_walk", bodyModule)) {
                        const llvm::DataLayout& wdl = bodyModule->getDataLayout();
                        llvm::Type* wi64 = llvm::Type::getInt64Ty(ctx);
                        b.CreateCall(walkFn, {arrPtr,
                            llvm::ConstantInt::get(wi64,
                                wdl.getTypeAllocSize(arrField->getLlvmType())),
                            llvm::ConstantInt::get(wi64,
                                arrField->elementStrideBytes(wdl, &ctx))});
                    }
                }
                if (CajetaClass::arrayElementCarriesArraySlotBits(
                        arrField->getElementType())) {
                    if (llvm::Function* walkFn = cajModule->getRuntimeFunction(
                            "__cajeta_tail_arrelem_drop_walk", bodyModule)) {
                        const llvm::DataLayout& wdl = bodyModule->getDataLayout();
                        llvm::Type* wi64 = llvm::Type::getInt64Ty(ctx);
                        b.CreateCall(walkFn, {arrPtr,
                            llvm::ConstantInt::get(wi64,
                                wdl.getTypeAllocSize(arrField->getLlvmType())),
                            llvm::ConstantInt::get(wi64,
                                arrField->elementStrideBytes(wdl, &ctx)),
                            llvm::ConstantInt::get(wi64,
                                CajetaClass::arrayElementInnerDropKind(
                                    arrField->getElementType()))});
                    }
                }
                {
                    auto strElem = dynamic_pointer_cast<CajetaClass>(
                        arrField->getElementType());
                    if (strElem && slotMemberIsString(strElem)) {
                        if (llvm::Function* swFn = cajModule->getRuntimeFunction(
                                "__cajeta_string_elem_drop_walk", bodyModule)) {
                            const llvm::DataLayout& swDl =
                                bodyModule->getDataLayout();
                            llvm::Type* swi64 = llvm::Type::getInt64Ty(ctx);
                            b.CreateCall(swFn, {arrPtr,
                                llvm::ConstantInt::get(swi64,
                                    swDl.getTypeAllocSize(
                                        arrField->getLlvmType())),
                                llvm::ConstantInt::get(swi64,
                                    arrField->elementStrideBytes(
                                        swDl, &ctx))});
                        }
                    }
                }
                if (CajetaClass::arrayElementCarriesMemberBits(
                        arrField->getElementType())) {
                    const llvm::DataLayout& wdl = bodyModule->getDataLayout();
                    if (llvm::Function* mwFn = getOrCreateMemberWalk(
                            cajModule, bodyModule,
                            dynamic_pointer_cast<CajetaClass>(
                                arrField->getElementType()),
                            wdl.getTypeAllocSize(arrField->getLlvmType()),
                            arrField->elementStrideBytes(wdl, &ctx),
                            /*withFree=*/false)) {
                        b.CreateCall(mwFn, {arrPtr});
                    }
                }
                b.CreateCall(freeArrayFn, {arrPtr});
                if (ownCont) {
                    b.CreateBr(ownCont);
                    b.SetInsertPoint(ownCont);
                }
                continue;
            }

            if (auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType)) {
                if (dynamic_pointer_cast<CajetaView>(fieldType)) continue;
                if (fieldClass->isValueType()
                        && fieldClass->isSharedCapableValue()) {
                    llvm::Value* slot = b.CreateStructGEP(
                        rawLlvmType(), instance, fieldIdx,
                        std::string("drop_vshared_") + property->getName());
                    fieldClass->emitValueSharedOp(b, slot, cajModule,
                                                  bodyModule, /*retain=*/false);
                    continue;
                }
                if (fieldClass->isInterface()) {
                    if (!ifaceDropFn) {
                        ifaceDropFn = cajModule->getRuntimeFunction("__cajeta_iface_drop", bodyModule);
                    }
                    if (!ifaceDropFn) continue;
                    llvm::Value* bodyPtr = b.CreateStructGEP(
                        rawLlvmType(), instance, fieldIdx,
                        std::string("drop_iface_body_") + property->getName());
                    b.CreateCall(ifaceDropFn, {bodyPtr});
                    continue;
                }
                if (!fieldClass->hasVtablePointerAtSlotZero()) continue;
                if (!virtualDropFn) {
                    virtualDropFn = cajModule->getRuntimeFunction(
                        "__cajeta_class_virtual_drop", bodyModule);
                }
                if (!virtualDropFn) continue;
                fieldClass->patchVirtualTableDropFn();
                llvm::BasicBlock* ownCont = nullptr;
                {
                    int bitIdx = ownershipBitIndexOf(property);
                    int wordIdx = getOwnershipWordLlvmIndex();
                    if (bitIdx >= 0 && wordIdx >= 0 && b.GetInsertBlock()) {
                        llvm::Type* gi64 = llvm::Type::getInt64Ty(ctx);
                        llvm::Function* fn = b.GetInsertBlock()->getParent();
                        llvm::Value* wordSlot = b.CreateStructGEP(
                            rawLlvmType(), instance, (unsigned) wordIdx,
                            "own_bits_slot");
                        llvm::Value* w = b.CreateLoad(gi64, wordSlot);
                        llvm::Value* bit = b.CreateAnd(
                            b.CreateLShr(w, llvm::ConstantInt::get(gi64, bitIdx)),
                            llvm::ConstantInt::get(gi64, 1));
                        llvm::Value* owned = b.CreateICmpNE(
                            bit, llvm::ConstantInt::get(gi64, 0));
                        llvm::BasicBlock* dropBB =
                            llvm::BasicBlock::Create(ctx, "own_drop", fn);
                        ownCont = llvm::BasicBlock::Create(ctx, "own_cont", fn);
                        b.CreateCondBr(owned, dropBB, ownCont);
                        b.SetInsertPoint(dropBB);
                    }
                }
                llvm::Value* slot = b.CreateStructGEP(
                    rawLlvmType(), instance, fieldIdx,
                    std::string("drop_ref_slot_") + property->getName());
                llvm::Value* refPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_ref_ptr_") + property->getName());
                b.CreateCall(virtualDropFn, {refPtr});
                if (ownCont) {
                    b.CreateBr(ownCont);
                    b.SetInsertPoint(ownCont);
                }
                continue;
            }

            if (dynamic_pointer_cast<CajetaFunctionType>(fieldType)) {
                llvm::Function* closureDropFn = cajModule->getRuntimeFunction(
                    "__cajeta_closure_drop", bodyModule);
                if (!closureDropFn) continue;
                int bitIdx = ownershipBitIndexOf(property);
                int wordIdx = getOwnershipWordLlvmIndex();
                if (bitIdx < 0 || wordIdx < 0 || !b.GetInsertBlock()) continue;
                llvm::Type* gi64 = llvm::Type::getInt64Ty(ctx);
                llvm::Function* fn = b.GetInsertBlock()->getParent();
                llvm::Value* wordSlot = b.CreateStructGEP(
                    rawLlvmType(), instance, (unsigned) wordIdx, "own_bits_slot");
                llvm::Value* w = b.CreateLoad(gi64, wordSlot);
                llvm::Value* bit = b.CreateAnd(
                    b.CreateLShr(w, llvm::ConstantInt::get(gi64, bitIdx)),
                    llvm::ConstantInt::get(gi64, 1));
                llvm::Value* owned = b.CreateICmpNE(bit, llvm::ConstantInt::get(gi64, 0));
                llvm::BasicBlock* dropBB = llvm::BasicBlock::Create(ctx, "own_drop_fn", fn);
                llvm::BasicBlock* ownCont = llvm::BasicBlock::Create(ctx, "own_cont_fn", fn);
                b.CreateCondBr(owned, dropBB, ownCont);
                b.SetInsertPoint(dropBB);
                llvm::Value* slot = b.CreateStructGEP(
                    rawLlvmType(), instance, fieldIdx,
                    std::string("drop_fn_slot_") + property->getName());
                llvm::Value* recPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_fn_ptr_") + property->getName());
                b.CreateCall(closureDropFn, {recPtr});
                b.CreateBr(ownCont);
                b.SetInsertPoint(ownCont);
                continue;
            }
        }
    }

    // True when this value type holds a reference-counted root — Utf8, a Slice
    // instantiation, or a value aggregate embedding one — so copies retain and drops
    // release.
    bool CajetaClass::isSharedCapableValue() {
        if (qName && qName->getTypeName() == "Utf8"
                && qName->getPackageName() == "cajeta.lang") {
            return true;
        }
        if (qName && qName->getPackageName() == "cajeta.lang"
                && qName->getTypeName().rfind("Slice", 0) == 0
                && isValueType()) {
            return true;
        }
        if (!isValueType() || interfaceFlag) return false;
        for (auto& property : propertyList) {
            if (property->isStatic()) continue;
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(property->getType());
            if (fieldClass && !dynamic_pointer_cast<CajetaView>(property->getType())
                    && fieldClass->isSharedCapableValue()) {
                return true;
            }
        }
        return false;
    }

    // Emits the retain (or release) for a shared-capable value at `valuePtr`: the
    // Utf8 or Slice runtime call, or a recursive walk of the shared-capable members
    // of a value aggregate.
    void CajetaClass::emitValueSharedOp(llvm::IRBuilder<>& b,
                                        llvm::Value* valuePtr,
                                        CajetaModulePtr cajModule,
                                        llvm::Module* bodyModule,
                                        bool retain) {
        if (qName && qName->getTypeName() == "Utf8"
                && qName->getPackageName() == "cajeta.lang") {
            llvm::Function* fn = cajModule->getRuntimeFunction(
                retain ? "__cajeta_utf8_retain" : "__cajeta_utf8_release",
                bodyModule);
            if (fn) b.CreateCall(fn, {valuePtr});
            return;
        }
        if (qName && qName->getPackageName() == "cajeta.lang"
                && qName->getTypeName().rfind("Slice", 0) == 0
                && isValueType()) {
            llvm::Function* fn = cajModule->getRuntimeFunction(
                retain ? "__cajeta_slice_retain" : "__cajeta_slice_release",
                bodyModule);
            if (fn) b.CreateCall(fn, {valuePtr});
            return;
        }
        for (auto& property : propertyList) {
            if (property->isStatic()) continue;
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(property->getType());
            if (!fieldClass || dynamic_pointer_cast<CajetaView>(property->getType())
                    || !fieldClass->isSharedCapableValue()) {
                continue;
            }
            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);
            llvm::Value* slot = b.CreateStructGEP(
                rawLlvmType(), valuePtr, fieldIdx,
                std::string(retain ? "vret_" : "vrel_") + property->getName());
            fieldClass->emitValueSharedOp(b, slot, cajModule, bodyModule, retain);
        }
    }

    // Emits (or reuses) `__cajeta_vrel_<class>`, which releases one value of this
    // type through emitValueSharedOp. LinkOnceODR plus comdat: it is materialized
    // per use and must merge across TUs rather than collide.
    llvm::Function* CajetaClass::getOrCreateValueReleaseFunction() {
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        std::string relName = std::string("__cajeta_vrel_") + qName->toCanonical();
        for (char& c : relName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }
        if (llvm::Function* existing = lmod->getFunction(relName)) {
            return existing;
        }
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);
        llvm::Function* fn = llvm::Function::Create(fnTy,
            llvm::Function::LinkOnceODRLinkage, relName, lmod);
        {
            llvm::Triple relTriple(lmod->getTargetTriple());
            if (!relTriple.isOSBinFormatMachO()) {
                fn->setComdat(lmod->getOrInsertComdat(relName));
            }
        }
        llvm::Module* prevEmitLlvm = CajetaModule::getCurrentEmitLlvmModule();
        CajetaModule::setCurrentEmitLlvmModule(lmod);
        struct RestoreEmitLlvm {
            llvm::Module* prev;
            ~RestoreEmitLlvm() { CajetaModule::setCurrentEmitLlvmModule(prev); }
        } restoreEmitLlvm{prevEmitLlvm};
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::IRBuilder<> b(bb);
        emitValueSharedOp(b, fn->getArg(0), module, lmod, /*retain=*/false);
        b.CreateRetVoid();
        return fn;
    }

    // Emits (or reuses) `__cajeta_<class>_drop`: null guard, this class's drop body,
    // every ancestor's at its sub-object offset, then one __cajeta_free. Wildcard
    // proxies and String route to runtime drops instead.
    llvm::Function* CajetaClass::getOrCreateDropFunction() {
        if (interfaceFlag) return nullptr;
        // Never cache these two runtime handles: they resolve into the CURRENT
        // emit module, so a cached one carries a stale module binding forward.
        if (isWildcardInstantiation()) {
            return module->getRuntimeFunction("__cajeta_class_virtual_drop");
        }
        if (qName && qName->toCanonical() == "cajeta.lang.String") {
            return module->getRuntimeFunction("__cajeta_string_drop_claimed");
        }
        auto& llvmDropFunction = dropFnRef();
        if (llvmDropFunction) return llvmDropFunction;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);

        std::string dropName = std::string("__cajeta_") + qName->toCanonical() + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }

        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmDropFunction = existing;
            return existing;
        }

        // LinkOnceODR plus comdat: the wrapper is materialized per use and can be
        // emitted into several TUs. The body is deterministic, so ODR holds.
        llvmDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::LinkOnceODRLinkage, dropName, lmod);
        {
            llvm::Triple dropTriple(lmod->getTargetTriple());
            if (!dropTriple.isOSBinFormatMachO()) {
                llvmDropFunction->setComdat(lmod->getOrInsertComdat(dropName));
            }
        }
        // Pin the emit module to `lmod` while emitting: the body's runtime callees
        // resolve into the current emit module, a different one under test reuse.
        llvm::Module* prevDropEmitLlvm = CajetaModule::getCurrentEmitLlvmModule();
        CajetaModule::setCurrentEmitLlvmModule(lmod);
        struct RestoreDropEmitLlvm {
            llvm::Module* prev;
            ~RestoreDropEmitLlvm() { CajetaModule::setCurrentEmitLlvmModule(prev); }
        } restoreDropEmitLlvm{prevDropEmitLlvm};
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* instance = llvmDropFunction->getArg(0);

        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(instance,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);

        b.SetInsertPoint(doDrop);

        emitDropBodyInline(b, instance, module);

        for (auto& ancestor : collectDestructorChain()) {
            uint64_t off = getSubObjectByteOffset(ancestor.get());
            llvm::Value* ancestorThis = instance;
            if (off != 0) {
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                ancestorThis = b.CreateInBoundsGEP(i8Ty, instance,
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(ctx), off),
                    std::string("dtor_subobj_")
                        + ancestor->getQName()->getTypeName());
            }
            ancestor->emitDropBodyInline(b, ancestorThis, module);
        }

        llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free", lmod);
        if (freeFn) {
            b.CreateCall(freeFn, {instance});
        }

        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmDropFunction;
    }

    // Declaration of the reflective invoke adapter, filled by emitReflectInvokeBody:
    //   void __cajeta_<canonical>_reflect_invoke(ptr obj, i32 idx, ptr args, ptr ret)
    llvm::Function* CajetaClass::getOrCreateReflectInvokeDecl() {
        auto& llvmReflectInvokeFunction = reflectInvokeFnRef();
        if (llvmReflectInvokeFunction) return llvmReflectInvokeFunction;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx),
            {(llvm::Type*) ptrTy, i32Ty, (llvm::Type*) ptrTy, (llvm::Type*) ptrTy},
            /*isVarArg=*/false);
        std::string name = std::string("__cajeta_") + qName->toCanonical() + "_reflect_invoke";
        for (char& c : name) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') c = '_';
        }
        if (llvm::Function* existing = lmod->getFunction(name)) {
            llvmReflectInvokeFunction = existing;
            return existing;
        }
        llvmReflectInvokeFunction = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, name, lmod);
        return llvmReflectInvokeFunction;
    }

    // Fills the invoke adapter: a switch over the method-list index whose arms load
    // each argument from the 8-byte-strided `args` buffer, make a direct call, and
    // store a scalar result to `ret`. Unmarshallable shapes fall to the default.
    void CajetaClass::emitReflectInvokeBody() {
        auto& llvmReflectInvokeBodyEmitted = reflectInvokeBodyEmittedRef();
        auto& llvmReflectInvokeFunction = reflectInvokeFnRef();
        if (llvmReflectInvokeBodyEmitted) return;
        llvm::Function* fn = llvmReflectInvokeFunction
            ? llvmReflectInvokeFunction : getOrCreateReflectInvokeDecl();
        if (!fn) return;
        llvmReflectInvokeBodyEmitted = true;
        if (!fn->empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = fn->getParent();
        llvm::IntegerType* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

        llvm::Value* objArg = fn->getArg(0);
        llvm::Value* idxArg = fn->getArg(1);
        llvm::Value* argsArg = fn->getArg(2);
        llvm::Value* retArg = fn->getArg(3);

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::BasicBlock* end = llvm::BasicBlock::Create(ctx, "end", fn);
        llvm::IRBuilder<> b(entry);

        auto& methods = getMethodList();
        llvm::SwitchInst* sw = b.CreateSwitch(idxArg, end, (unsigned) methods.size());

        bool sealed = getModifiers().count(REFLECT_SEALED) != 0;

        int i = -1;
        for (auto& m : methods) {
            i++;
            if (!m || m->isConstructor()) continue;
            if (sealed && m->getModifiers().count(PRIVATE)) continue;
            llvm::Function* callee = m->getLlvmFunction();
            if (!callee) continue;
            if (callee->isDeclaration()) continue;
            llvm::FunctionType* cTy = callee->getFunctionType();
            if (cTy->isVarArg()) continue;

            auto pl = m->getParameterList();
            if (cTy->getNumParams() != pl.size()) continue;
            bool hasThis = !pl.empty() && pl.front()->getName() == "this";
            unsigned userStart = hasThis ? 1u : 0u;

            auto marshallable = [](llvm::Type* t) {
                return t->isIntegerTy() || t->isFloatingPointTy() || t->isPointerTy();
            };
            bool ok = true;
            for (unsigned p = 0; p < cTy->getNumParams() && ok; ++p)
                ok = marshallable(cTy->getParamType(p));
            llvm::Type* rt = cTy->getReturnType();
            if (ok && !rt->isVoidTy() && !marshallable(rt)) ok = false;
            if (!ok) continue;

            llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(
                ctx, std::string("invoke_") + std::to_string(i), fn);
            sw->addCase(llvm::ConstantInt::get(i32Ty, (uint64_t) i), caseBB);
            b.SetInsertPoint(caseBB);

            llvm::Function* calleeInMod = CajetaModule::ensureFunctionInModule(lmod, callee);
            std::vector<llvm::Value*> callArgs;
            if (hasThis) {
                llvm::Type* thisTy = cTy->getParamType(0);
                if (thisTy->isPointerTy()) {
                    callArgs.push_back(objArg);
                } else {
                    // Enum companion: `this` is the i32 ordinal, not an address, so
                    // load it out of the receiver the reflective caller handed us.
                    callArgs.push_back(b.CreateLoad(thisTy, objArg, "this.ord"));
                }
            }
            for (unsigned p = userStart; p < cTy->getNumParams(); ++p) {
                llvm::Type* pt = cTy->getParamType(p);
                llvm::Value* slot = b.CreateInBoundsGEP(i64Ty, argsArg,
                    llvm::ConstantInt::get(i64Ty, p - userStart),
                    std::string("argslot_") + std::to_string(p - userStart));
                callArgs.push_back(b.CreateLoad(pt, slot,
                    std::string("arg_") + std::to_string(p - userStart)));
            }
            llvm::Value* result = b.CreateCall(calleeInMod->getFunctionType(),
                calleeInMod, callArgs);
            if (!rt->isVoidTy()) b.CreateStore(result, retArg);
            b.CreateBr(end);
        }

        b.SetInsertPoint(end);
        b.CreateRetVoid();
    }

    // This class's constructors sorted by canonical signature: the stable index
    // space the newInstance adapter and the #Rtti constructor table share.
    // Constructors live in `methods`, never in methodList.
    std::vector<MethodPtr> CajetaClass::getReflectConstructorList() {
        std::vector<MethodPtr> ctors;
        for (auto& entry : methods) {
            if (entry.second && entry.second->isConstructor())
                ctors.push_back(entry.second);
        }
        std::sort(ctors.begin(), ctors.end(),
            [](const MethodPtr& a, const MethodPtr& b) {
                return a->toCanonical() < b->toCanonical();
            });
        return ctors;
    }

    // Declaration of the reflective newInstance adapter, filled by emitReflectNewBody:
    //   ptr __cajeta_<canonical>_reflect_new(i32 ctorIndex, ptr args)
    llvm::Function* CajetaClass::getOrCreateReflectNewDecl() {
        auto& llvmReflectNewFunction = reflectNewFnRef();
        if (llvmReflectNewFunction) return llvmReflectNewFunction;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            (llvm::Type*) ptrTy, {i32Ty, (llvm::Type*) ptrTy}, /*isVarArg=*/false);
        std::string name = std::string("__cajeta_") + qName->toCanonical() + "_reflect_new";
        for (char& c : name) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') c = '_';
        }
        if (llvm::Function* existing = lmod->getFunction(name)) {
            llvmReflectNewFunction = existing;
            return existing;
        }
        llvmReflectNewFunction = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, name, lmod);
        return llvmReflectNewFunction;
    }

    // Fills the newInstance adapter: a switch over the constructor index whose arms
    // alloc and zero the instance, install its vtables, patch drop_fn, run the
    // chosen constructor and return it. Unknown indices return null.
    void CajetaClass::emitReflectNewBody() {
        auto& llvmReflectNewBodyEmitted = reflectNewBodyEmittedRef();
        auto& llvmReflectNewFunction = reflectNewFnRef();
        if (llvmReflectNewBodyEmitted) return;
        llvm::Function* fn = llvmReflectNewFunction
            ? llvmReflectNewFunction : getOrCreateReflectNewDecl();
        if (!fn) return;
        llvmReflectNewBodyEmitted = true;
        if (!fn->empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = fn->getParent();
        llvm::IntegerType* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::Value* idxArg = fn->getArg(0);
        llvm::Value* argsArg = fn->getArg(1);

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(ctx, "fail", fn);
        llvm::IRBuilder<> b(entry);

        auto ctors = getReflectConstructorList();
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(getLlvmType());
        llvm::Function* allocFn = module->getRuntimeFunction("__cajeta_alloc",
            /*explicitTarget=*/lmod);
        uint64_t allocSize = st ? lmod->getDataLayout().getTypeAllocSize(st) : 0;

        llvm::SwitchInst* sw = b.CreateSwitch(idxArg, fail, (unsigned) ctors.size());

        auto marshallable = [](llvm::Type* t) {
            return t->isIntegerTy() || t->isFloatingPointTy() || t->isPointerTy();
        };

        bool sealed = getModifiers().count(REFLECT_SEALED) != 0;

        int i = -1;
        for (auto& ctor : ctors) {
            i++;
            if (!ctor || !st || !allocFn) continue;
            if (sealed && ctor->getModifiers().count(PRIVATE)) continue;
            llvm::Function* callee = ctor->getLlvmFunction();
            if (!callee || callee->isDeclaration()) continue;
            llvm::FunctionType* cTy = callee->getFunctionType();
            if (cTy->isVarArg()) continue;
            auto pl = ctor->getParameterList();
            if (cTy->getNumParams() != pl.size()) continue;
            bool hasThis = !pl.empty() && pl.front()->getName() == "this";
            unsigned userStart = hasThis ? 1u : 0u;
            bool ok = true;
            for (unsigned p = 0; p < cTy->getNumParams() && ok; ++p)
                ok = marshallable(cTy->getParamType(p));
            if (!ok) continue;

            llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(
                ctx, std::string("new_") + std::to_string(i), fn);
            sw->addCase(llvm::ConstantInt::get(i32Ty, (uint64_t) i), caseBB);
            b.SetInsertPoint(caseBB);

            llvm::Value* inst = b.CreateCall(allocFn,
                {llvm::ConstantInt::get(i64Ty, allocSize)}, "inst");
            b.CreateMemSet(inst, llvm::ConstantInt::get(i8Ty, 0),
                llvm::ConstantInt::get(i64Ty, allocSize), llvm::MaybeAlign(8));

            if (hasVtablePointerAtSlotZero()) {
                if (llvm::GlobalVariable* vt = getVirtualTableGlobal()) {
                    llvm::Constant* vtRef =
                        CajetaModule::ensureGlobalInModule(lmod, vt);
                    llvm::Value* slot0 = b.CreateStructGEP(st, inst, 0, "vtable_slot");
                    b.CreateStore(vtRef, slot0);
                }
                for (auto& sub : getNonFirstSubObjects()) {
                    llvm::GlobalVariable* secVT = getOrCreateSecondaryVTable(sub.ancestor);
                    if (!secVT) continue;
                    llvm::Constant* secRef =
                        CajetaModule::ensureGlobalInModule(lmod, secVT);
                    llvm::Value* secSlot = b.CreateStructGEP(
                        st, inst, (unsigned) sub.slot, "sec_vtable_slot");
                    b.CreateStore(secRef, secSlot);
                }
                patchVirtualTableDropFn();
            }

            llvm::Function* calleeInMod = CajetaModule::ensureFunctionInModule(lmod, callee);
            std::vector<llvm::Value*> callArgs;
            if (hasThis) callArgs.push_back(inst);
            for (unsigned p = userStart; p < cTy->getNumParams(); ++p) {
                llvm::Type* pt = cTy->getParamType(p);
                llvm::Value* slot = b.CreateInBoundsGEP(i64Ty, argsArg,
                    llvm::ConstantInt::get(i64Ty, p - userStart),
                    std::string("argslot_") + std::to_string(p - userStart));
                callArgs.push_back(b.CreateLoad(pt, slot,
                    std::string("arg_") + std::to_string(p - userStart)));
            }
            b.CreateCall(calleeInMod->getFunctionType(), calleeInMod, callArgs);
            b.CreateRet(inst);
        }

        b.SetInsertPoint(fail);
        b.CreateRet(llvm::ConstantPointerNull::get(ptrTy));
    }

    // The sole class-registration site. Patches slot 0 of #ClassObject with the
    // Class<?> vtable for classes parsed before cajeta.reflect.Class, then emits the
    // keepsClass-gated registration ctor for any class whose slot 0 is non-null.
    void CajetaClass::finalizeClassObject() {
        llvm::GlobalVariable* co = getClassObjectGlobal();
        if (!co || !co->hasInitializer()) return;
        auto* init = llvm::dyn_cast<llvm::ConstantStruct>(co->getInitializer());
        if (!init || init->getNumOperands() < 2) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = getEmitModule()->getLlvmModule();

        if (init->getOperand(0)->isNullValue()) {
            auto& s2m = CajetaModule::getStructureToModule();
            auto mit = s2m.find("cajeta.reflect.Class<?>");
            if (mit == s2m.end() || !mit->second) return;
            auto& structs = mit->second->getStructures();
            auto sit = structs.find("cajeta.reflect.Class<?>");
            if (sit == structs.end() || !sit->second) return;
            llvm::GlobalVariable* cv = sit->second->getVirtualTableGlobal();
            if (!cv) return;
            llvm::Constant* classVtableRef =
                CajetaModule::ensureGlobalInModule(lmod, cv);
            auto* coTy = llvm::cast<llvm::StructType>(co->getValueType());
            co->setInitializer(llvm::ConstantStruct::get(
                coTy, {classVtableRef, init->getOperand(1)}));
        }

        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        std::string canon = toCanonical();
        std::string regCtorName = "__cajeta_class_reg_ctor." + canon;
        if (!co->getInitializer()->getAggregateElement(0u)->isNullValue()
                && module->keepsClass(canon)
                && lmod->getFunction(regCtorName) == nullptr) {
            llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
            llvm::FunctionType* regTy =
                llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
            llvm::FunctionCallee regFn =
                lmod->getOrInsertFunction("__cajeta_register_class", regTy);
            llvm::Function* regCtor = llvm::Function::Create(
                llvm::FunctionType::get(voidTy, false),
                llvm::GlobalValue::InternalLinkage,
                regCtorName, lmod);
            llvm::IRBuilder<> rb(llvm::BasicBlock::Create(ctx, "entry", regCtor));
            llvm::Constant* nameStr =
                rb.CreateGlobalString(canon, "cajeta.class.name." + canon);
            rb.CreateCall(regFn, {nameStr, co});
            rb.CreateRetVoid();
            llvm::appendToGlobalCtors(*lmod, regCtor, /*Priority=*/65535);
        }
    }

    // Instantiates cajeta.reflect.Class<?> so its bodies are emitted and its vtable
    // backs every #ClassObject. Idempotent, and a no-op when reflect is absent from
    // the compile unit.
    void CajetaClass::ensureClassWildcardInstantiated() {
        auto& cmap = CajetaType::getCanonicalMap();
        auto cit = cmap.find("cajeta.reflect.Class");
        if (cit == cmap.end()) return;
        auto classTmpl = std::dynamic_pointer_cast<CajetaClass>(cit->second);
        if (!classTmpl || !classTmpl->isTemplate()) return;
        CajetaTypePtr wild = CajetaType::wildcardSentinel();
        if (!wild) return;
        classTmpl->instantiate({wild});
    }

    struct MethodEntry {
        MethodPtr method;
        int score;
        MethodEntry(MethodPtr method) { this->method = method; score = 0; }
    };

    // The candidate in `canonical` whose labeled parameters accept `parameters` with
    // the smallest cumulative rank widening; null when none of them does.
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

    // TODO: call this before addMethods, once parent classes are loaded.
    // Re-indexes `structure`'s methods, ancestors first, into this class's
    // labeled and unlabeled method maps.
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

    // Resolves `extends` names into superClasses: module structures by canonical,
    // then by short name, then the process-global canonicalMap, which may yield a
    // placeholder deliberately — tryGeneratePrototype defers on one.
    void CajetaClass::resolveSuperClasses() {
        superClasses.clear();
        for (auto& qName : qExtended) {
            auto& structures = module->getStructures();
            auto it = structures.find(qName->toCanonical());
            if (it != structures.end()) {
                superClasses.push_back(it->second);
                continue;
            }
            bool found = false;
            for (auto& entry : structures) {
                if (entry.second->getQName()->getTypeName() == qName->getTypeName()) {
                    superClasses.push_back(entry.second);
                    found = true;
                    break;
                }
            }
            if (found) continue;
            auto& canon = canonicalMap;
            auto canonIt = canon.find(qName->toCanonical());
            if (canonIt == canon.end()) {
                canonIt = canon.find(qName->getTypeName());
            }
            if (canonIt != canon.end()) {
                if (auto klass = std::dynamic_pointer_cast<CajetaClass>(canonIt->second)) {
                    superClasses.push_back(klass);
                }
            }
        }
    }

    // Runs generatePrototype only once every superclass, interface and signature-
    // referenced @ValueType is laid out; returns false to defer. An uninstantiated
    // template counts as done, since it never sets prototypeBuilt.
    bool CajetaClass::tryGeneratePrototype() {
        if (prototypeBuilt) return true;
        resolveSuperClasses();
        resolveImplementedInterfaces();
        for (auto& parent : superClasses) {
            if (!parent) continue;
            if (parent->isTemplate()) continue;
            if (parent->isPlaceholder()) return false;
            if (!parent->prototypeBuilt) return false;
        }
        for (auto& iface : implementedInterfaces) {
            if (!iface) continue;
            if (iface->isTemplate()) continue;
            if (iface->isPlaceholder()) return false;
            if (!iface->prototypeBuilt) return false;
        }
        // A value type passes BY VALUE and its flat struct is baked into the method
        // signature, which is created exactly once. A not-yet-parsed placeholder
        // lowers to `ptr`, freezing the wrong ABI, so defer instead.
        auto valueTypePlaceholder = [](const CajetaTypePtr& t) -> bool {
            if (!t || !t->getQName()) return false;
            const std::string canonical = t->getQName()->toCanonical();
            if (!t->isValueType()
                    && !CajetaType::isArchiveValueType(canonical)) {
                return false;
            }
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(canonical);
            if (it == cmap.end() || !it->second) return true;
            auto cls = std::dynamic_pointer_cast<CajetaClass>(it->second);
            return cls && cls->isPlaceholder();
        };
        for (auto& methodEntry : methods) {
            auto& method = methodEntry.second;
            if (!method) continue;
            if (valueTypePlaceholder(method->getReturnType())) return false;
            for (auto& p : method->getParameterList()) {
                if (p && valueTypePlaceholder(p->getType())) return false;
            }
        }
        generatePrototype();
        return true;
    }

    // Resolves `implements` names into implementedInterfaces (module structures, the
    // global canonicalMap, then by short name), instantiating a generic interface
    // with the clause's type arguments. Non-interfaces are skipped.
    void CajetaClass::resolveImplementedInterfaces() {
        implementedInterfaces.clear();
        auto& structures = module->getStructures();
        auto qiArgsIter = qImplementedTypeArgs.begin();
        for (auto& qn : qImplemented) {
            CajetaClassPtr found;
            auto it = structures.find(qn->toCanonical());
            if (it != structures.end()) {
                found = it->second;
            } else {
                for (auto& entry : structures) {
                    if (entry.second->getQName()->getTypeName() == qn->getTypeName()) {
                        found = entry.second;
                        break;
                    }
                }
            }
            if (!found) {
                auto& canonMap = CajetaType::getCanonicalMap();
                auto cit = canonMap.find(qn->toCanonical());
                if (cit != canonMap.end()) {
                    found = std::dynamic_pointer_cast<CajetaClass>(cit->second);
                }
                if (!found) {
                    for (auto& [canon, t] : canonMap) {
                        auto cls = std::dynamic_pointer_cast<CajetaClass>(t);
                        if (!cls) continue;
                        auto cqn = cls->getQName();
                        if (cqn && cqn->getTypeName() == qn->getTypeName()) {
                            found = cls;
                            break;
                        }
                    }
                }
            }
            if (found && found->isInterface() && found->isTemplate()
                    && qiArgsIter != qImplementedTypeArgs.end()
                    && !qiArgsIter->empty()) {
                vector<CajetaTypePtr> resolvedArgs;
                resolvedArgs.reserve(qiArgsIter->size());
                auto& canonMap = CajetaType::getCanonicalMap();
                for (auto& argQn : *qiArgsIter) {
                    CajetaTypePtr argType;
                    auto cit = canonMap.find(argQn->toCanonical());
                    if (cit != canonMap.end()) {
                        argType = cit->second;
                    } else {
                        auto nit = canonMap.find(argQn->getTypeName());
                        if (nit != canonMap.end()) argType = nit->second;
                    }
                    if (!argType) break;
                    resolvedArgs.push_back(argType);
                }
                if (resolvedArgs.size() == qiArgsIter->size()) {
                    auto inst = found->instantiate(resolvedArgs);
                    if (inst) found = inst;
                }
            }
            if (found && found->isInterface()) {
                implementedInterfaces.push_back(found);
            }
            if (qiArgsIter != qImplementedTypeArgs.end()) ++qiArgsIter;
        }
    }

    // Computes this class's virtual slots: the unique method set walked parent-first
    // with overrides matched by name+params suffix, aliased under parent, interface
    // and template-origin canonicals, then sorted by signature hash.
    void CajetaClass::buildVirtualTable() {
        virtualMethodList.clear();
        virtualSlotHashList.clear();
        map<string, MethodPtr> uniqueByCanonical;

        auto suffixOf = [](const string& canon) -> string {
            auto pos = canon.rfind("::");
            return (pos == string::npos) ? canon : canon.substr(pos + 2);
        };

        for (auto& m : methodList) {
            if (!m) continue;
            auto overrideAnn = m->findAnnotation("Override");
            if (!overrideAnn) continue;
            std::string fromName = overrideAnn->getClassRef("from");
            if (fromName.empty()) fromName = overrideAnn->getString("from");
            if (fromName.empty()) continue;
            CajetaClassPtr fromClass;
            for (auto& [canon, t] : CajetaType::getCanonicalMap()) {
                if (auto cls = std::dynamic_pointer_cast<CajetaClass>(t)) {
                    auto qn = cls->getQName();
                    if (qn && (qn->getTypeName() == fromName
                            || qn->toCanonical() == fromName)) {
                        fromClass = cls;
                        break;
                    }
                }
            }
            bool isAncestor = false;
            if (fromClass) {
                std::function<bool(CajetaClassPtr)> walkAncestors =
                    [&](CajetaClassPtr c) -> bool {
                        if (!c) return false;
                        for (auto& sup : c->getSuperClasses()) {
                            if (sup.get() == fromClass.get()) return true;
                            if (walkAncestors(sup)) return true;
                        }
                        return false;
                    };
                isAncestor = walkAncestors(
                    static_pointer_cast<CajetaClass>(shared_from_this()));
            }
            if (!fromClass || !isAncestor) {
                std::string msg = "@Override(from=" + fromName + ") on '"
                    + m->toCanonical(/*labeled=*/false) + "' in class '"
                    + qName->toCanonical() + "': '" + fromName
                    + "' is not an ancestor of '" + qName->toCanonical()
                    + "'";
                throw Exception(msg, "CAJETA_ERROR_OVERRIDE_FROM_MISMATCH");
            }
            std::string ourSuffix = suffixOf(m->toCanonical(/*labeled=*/false));
            bool sameSuffixFound = false;
            for (auto& fm : fromClass->getMethodList()) {
                if (!fm) continue;
                if (fm->isConstructor()) continue;
                if (fm->getModifiers().find(STATIC) != fm->getModifiers().end()) continue;
                if (suffixOf(fm->toCanonical(/*labeled=*/false)) == ourSuffix) {
                    sameSuffixFound = true;
                    break;
                }
            }
            if (!sameSuffixFound) {
                std::string msg = "@Override(from=" + fromName + ") on '"
                    + m->toCanonical(/*labeled=*/false) + "' in class '"
                    + qName->toCanonical() + "': '"
                    + fromClass->getQName()->toCanonical()
                    + "' does not declare a method with matching name + parameters";
                throw Exception(msg, "CAJETA_ERROR_OVERRIDE_FROM_MISMATCH");
            }
        }
        std::function<void(CajetaClassPtr)> walk = [&](CajetaClassPtr c) {
            for (auto& sup : c->getSuperClasses()) walk(sup);
            for (auto& m : c->getMethodList()) {
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                if (m->isAbstract()) continue;
                if (m->isMethodTemplate()) continue;
                string canon = m->toCanonical(/*labeled=*/false);
                string suffix = suffixOf(canon);
                for (auto it = uniqueByCanonical.begin();
                        it != uniqueByCanonical.end(); ) {
                    if (suffixOf(it->first) == suffix) {
                        it = uniqueByCanonical.erase(it);
                    } else {
                        ++it;
                    }
                }
                uniqueByCanonical[canon] = m;
            }
        };
        walk(static_pointer_cast<CajetaClass>(shared_from_this()));

        {
            std::map<std::string,
                std::vector<std::pair<CajetaClassPtr, MethodPtr>>> bySuffixAll;
            std::function<void(CajetaClassPtr)> gather = [&](CajetaClassPtr c) {
                for (auto& sup : c->getSuperClasses()) {
                    if (sup) gather(sup);
                }
                for (auto& m : c->getMethodList()) {
                    if (!m) continue;
                    if (m->isConstructor()) continue;
                    if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                    if (m->isMethodTemplate()) continue;
                    std::string suffix = suffixOf(m->toCanonical(/*labeled=*/false));
                    bySuffixAll[suffix].push_back({c, m});
                }
            };
            auto self = static_pointer_cast<CajetaClass>(shared_from_this());
            gather(self);

            std::function<bool(CajetaClassPtr, CajetaClassPtr)> isAncestor =
                [&](CajetaClassPtr anc, CajetaClassPtr desc) -> bool {
                    if (!anc || !desc) return false;
                    if (anc.get() == desc.get()) return true;
                    for (auto& sup : desc->getSuperClasses()) {
                        if (isAncestor(anc, sup)) return true;
                    }
                    return false;
                };

            auto retCanon = [](const MethodPtr& m) -> std::string {
                auto rt = m->getReturnType();
                if (!rt) return std::string("<void>");
                return rt->toCanonical();
            };
            auto returnsCompatible = [&](const MethodPtr& a,
                                         const MethodPtr& b) -> bool {
                if (retCanon(a) == retCanon(b)) return true;
                auto ra = std::dynamic_pointer_cast<CajetaClass>(a->getReturnType());
                auto rb = std::dynamic_pointer_cast<CajetaClass>(b->getReturnType());
                if (!ra || !rb) return false;
                return isAncestor(ra, rb) || isAncestor(rb, ra);
            };

            for (auto& [suffix, decls] : bySuffixAll) {
                if (decls.size() < 2) continue;

                for (size_t i = 1; i < decls.size(); ++i) {
                    if (!returnsCompatible(decls.front().second, decls[i].second)) {
                        std::string msg = "class '" + qName->toCanonical()
                            + "': method '" + suffix
                            + "' is declared with conflicting return types — '"
                            + decls.front().first->getQName()->toCanonical()
                            + "' returns '" + retCanon(decls.front().second)
                            + "' but '" + decls[i].first->getQName()->toCanonical()
                            + "' returns '" + retCanon(decls[i].second)
                            + "'. Cajeta allows covariant overrides (subclass "
                            + "narrows the return type) but unrelated types "
                            + "cannot share a vtable slot";
                        throw Exception(msg,
                            "CAJETA_ERROR_RETURN_TYPE_COLLISION");
                    }
                }

                bool selfDeclares = false;
                std::vector<std::pair<CajetaClassPtr, MethodPtr>> concrete;
                for (auto& [cls, m] : decls) {
                    if (cls.get() == self.get() && !m->isAbstract()) {
                        selfDeclares = true;
                    }
                    if (!m->isAbstract()) {
                        concrete.push_back({cls, m});
                    }
                }
                if (selfDeclares) continue;
                if (concrete.size() < 2) continue;

                CajetaClassPtr siblingA, siblingB;
                for (size_t i = 0; i < concrete.size() && !siblingA; ++i) {
                    for (size_t j = i + 1; j < concrete.size(); ++j) {
                        if (concrete[i].first.get() == concrete[j].first.get()) continue;
                        bool aIsAncOfB = isAncestor(concrete[i].first, concrete[j].first);
                        bool bIsAncOfA = isAncestor(concrete[j].first, concrete[i].first);
                        if (!aIsAncOfB && !bIsAncOfA) {
                            siblingA = concrete[i].first;
                            siblingB = concrete[j].first;
                            break;
                        }
                    }
                }
                if (siblingA && siblingB) {
                    std::string msg = "class '" + qName->toCanonical()
                        + "': call to '" + suffix + "' is ambiguous; both '"
                        + siblingA->getQName()->toCanonical() + "::" + suffix
                        + "' and '"
                        + siblingB->getQName()->toCanonical() + "::" + suffix
                        + "' reach this class through different parents. "
                        + "Resolve by either (1) overriding '" + suffix
                        + "' in '" + qName->toCanonical()
                        + "' or (2) qualifying the call via 'super<Base>."
                        + suffix + "' (MultiClassing Phase 2)";
                    throw Exception(msg,
                        "CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH");
                }
            }
        }

        {
            map<string, MethodPtr> bySuffix;
            for (auto& [canon, m] : uniqueByCanonical) {
                bySuffix[suffixOf(canon)] = m;
            }
            // Abstract methods are deliberately NOT skipped: a sibling's concrete
            // impl satisfies an abstract obligation, and dispatch can still arrive
            // on the abstract declaration's canonical.
            std::function<void(CajetaClassPtr)> aliasWalk = [&](CajetaClassPtr c) {
                for (auto& sup : c->getSuperClasses()) {
                    for (auto& m : sup->getMethodList()) {
                        if (m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                        if (m->isMethodTemplate()) continue;
                        string supCanon = m->toCanonical(/*labeled=*/false);
                        auto it = bySuffix.find(suffixOf(supCanon));
                        if (it != bySuffix.end()) {
                            uniqueByCanonical[supCanon] = it->second;
                        }
                    }
                    aliasWalk(sup);
                }
            };
            aliasWalk(static_pointer_cast<CajetaClass>(shared_from_this()));
        }

        auto findConcreteFor = [&](MethodPtr abstractM) -> MethodPtr {
            string targetSig = abstractM->toCanonical(/*labeled=*/false);
            auto pos = targetSig.rfind("::");
            string suffix = (pos == string::npos) ? targetSig : targetSig.substr(pos + 2);
            for (auto& [canon, m] : uniqueByCanonical) {
                auto p = canon.rfind("::");
                if (p == string::npos) continue;
                if (canon.substr(p + 2) == suffix) return m;
            }
            return nullptr;
        };
        std::set<CajetaClass*> visitedIfaces;
        std::function<void(CajetaClassPtr)> walkIface = [&](CajetaClassPtr iface) {
            if (!iface) return;
            if (!visitedIfaces.insert(iface.get()).second) return;
            for (auto& m : iface->getMethodList()) {
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                if (auto concrete = findConcreteFor(m)) {
                    if (m->isReturnsView() != concrete->isReturnsView()) {
                        throw Exception(
                            "class '" + qName->toCanonical() + "' implements '"
                            + m->toCanonical(/*labeled=*/false)
                            + "' with a mismatched return stance: the "
                            "interface declares "
                            + (m->isReturnsView()
                                ? std::string("`^` (a view the caller must "
                                              "not free)")
                                : std::string("a non-view return"))
                            + " but the implementation declares "
                            + (concrete->isReturnsView()
                                ? std::string("`^`")
                                : (concrete->isReturnsOwnership()
                                    ? std::string("`#` (an owned transfer)")
                                    : std::string("a plain return")))
                            + ". Interface-typed call sites apply the "
                            "interface's rules, so the mismatch is a leak or "
                            "a double free depending on direction. Fix: make "
                            "the stances agree. See "
                            "specs/stdlib-ownership-convention-spec.md §4.7.",
                            "CAJETA_ERROR_VIEW_STANCE_MISMATCH");
                    }
                    uniqueByCanonical[m->toCanonical(/*labeled=*/false)] = concrete;
                    continue;
                }
                std::string msg = "class '" + qName->toCanonical()
                    + "' implements interface '" + iface->getQName()->toCanonical()
                    + "' but does not provide '" + m->toCanonical(/*labeled=*/false)
                    + "'";
                throw Exception(msg, "CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED");
            }
            for (auto& parent : iface->getSuperClasses()) {
                if (parent && parent->isInterface()) walkIface(parent);
            }
        };
        for (auto& iface : implementedInterfaces) {
            walkIface(iface);
        }

        {
            bool selfIsAbstract = false;
            for (auto& m : methodList) {
                if (m && m->isAbstract()) { selfIsAbstract = true; break; }
            }
            if (!selfIsAbstract) {
                auto suffixOf = [](const std::string& canon) -> std::string {
                    auto pos = canon.rfind("::");
                    return (pos == std::string::npos) ? canon : canon.substr(pos + 2);
                };
                std::function<void(CajetaClassPtr)> checkAbstracts =
                    [&](CajetaClassPtr c) {
                        for (auto& m : c->getMethodList()) {
                            if (!m || !m->isAbstract()) continue;
                            std::string targetSuffix = suffixOf(
                                m->toCanonical(/*labeled=*/false));
                            bool covered = false;
                            for (auto& [canon, mm] : uniqueByCanonical) {
                                if (!mm || mm->isAbstract()) continue;
                                if (suffixOf(canon) == targetSuffix) {
                                    covered = true;
                                    break;
                                }
                            }
                            if (!covered) {
                                std::string msg = "class '" + qName->toCanonical()
                                    + "' inherits abstract method '"
                                    + m->toCanonical(/*labeled=*/false)
                                    + "' from '" + c->getQName()->toCanonical()
                                    + "' but does not override it";
                                throw Exception(msg,
                                    "CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED");
                            }
                        }
                        for (auto& sup : c->getSuperClasses()) {
                            if (sup) checkAbstracts(sup);
                        }
                    };
                checkAbstracts(static_pointer_cast<CajetaClass>(shared_from_this()));
            }
        }

        {
            map<string, MethodPtr> templateAliases;
            auto addAliasFor = [&](CajetaClassPtr cls, MethodPtr m) {
                if (!cls) return;
                auto origin = cls->getTemplateOrigin();
                if (!origin) return;
                string aliasCanon = Method::buildTemplateOriginCanonical(
                    cls, m->getName(),
                    m->getParameterList(), /*labeled=*/false);
                templateAliases[aliasCanon] = m;
            };
            std::function<void(CajetaClassPtr, MethodPtr)> walkSupers =
                [&](CajetaClassPtr c, MethodPtr m) {
                    if (!c) return;
                    for (auto& sup : c->getSuperClasses()) {
                        addAliasFor(sup, m);
                        walkSupers(sup, m);
                    }
                };
            for (auto& [canon, m] : uniqueByCanonical) {
                if (!m || m->isMethodTemplate()) continue;
                auto mParent = m->getParent();
                if (!mParent) continue;
                addAliasFor(mParent, m);
                walkSupers(mParent, m);
            }
            for (auto& [c, m] : templateAliases) {
                uniqueByCanonical.emplace(c, m);
            }
        }

        // Sorted by hash for binary search at dispatch. FNV-1a is stable across
        // runs, so the compiler and the runtime agree on every slot.
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
            virtualSlotHashList.push_back(hash);
        }
    }

    // Builds the virtual slot list and emits the vtable global plus RTTI.
    // Idempotent — returns immediately once a vtable global exists.
    void CajetaClass::writeVirtualTable() {
        if (vtableGlobalRef() != nullptr) return;
        buildVirtualTable();
        StructureMetadata(module).populate(
            static_pointer_cast<CajetaClass>(shared_from_this()));
    }

    // Inheritance distance from `argType` to `declaredType`: 0 for an exact
    // canonical match, one per hop up extends/implements (BFS), -1 when not
    // assignable. `relaxNullToRef` lets a null literal match any reference formal.
    static int subtypeDistance(CajetaTypePtr declaredType, CajetaTypePtr argType,
                               bool relaxNullToRef = false) {
        if (!declaredType || !argType) return -1;
        if (declaredType->getQName() && argType->getQName()
                && declaredType->getQName()->toCanonical()
                    == argType->getQName()->toCanonical()) {
            return 0;
        }
        if (relaxNullToRef && argType->getQName()
                && argType->getQName()->toCanonical() == "pointer") {
            // ARRAY_TYPE_ID carries PRIMITIVE_FLAG, so only a true SCALAR primitive
            // may reject `null`; an array is a reference type that accepts it.
            bool declIsArray =
                dynamic_pointer_cast<CajetaArray>(declaredType) != nullptr;
            bool declIsPrimitive =
                (declaredType->getTypeFlags() & PRIMITIVE_FLAG) != 0
                && !declIsArray;
            if (declIsPrimitive) return -1;
            return 1000;
        }
        // An enum constant resolves to int32 (its ordinal's type) and enums are
        // i32-backed, so the two interchange here at a nonzero distance.
        {
            auto isEnumT = [](const CajetaTypePtr& t) {
                return t->getQName()
                    && CajetaType::isArchiveEnum(t->getQName()->toCanonical());
            };
            auto isInt32T = [](const CajetaTypePtr& t) {
                return t->getQName()
                    && t->getQName()->toCanonical() == "int32";
            };
            if ((isEnumT(declaredType) && isInt32T(argType))
                    || (isInt32T(declaredType) && isEnumT(argType))) {
                return 1;
            }
        }
        auto argClass = dynamic_pointer_cast<CajetaClass>(argType);
        auto declaredClass = dynamic_pointer_cast<CajetaClass>(declaredType);
        if (!argClass || !declaredClass) return -1;
        if (declaredClass->isWildcardInstantiation()
                && CajetaClass::isAssignableToWildcard(argClass, declaredClass)) {
            return 1;
        }
        const string declaredCanonical = declaredClass->getQName()->toCanonical();
        std::vector<std::pair<CajetaClassPtr, int>> frontier{ {argClass, 0} };
        size_t cursor = 0;
        while (cursor < frontier.size()) {
            auto [cls, depth] = frontier[cursor++];
            for (auto& parent : cls->getSuperClasses()) {
                if (!parent || !parent->getQName()) continue;
                if (parent->getQName()->toCanonical() == declaredCanonical) {
                    return depth + 1;
                }
                frontier.push_back({parent, depth + 1});
            }
            for (auto& iface : cls->getImplementedInterfaces()) {
                if (!iface || !iface->getQName()) continue;
                if (iface->getQName()->toCanonical() == declaredCanonical) {
                    return depth + 1;
                }
                frontier.push_back({iface, depth + 1});
            }
        }
        return -1;
    }

    // The method in `genericMap` named `methodName` whose positional formals accept
    // `parameters` with the smallest cumulative subtype distance. The implicit
    // `this` formal is skipped when present.
    static MethodPtr findSubtypeMatch(
            const map<string, map<string, MethodPtr>>& genericMap,
            const string& methodName,
            const vector<ParameterEntry>& parameters,
            bool relaxNullToRef = false) {
        MethodPtr best;
        int bestScore = std::numeric_limits<int>::max();
        const size_t argCount = parameters.size();
        for (auto& bucket : genericMap) {
            for (auto& entry : bucket.second) {
                MethodPtr method = entry.second;
                if (!method) continue;
                if (method->getName() != methodName) continue;
                std::vector<FormalParameterPtr> ordered = method->getParameterList();
                bool isStatic = method->getModifiers().find(STATIC)
                    != method->getModifiers().end();
                size_t paramOffset = 0;
                if (!isStatic && !ordered.empty()
                        && ordered.front()->getName() == "this") {
                    paramOffset = 1;
                }
                if (ordered.size() - paramOffset != argCount) continue;
                int score = 0;
                bool ok = true;
                for (size_t i = 0; i < argCount; ++i) {
                    int dist = subtypeDistance(
                        ordered[i + paramOffset]->getType(),
                        parameters[i].type,
                        relaxNullToRef);
                    if (dist < 0) { ok = false; break; }
                    score += dist;
                }
                if (ok && score < bestScore) {
                    bestScore = score;
                    best = method;
                }
            }
        }
        return best;
    }

    // Unifies one (formal, arg) pair, recording method-level T-var bindings and
    // recursing into class-template arguments and function types. Returns false
    // only on a contradiction; non-placeholder formals are presumed compatible.
    static bool unifyMethodTemplateFormal(
        CajetaTypePtr formal, CajetaTypePtr arg,
        const std::set<std::string>& tparamNames,
        std::map<std::string, CajetaTypePtr>& bindings) {
        if (!formal || !arg) return false;

        if (auto fc = std::dynamic_pointer_cast<CajetaClass>(formal)) {
            if (fc->isPlaceholder()
                    && tparamNames.count(fc->getQName()->getTypeName())) {
                const std::string& name = fc->getQName()->getTypeName();
                auto existing = bindings.find(name);
                bool argIsVoid = (arg->toCanonical() == "cajeta.void"
                    || arg->toCanonical() == "void");
                if (existing == bindings.end()) {
                    // Skip a void arg: a lambda whose return type could not be
                    // inferred falls back to void and carries no information.
                    if (argIsVoid) return true;
                    bindings[name] = arg;
                    return true;
                }
                // First binding wins: lambda return-type inference is unreliable
                // enough that re-checking the second causes false rejections.
                return true;
            }
            if (!fc->getTypeArguments().empty()) {
                auto ac = std::dynamic_pointer_cast<CajetaClass>(arg);
                if (!ac || ac->getTypeArguments().empty()) return true;
                const auto& fArgs = fc->getTypeArguments();
                const auto& aArgs = ac->getTypeArguments();
                if (fArgs.size() != aArgs.size()) return true;
                for (size_t i = 0; i < fArgs.size(); ++i) {
                    if (!unifyMethodTemplateFormal(
                            fArgs[i], aArgs[i], tparamNames, bindings)) {
                        return false;
                    }
                }
                return true;
            }
            return true;
        }

        if (auto ffn = std::dynamic_pointer_cast<CajetaFunctionType>(formal)) {
            auto afn = std::dynamic_pointer_cast<CajetaFunctionType>(arg);
            if (!afn) return true;
            const auto& fps = ffn->getParameterTypes();
            const auto& aps = afn->getParameterTypes();
            if (fps.size() != aps.size()) return true;
            for (size_t i = 0; i < fps.size(); ++i) {
                if (!unifyMethodTemplateFormal(
                        fps[i], aps[i], tparamNames, bindings)) {
                    return false;
                }
            }
            return unifyMethodTemplateFormal(
                ffn->getReturnType(), afn->getReturnType(),
                tparamNames, bindings);
        }

        return true;
    }

    // Finds a method template on `cls` matching name and arity, unifies its T-vars
    // against the argument types (or takes `explicitArgs` verbatim), and returns a
    // fresh instantiation. Null on no candidate, arity mismatch, or an unbound T.
    static MethodPtr tryInstantiateMethodTemplate(
        CajetaClass* cls, const std::string& methodName,
        const std::vector<ParameterEntry>& parameters,
        const std::vector<CajetaTypePtr>& explicitArgs = {}) {
        for (auto& m : cls->getMethodList()) {
            if (!m) continue;
            if (m->getName() != methodName) continue;
            if (!m->isMethodTemplate()) continue;
            auto formals = m->getParameterList();
            if (formals.size() != parameters.size()) continue;

            const auto& tparams = m->getMethodTypeParameters();

            if (!explicitArgs.empty()) {
                if (explicitArgs.size() != tparams.size()) continue;
                return m->instantiateMethodTemplate(explicitArgs);
            }

            std::set<std::string> tparamNames;
            for (auto& tp : tparams) tparamNames.insert(tp.name);

            std::map<std::string, CajetaTypePtr> bindings;
            bool ok = true;
            for (size_t i = 0; i < formals.size(); ++i) {
                if (!unifyMethodTemplateFormal(
                        formals[i]->getType(), parameters[i].type,
                        tparamNames, bindings)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (bindings.size() < tparams.size()) {
                for (size_t i = 0; i < formals.size(); ++i) {
                    auto fc = std::dynamic_pointer_cast<CajetaClass>(
                        formals[i]->getType());
                    auto ac = std::dynamic_pointer_cast<CajetaClass>(
                        parameters[i].type);
                    if (!fc || !ac) continue;
                    if (!fc->getTypeArguments().empty()) continue;
                    if (ac->getTypeArguments().empty()) continue;
                    const auto& aArgs = ac->getTypeArguments();
                    if (aArgs.empty()) continue;
                    for (auto& tp : tparams) {
                        if (bindings.count(tp.name)) continue;
                        bindings[tp.name] = aArgs.back();
                        break;
                    }
                    if (bindings.size() >= tparams.size()) break;
                }
            }
            std::vector<CajetaTypePtr> args;
            for (auto& tp : tparams) {
                auto it = bindings.find(tp.name);
                if (it == bindings.end()) { args.clear(); break; }
                args.push_back(it->second);
            }
            if (args.empty()) continue;
            return m->instantiateMethodTemplate(args);
        }
        return nullptr;
    }

    // Registers a fresh method-template instantiation on `host` and emits its
    // prototype and body, restoring the builder, current-method and scope-stack
    // state the nested codegen mutates. Idempotent on re-entry.
    static void bringMethodTemplateInstantiationToLife(
            CajetaClass* host, MethodPtr inst,
            CajetaModulePtr activeModule = nullptr) {
        xref::SyntheticSourceScope xrefMask;

        if (host->getMethods().find(inst->getMapKey()) != host->getMethods().end()) {
            return;
        }
        host->addMethod(inst);
        auto hostMod = inst->getEmitModule();
        llvm::IRBuilder<>* savedBuilder = hostMod ? hostMod->getBuilder() : nullptr;
        MethodPtr savedCurrent = hostMod ? hostMod->getCurrentMethod() : nullptr;
        // Save the insert point of the ACTIVE codegen builder, not the host's: a
        // classpath template's emit module is not the module being generated, so
        // its builder's block may already be freed.
        CajetaModulePtr ipMod = activeModule;
        if (!ipMod) ipMod = CajetaModule::getCurrentCodegenModule();
        if (!ipMod) ipMod = hostMod;
        llvm::IRBuilder<>* ipBuilder = ipMod ? ipMod->getBuilder() : nullptr;
        llvm::BasicBlock* ipInsertBB = ipBuilder
            ? ipBuilder->GetInsertBlock() : nullptr;
        // Scope-stack barrier: the inner body's resolveTypes must not reach the
        // caller's locals through the parent chain and pin a wrong type.
        list<ScopePtr> savedScopes;
        if (hostMod) {
            savedScopes = hostMod->getScopeStack().save();
        }
        inst->generatePrototype();
        inst->generateCode();
        if (hostMod) {
            hostMod->getScopeStack().restore(savedScopes);
            hostMod->setBuilder(savedBuilder);
            hostMod->setCurrentMethod(savedCurrent);
        }
        if (ipBuilder && ipInsertBB) {
            ipBuilder->SetInsertPoint(ipInsertBB);
        }
    }

    // Registers an instantiation WITHOUT emitting it: the codegen fixed-point then
    // emits it with full cursor context, which a pre-loop generateCode lacks.
    void CajetaClass::ensureMethodInstantiationAlive(MethodPtr inst) {
        if (methods.find(inst->getMapKey()) != methods.end()) return;
        addMethod(std::move(inst));
    }

    // Overload resolution plus the xref call-edge recording that every callee
    // resolution funnels through. An unfilled placeholder receiver is materialized
    // first, so resolution runs against real members rather than a phantom.
    MethodPtr CajetaClass::resolveMethod(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs,
            CajetaModulePtr activeModule) {
        if (isPlaceholder() && getQName() && CajetaModule::userMaterializeHook) {
            CajetaModule::userMaterializeHook(getQName()->toCanonical());
        }
        MethodPtr resolved = resolveMethodImpl(methodName, parameters, isConstructor,
                                               floatingParams, explicitMethodTypeArgs,
                                               activeModule);
        noteResolvedCallXref(resolved, isConstructor, activeModule);
        return resolved;
    }

    // Records one resolved call edge, mapping a monomorphized instantiation back to
    // the template member the developer wrote. Factored out so the lint path derives
    // the key, the caller and the virtual bit identically.
    void CajetaClass::noteResolvedCallXref(const MethodPtr& resolved,
            bool isConstructor, CajetaModulePtr activeModule) {
        if (!xref::captureEnabled() || !resolved) return;

        CajetaClassPtr owner = resolved->getParent();
        if (!owner || !owner->getQName()) return;

        std::string calleeKey = resolved->toCanonical(/*labeled=*/false);
        const std::string ownerCanon = owner->getQName()->toCanonical();

        auto lt = ownerCanon.find('<');
        if (lt != std::string::npos) {
            auto pl = resolved->getParameterList();
            size_t off = (!pl.empty() && pl.front()
                          && pl.front()->getName() == "this") ? 1 : 0;
            calleeKey = xref::templateKeyFor(ownerCanon.substr(0, lt),
                                             resolved->getName(),
                                             (int) (pl.size() - off));
        } else if (Method* origin = resolved->getTemplateOrigin()) {
            calleeKey = origin->toCanonical(/*labeled=*/false);
        }

        std::string callerKey;
        if (activeModule) {
            if (auto caller = activeModule->getCurrentMethod()) {
                callerKey = caller->toCanonical(/*labeled=*/false);
            }
        }

        const bool isVirtual = !isConstructor
                            && !resolved->getModifiers().count(Modifier::STATIC);

        xref::noteResolvedCall(calleeKey, callerKey, isVirtual);
    }

    // Resolution proper: exact key, enum-ordinal decay, closest labeled match,
    // subtype-aware positional match, the parent chain, then method-template
    // instantiation. Null when nothing matches.
    MethodPtr CajetaClass::resolveMethodImpl(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs,
            CajetaModulePtr activeModule) {
        if (!isConstructor && !explicitMethodTypeArgs.empty()) {
            if (MethodPtr inst = tryInstantiateMethodTemplate(
                    this, methodName, parameters, explicitMethodTypeArgs)) {
                bringMethodTemplateInstantiationToLife(this, inst, activeModule);
                return inst;
            }
            for (auto& parent : superClasses) {
                if (MethodPtr inst = tryInstantiateMethodTemplate(
                        parent.get(), methodName, parameters, explicitMethodTypeArgs)) {
                    bringMethodTemplateInstantiationToLife(parent.get(), inst, activeModule);
                    return inst;
                }
            }
            return nullptr;
        }
        string generic = Method::buildGeneric(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);
        string canonical = Method::buildCanonical(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);

        if (const char* dbg = std::getenv("CAJETA_DBG_RESOLVE");
                dbg && methodName == dbg) {
            fprintf(stderr, "[dbg-res] resolve '%s' on %s floating=%d\n",
                    methodName.c_str(), toCanonical().c_str(), (int) floatingParams);
            fprintf(stderr, "[dbg-res]   call generic:   %s\n", generic.c_str());
            fprintf(stderr, "[dbg-res]   call canonical: %s\n", canonical.c_str());
            for (auto& p : parameters) {
                fprintf(stderr, "[dbg-res]   arg label='%s' type=%s\n",
                        p.label.c_str(),
                        p.type ? p.type->toCanonical().c_str() : "<null>");
            }
            auto* gm = isConstructor
                ? (floatingParams ? &labeledConstructorMap : &unlabeledConstructorMap)
                : (floatingParams ? &labeledMethodMap : &unlabeledMethodMap);
            for (auto& [g, cm] : *gm) {
                if (g.find(methodName) == string::npos) continue;
                fprintf(stderr, "[dbg-res]   indexed generic (eq=%d): %s\n",
                        (int) (g == generic), g.c_str());
                for (auto& [c, m] : cm)
                    fprintf(stderr, "[dbg-res]     canonical: %s\n", c.c_str());
            }
        }

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
            // Before the closest-match scan: that scan ties at 0 for positional
            // calls, so bucket order would otherwise pick an arbitrary overload.
            bool anyEnum = false;
            vector<ParameterEntry> decayed = parameters;
            for (auto& p : decayed) {
                if (p.type && (p.type->getTypeFlags() & ENUM_FLAG)) {
                    auto i32It = CajetaType::getCanonicalMap().find("int32");
                    if (i32It != CajetaType::getCanonicalMap().end()) {
                        p.type = i32It->second;
                        anyEnum = true;
                    }
                }
            }
            if (anyEnum) {
                string decayedCanonical = Method::buildCanonical(
                    static_pointer_cast<CajetaClass>(shared_from_this()),
                    methodName, decayed, floatingParams);
                auto dit = canonicalMap.find(decayedCanonical);
                if (dit != canonicalMap.end()) {
                    return dit->second;
                }
            }
            MethodPtr m = getClosestMethod(methodName, parameters, canonicalMap);
            if (const char* dbg = std::getenv("CAJETA_DBG_RESOLVE");
                    dbg && methodName == dbg) {
                fprintf(stderr, "[dbg-res]   canonical miss; closest=%s\n",
                        m ? "FOUND" : "null");
            }
            if (m) return m;
        }

        // Operators stay strict: `s == null` must keep its direct ptr-icmp rather
        // than resolving to Object::operator==, whose body derefs the receiver.
        bool relaxNull = isConstructor
            || methodName.rfind("operator", 0) != 0;
        if (MethodPtr m = findSubtypeMatch(*genericMap, methodName, parameters,
                /*relaxNullToRef=*/relaxNull)) {
            return m;
        }

        if (!isConstructor) {
            for (auto& parent : getSuperClasses()) {
                // ...Impl, not the wrapper: the wrapper records an xref edge, and a
                // parent walk would record one per level of the chain.
                MethodPtr m = parent->resolveMethodImpl(methodName, parameters,
                    isConstructor, floatingParams, {}, activeModule);
                if (m) return m;
            }
        }

        if (!isConstructor) {
            if (MethodPtr inst = tryInstantiateMethodTemplate(
                    this, methodName, parameters)) {
                bringMethodTemplateInstantiationToLife(this, inst, activeModule);
                return inst;
            }
            for (auto& parent : superClasses) {
                if (MethodPtr inst = tryInstantiateMethodTemplate(
                        parent.get(), methodName, parameters)) {
                    bringMethodTemplateInstantiationToLife(parent.get(), inst, activeModule);
                    return inst;
                }
            }
        }
        return nullptr;
    }

    // Reorders a mixed positional-prefix plus named-suffix call into formal order
    // and strips the labels so the positional path resolves it. False for an
    // all-positional or all-labeled call; throws LANG-NAMEDARG on a bad mix.
    bool CajetaClass::normalizePartialLabeledCall(const string& methodName,
            bool isConstructor, vector<ParameterEntry>& parameters) {
        size_t n = parameters.size();
        size_t firstLabeled = n, labeledCount = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!parameters[i].label.empty()) {
                if (firstLabeled == n) firstLabeled = i;
                labeledCount++;
            }
        }
        if (labeledCount == 0) return false;
        if (labeledCount == n) return false;

        size_t P = firstLabeled;
        for (size_t i = P; i < n; ++i) {
            if (parameters[i].label.empty()) {
                throw Exception("named arguments must form a trailing group: "
                    "positional argument follows a named argument", "LANG-NAMEDARG");
            }
        }
        // Argument labels carry the grammar's trailing ':' and formal parameter
        // names do not, so normalize before matching.
        auto stripColon = [](const string& s) {
            return (!s.empty() && s.back() == ':') ? s.substr(0, s.size() - 1) : s;
        };
        set<string> namedLabels;
        for (size_t i = P; i < n; ++i) {
            if (!namedLabels.insert(stripColon(parameters[i].label)).second) {
                throw Exception("duplicate named argument '" +
                    stripColon(parameters[i].label) + "'", "LANG-NAMEDARG");
            }
        }

        bool found = false;
        map<string, size_t> labelToFormalIndex;
        auto scan = [&](map<string, map<string, MethodPtr>>& mp) {
            for (auto& bucket : mp) {
                for (auto& entry : bucket.second) {
                    MethodPtr m = entry.second;
                    if (!m || m->getName() != methodName) continue;
                    auto formals = m->getParameterList();
                    size_t off = (!formals.empty() && formals.front() &&
                                  formals.front()->getName() == "this") ? 1 : 0;
                    if (formals.size() - off != n) continue;
                    set<string> suffixNames;
                    for (size_t i = P; i < n; ++i)
                        suffixNames.insert(formals[off + i]->getName());
                    if (suffixNames != namedLabels) continue;
                    map<string, size_t> perm;
                    for (size_t i = P; i < n; ++i)
                        perm[formals[off + i]->getName()] = i;
                    if (!found) { labelToFormalIndex = perm; found = true; }
                    else if (perm != labelToFormalIndex) {
                        throw Exception("ambiguous named call: overloads of '" +
                            methodName + "' order the named parameters differently",
                            "LANG-NAMEDARG");
                    }
                }
            }
        };
        scan(isConstructor ? unlabeledConstructorMap : unlabeledMethodMap);
        if (!isConstructor) {
            for (auto& parent : superClasses) scan(parent->unlabeledMethodMap);
        }
        if (!found) {
            throw Exception("no '" + methodName + "' matches the named arguments "
                "provided — check the parameter names", "LANG-NAMEDARG");
        }

        vector<ParameterEntry> reordered;
        reordered.reserve(n);
        for (size_t i = 0; i < P; ++i) reordered.push_back(parameters[i]);
        map<size_t, ParameterEntry> byFormalIndex;
        for (size_t i = P; i < n; ++i)
            byFormalIndex.insert({labelToFormalIndex[stripColon(parameters[i].label)],
                                  parameters[i]});
        for (auto& kv : byFormalIndex) {
            ParameterEntry e = kv.second;
            e.label.clear();
            reordered.push_back(e);
        }
        parameters = reordered;
        return true;
    }

    // The target llvm::Function of a closure argument IFF it is a directly-written
    // non-capturing lambda: a constant `{ ptr fn, ptr null, ptr null }` global,
    // traced through a single-store slot. Null for any runtime closure.
    static llvm::Function* extractClosureTargetFn(llvm::Value* closureArg,
                                                  llvm::Constant** outRecord = nullptr,
                                                  int depth = 0) {
        if (!closureArg || depth > 8) return nullptr;
        llvm::Value* v = closureArg->stripPointerCasts();

        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(v)) {
            auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                load->getPointerOperand()->stripPointerCasts());
            if (!slot) return nullptr;
            llvm::StoreInst* onlyStore = nullptr;
            for (auto* u : slot->users()) {
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                    if (st->getValueOperand() == slot) return nullptr;
                    if (onlyStore) return nullptr;
                    onlyStore = st;
                } else if (!llvm::isa<llvm::LoadInst>(u)) {
                    return nullptr;
                }
            }
            if (!onlyStore) return nullptr;
            return extractClosureTargetFn(onlyStore->getValueOperand(), outRecord, depth + 1);
        }

        auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(v);
        if (!gv || !gv->isConstant() || !gv->hasInitializer()) return nullptr;
        auto* cs = llvm::dyn_cast<llvm::ConstantStruct>(gv->getInitializer());
        if (!cs || cs->getNumOperands() < 2) return nullptr;
        if (!cs->getOperand(1)->isNullValue()) return nullptr;
        auto* fn = llvm::dyn_cast<llvm::Function>(cs->getOperand(0)->stripPointerCasts());
        if (fn && outRecord) *outRecord = gv;
        return fn;
    }

    // Public wrapper over extractClosureTargetFn.
    llvm::Function* CajetaClass::extractClosureTarget(llvm::Value* closureArg,
                                                      llvm::Constant** outRecord) {
        return extractClosureTargetFn(closureArg, outRecord, 0);
    }

    // The closest real member name to `typo` across this class and its ancestors,
    // bounded at 2 edits and at a third of the longer name. Returns "" when nothing
    // is close enough, since a wrong guess is worse than no guess.
    string CajetaClass::suggestMemberName(const string& typo) {
        auto editDistance = [](const string& a, const string& b) -> size_t {
            vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
            for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
            for (size_t i = 1; i <= a.size(); ++i) {
                cur[0] = i;
                for (size_t j = 1; j <= b.size(); ++j) {
                    size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
                    cur[j] = std::min({sub, prev[j] + 1, cur[j - 1] + 1});
                }
                prev = cur;
            }
            return prev[b.size()];
        };

        string best;
        size_t bestDist = SIZE_MAX;
        std::function<void(CajetaClass*)> consider = [&](CajetaClass* cls) {
            if (!cls) return;
            auto weigh = [&](const string& name) {
                if (name.rfind("__", 0) == 0) return;
                if (name == typo) return;
                size_t d = editDistance(typo, name);
                size_t longer = std::max(typo.size(), name.size());
                if (d > 2 || d * 3 > longer) return;
                if (d < bestDist) { bestDist = d; best = name; }
            };
            for (auto& mEntry : cls->getMethods()) {
                if (mEntry.second) weigh(mEntry.second->getName());
            }
            for (auto& pEntry : cls->getProperties()) weigh(pEntry.first);
            for (auto& sup : cls->getSuperClasses()) consider(sup.get());
        };
        consider(this);
        return best;
    }

    // The diagnostic for an unresolved member: "no member" when nothing of that name
    // exists on the receiver or an ancestor, otherwise "no overload" listing the
    // candidate signatures.
    Exception CajetaClass::memberNotFoundException(const string& methodName,
            const vector<ParameterEntry>& parameters, int line, int column) {
        vector<string> candidates;
        std::function<void(CajetaClass*)> collect = [&](CajetaClass* cls) {
            if (!cls) return;
            for (auto& mEntry : cls->getMethods()) {
                auto& m = mEntry.second;
                if (!m || m->getName() != methodName) continue;
                string sig = methodName + "(";
                auto pl = m->getParameterList();
                bool isStatic = m->getModifiers().find(STATIC)
                    != m->getModifiers().end();
                size_t first = isStatic ? 0 : 1;
                for (size_t i = first; i < pl.size(); ++i) {
                    if (i > first) sig += ", ";
                    auto pt = pl[i] ? pl[i]->getType() : nullptr;
                    sig += (pt && pt->getQName())
                        ? pt->getQName()->toCanonical() : "?";
                }
                sig += ")";
                candidates.push_back(sig);
            }
            for (auto& sup : cls->getSuperClasses()) collect(sup.get());
        };
        collect(this);

        string recv = getQName() ? getQName()->toCanonical() : "<unknown>";
        if (candidates.empty()) {
            string msg = "no member '" + methodName + "' on '" + recv + "'";
            {
                auto origin = isInstantiation() ? getTemplateOrigin() : nullptr;
                const auto& targs = getTypeArguments();
                if (origin && origin->getQName()
                        && origin->getQName()->toCanonical()
                            == "cajeta.nucleo.frame.Table"
                        && targs.size() == 1 && targs[0]
                        && targs[0]->isWildcard()
                        && !targs[0]->wildcardBound()) {
                    return locatedException(line, column,
                        msg + " — schema not statically known here; narrow "
                        "with `.as<R>()` or use `col(\"...\")`",
                        "CAJETA_ERROR_MEMBER_NOT_FOUND");
                }
            }
            string hint = suggestMemberName(methodName);
            if (!hint.empty()) msg += " — did you mean '" + hint + "'?";
            return locatedException(line, column, msg,
                "CAJETA_ERROR_MEMBER_NOT_FOUND");
        }
        string msg = "no overload of '" + methodName + "' on '" + recv
            + "' accepts " + std::to_string(parameters.size())
            + " argument(s). Candidates:";
        for (auto& c : candidates) msg += "\n    " + c;
        return locatedException(line, column, msg,
            "CAJETA_ERROR_NO_MATCHING_OVERLOAD");
    }

    // Resolves `methodName` against `parameters` and emits the call: visibility
    // check, sret slot, argument coercion and upcasts, closure specialization, the
    // hidden transfer word, then direct, class-vtable or interface dispatch.
    llvm::Value* CajetaClass::invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisValue,
                                            CajetaModulePtr callerModule, bool forceDirectCall,
                                            const vector<CajetaTypePtr>& explicitMethodTypeArgs,
                                            llvm::Value* sretTarget,
                                            llvm::Value* transferWord,
                                            bool errorIfUnresolved,
                                            int callLine, int callColumn) {
        normalizePartialLabeledCall(methodName, isConstructor, parameters);

        bool floatingParams = true;
        for (auto &param : parameters) {
            if (param.label.empty()) {
                floatingParams &= false;
            }
        }

        if (floatingParams) {
            sort(parameters.begin(), parameters.end(), [](const ParameterEntry& a, const ParameterEntry& b) -> bool { return a.label < b.label; });
        }

        MethodPtr method = resolveMethod(methodName, parameters, isConstructor,
            floatingParams, explicitMethodTypeArgs, callerModule);
        if (!method) {
            if (isConstructor) {
                string args;
                for (auto& p : parameters) {
                    if (!args.empty()) args += ", ";
                    args += p.type ? p.type->toCanonical() : string("<?>");
                }
                throw Exception(
                    "no matching constructor `" + methodName + "(" + args
                        + ")` on `" + toCanonical() + "`. Without a matching "
                        "constructor the instance would be left zero-initialized "
                        "(vtable installed, no ctor run) and fail at first use. "
                        "Fix: match an existing constructor's signature, or add "
                        "the overload.",
                    "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR");
            }
            if (errorIfUnresolved) {
                throw memberNotFoundException(methodName, parameters,
                    callLine, callColumn);
            }
            return nullptr;
        }
        if (!isConstructor) {
            auto& sstack = callerModule
                ? callerModule->getStructureStack()
                : module->getStructureStack();
            CajetaClassPtr callerCls = sstack.empty()
                ? nullptr : sstack.back();
            CajetaClassPtr methodOwner = method->getParent();
            if (callerCls && methodOwner) {
                auto& mods = method->getModifiers();
                bool isPriv = mods.find(PRIVATE) != mods.end();
                bool isProt = mods.find(PROTECTED) != mods.end();
                bool isPkg  = mods.find(PACKAGE) != mods.end();
                bool sameClass = callerCls.get() == methodOwner.get();
                bool samePkg = false;
                if (callerCls->getQName() && methodOwner->getQName()) {
                    samePkg = callerCls->getQName()->getPackageName()
                        == methodOwner->getQName()->getPackageName();
                }
                std::function<bool(CajetaClass*)> isDescendant =
                    [&](CajetaClass* c) -> bool {
                        if (!c) return false;
                        if (c == methodOwner.get()) return true;
                        for (auto& sup : c->getSuperClasses()) {
                            if (isDescendant(sup.get())) return true;
                        }
                        return false;
                    };
                bool isSubclass = !sameClass && isDescendant(callerCls.get());

                bool allowed = true;
                if (isPriv) {
                    allowed = sameClass;
                } else if (isProt) {
                    allowed = sameClass || isSubclass || samePkg;
                } else if (isPkg) {
                    allowed = samePkg;
                }
                if (!allowed) {
                    std::string accessName =
                        isPriv ? "private" :
                        isProt ? "protected" :
                        isPkg  ? "package" : "";
                    throw Exception(
                        std::string("method `") + methodOwner->getQName()->toCanonical()
                        + "." + methodName + "` is " + accessName
                        + " and not accessible from `"
                        + callerCls->getQName()->toCanonical() + "`",
                        "CAJETA_ERROR_METHOD_NOT_ACCESSIBLE");
                }
            }
        }
        bool isStatic = method->getModifiers().find(STATIC) != method->getModifiers().end();
        vector<llvm::Value*> methodArgs;
        if (thisValue && !isStatic) {
            methodArgs.push_back(thisValue);
        }
        CajetaModulePtr emitMod = callerModule ? callerModule : getEmitModule();
        // sret ABI: a value-returning method takes the result slot as hidden
        // argument 0, BEFORE `this`.
        bool usesSret = method->returnsStackValue();
        int sretOffset = usesSret ? 1 : 0;
        llvm::Value* sretSlot = sretTarget;
        if (usesSret) {
            if (!sretSlot) {
                llvm::Type* structTy = method->getReturnType()
                    ? method->getReturnType()->getLlvmType() : nullptr;
                if (structTy) {
                    llvm::Function* parentFn =
                        emitMod->getBuilder()->GetInsertBlock()->getParent();
                    llvm::IRBuilder<> entryBuilder(&parentFn->getEntryBlock(),
                        parentFn->getEntryBlock().begin());
                    sretSlot = entryBuilder.CreateAlloca(structTy);
                }
            }
            if (sretSlot) {
                methodArgs.insert(methodArgs.begin(), sretSlot);
            }
        }
        auto* coerceBuilder = emitMod->getBuilder();
        llvm::FunctionType* mft = method->getLlvmFunctionType();
        int thisOffset = (thisValue && !isStatic) ? 1 : 0;
        auto formalParams = method->getParameterList();
        for (int i = 0; i < (int) parameters.size(); i++) {
            llvm::Value* v = parameters[i].value;
            int formalIdx = i + thisOffset;
            if (formalIdx >= 0 && formalIdx < (int) formalParams.size()
                    && v && parameters[i].type) {
                auto srcClass = std::dynamic_pointer_cast<CajetaClass>(
                    parameters[i].type);
                auto dstClass = std::dynamic_pointer_cast<CajetaClass>(
                    formalParams[formalIdx]->getType());
                if (srcClass && dstClass
                        && srcClass.get() != dstClass.get()
                        && !srcClass->isInterface()
                        && !dstClass->isInterface()) {
                    v = CajetaClass::adjustForUpcast(
                        emitMod, v, srcClass, dstClass);
                }
                if (srcClass && dstClass
                        && !srcClass->isInterface()
                        && dstClass->isInterface()) {
                    auto& lctx = *emitMod->getLlvmContext();
                    llvm::Type* bodyTy = dstClass->getLlvmType();
                    llvm::Type* ptrTy = llvm::PointerType::get(lctx, 0);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(lctx);
                    llvm::Value* bodyAlloca = coerceBuilder->CreateAlloca(bodyTy);
                    llvm::Value* dataSlot = coerceBuilder->CreateStructGEP(
                        bodyTy, bodyAlloca, 0, "iface_arg_data");
                    llvm::Value* vtSlot = coerceBuilder->CreateStructGEP(
                        bodyTy, bodyAlloca, 1, "iface_arg_vtable");
                    llvm::Value* kindSlot = coerceBuilder->CreateStructGEP(
                        bodyTy, bodyAlloca, 2, "iface_arg_kind");
                    coerceBuilder->CreateStore(v, dataSlot);
                    std::string ifaceCanonical =
                        dstClass->getQName()->toCanonical();
                    llvm::Constant* vtableRef = nullptr;
                    if (auto gv = srcClass->getInterfaceVTable(ifaceCanonical)) {
                        vtableRef = CajetaModule::ensureGlobalInModule(
                            emitMod->getLlvmModule(), gv);
                    }
                    if (!vtableRef) {
                        vtableRef = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                    }
                    coerceBuilder->CreateStore(vtableRef, vtSlot);
                    coerceBuilder->CreateStore(
                        llvm::ConstantInt::get(i64Ty,
                            (uint64_t) IFACE_KIND_BORROWED_CLASS),
                        kindSlot);
                    v = bodyAlloca;
                }
                if (dstClass && dstClass->isInterface() && !srcClass
                        && v && llvm::isa<llvm::ConstantPointerNull>(v)) {
                    llvm::Type* bodyTy = dstClass->getLlvmType();
                    if (bodyTy && bodyTy->isStructTy()) {
                        auto& lctx = *emitMod->getLlvmContext();
                        llvm::Value* bodyAlloca =
                            coerceBuilder->CreateAlloca(bodyTy, nullptr,
                                                        "iface_arg_null");
                        const llvm::DataLayout& dl =
                            emitMod->getLlvmModule()->getDataLayout();
                        coerceBuilder->CreateMemSet(bodyAlloca,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt8Ty(lctx), 0),
                            dl.getTypeAllocSize(bodyTy),
                            llvm::MaybeAlign(8));
                        v = bodyAlloca;
                    }
                }
            }
            if (mft && (int) mft->getNumParams() > i + thisOffset + sretOffset) {
                llvm::Type* expected = mft->getParamType(i + thisOffset + sretOffset);
                if (v && v->getType() != expected) {
                    if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                        v = coerceBuilder->CreateIntCast(v, expected, /*isSigned=*/true);
                    } else if (expected->isFloatingPointTy() && v->getType()->isFloatingPointTy()) {
                        v = coerceBuilder->CreateFPCast(v, expected);
                    } else if (expected->isPointerTy()
                               && (v->getType()->isStructTy()
                                   || v->getType()->isArrayTy()
                                   || v->getType()->isVectorTy())) {
                        // The aggregate ABI passes a value type BY POINTER; spill
                        // this rvalue temporary and pass the slot's address.
                        llvm::Value* spill = coerceBuilder->CreateAlloca(v->getType());
                        coerceBuilder->CreateStore(v, spill);
                        v = spill;
                    } else if ((expected->isStructTy() || expected->isVectorTy()
                                || expected->isArrayTy())
                               && v->getType()->isPointerTy()) {
                        // Mirror arm: the formal takes the aggregate BY VALUE while
                        // we hold its address.
                        v = coerceBuilder->CreateLoad(expected, v);
                    }
                }
            }
            methodArgs.push_back(v);
        }

        auto* builder = emitMod->getBuilder();
        auto& llvmCtx = *emitMod->getLlvmContext();

        if (Method* tmpl = method->getTemplateOrigin()) {
            const auto plist = method->getParameterList();
            for (size_t k = 0; k < plist.size(); ++k) {
                if (!plist[k]) continue;
                auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(
                    plist[k]->getType());
                if (!fnType) continue;
                size_t argPos = (size_t) sretOffset + k;
                if (argPos >= methodArgs.size()) continue;
                llvm::Constant* record = nullptr;
                llvm::Function* lambdaFn = extractClosureTargetFn(methodArgs[argPos], &record);
                if (!lambdaFn) continue;
                MethodPtr spec = tmpl->instantiateSpecializedClosure(
                    method->getMethodTypeArguments(), plist[k]->getName(),
                    lambdaFn, fnType, record);
                if (!spec) continue;
                if (emitMod) spec->setEmitModule(emitMod);
                CajetaClass* host = spec->getParent() ? spec->getParent().get() : this;
                bringMethodTemplateInstantiationToLife(host, spec, emitMod);
                methodArgs.erase(methodArgs.begin() + argPos);
                method = spec;
                break;
            }
        }

        if (method->needsTransferWord()) {
            llvm::Value* twv = transferWord;
            if (!twv) {
                twv = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*emitMod->getLlvmContext()), 0);
            }
            methodArgs.push_back(twv);
        }

        llvm::Module* currentLm = emitMod->getLlvmModule();
        if (llvm::IRBuilder<>* ib = emitMod->getBuilder()) {
            if (llvm::BasicBlock* ibb = ib->GetInsertBlock()) {
                if (llvm::Function* ibf = ibb->getParent()) {
                    currentLm = ibf->getParent();
                }
            }
        }
        const std::string canonical = method->getLlvmSymbolName();
        llvm::Function* targetFn = currentLm->getFunction(canonical);
        if (!targetFn) {
            llvm::FunctionCallee fc = currentLm->getOrInsertFunction(
                canonical, method->getLlvmFunctionType());
            targetFn = llvm::dyn_cast<llvm::Function>(fc.getCallee());
        }
        llvm::Value* callee = targetFn
            ? static_cast<llvm::Value*>(targetFn)
            : CajetaModule::ensureFunctionVisible(
                builder, method->getLlvmFunction(),
                method->getLlvmFunctionType());
        bool isView = dynamic_cast<CajetaView*>(this) != nullptr;
        bool isMethodTemplateInst = method->isMethodTemplateInstantiation();
        // A @Native forwarder on a FINAL class dispatches directly: its receiver may
        // be a null handle and the vtable load would fault. The `final` gate is
        // required, since a non-final base's @Native method can be overridden.
        bool isNativeForwarder = method->findAnnotation("Native") != nullptr;
        bool isFinalClass = this->getModifiers().find(FINAL) != this->getModifiers().end();
        bool useVtable = thisValue && !isStatic && !isConstructor && !isView
            && !forceDirectCall && !isMethodTemplateInst
            && !(isNativeForwarder && isFinalClass)
            // @ValueType PODs have no slot-0 vtable, so slot 0 is the first field
            // and a vtable load there returns garbage.
            && hasVtablePointerAtSlotZero();
        bool isInterfaceRecv = this->isInterface();
        bool methodOnClassAncestor = isInterfaceRecv && method->getParent()
            && !method->getParent()->isInterface();
        if (useVtable && isInterfaceRecv && methodOnClassAncestor) {
            llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
            llvm::Type* bodyTy = this->getLlvmType();
            llvm::Value* dataSlot = builder->CreateStructGEP(
                bodyTy, thisValue, 0, "iface_data_slot");
            llvm::Value* dataPtr = builder->CreateLoad(
                ptrTy, dataSlot, "iface_data");
            llvm::Function* lookupFn = emitMod->getRuntimeFunction(
                "__cajeta_vtable_lookup");
            if (lookupFn) {
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Value* vtable = builder->CreateLoad(
                    ptrTy, dataPtr, "iface_data_vtable");
                int64_t hash = signatureHash(
                    method->toCanonical(/*labeled=*/false));
                llvm::Value* fnPtr = builder->CreateCall(lookupFn,
                    {vtable, llvm::ConstantInt::get(i64Ty,
                        llvm::APInt(64, (uint64_t) hash, false))},
                    "iface_class_method_fn");
                callee = fnPtr;
            }
            // `this` sits at methodArgs[sretOffset]; overwriting index 0 would
            // clobber the hidden sret slot.
            if ((int) methodArgs.size() > sretOffset) {
                methodArgs[sretOffset] = dataPtr;
            }
        } else if (useVtable && isInterfaceRecv) {
            llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
            llvm::Type* bodyTy = this->getLlvmType();

            llvm::Value* dataSlot = builder->CreateStructGEP(
                bodyTy, thisValue, 0, "iface_data_slot");
            llvm::Value* vtableSlot = builder->CreateStructGEP(
                bodyTy, thisValue, 1, "iface_vtable_slot");
            llvm::Value* dataPtr = builder->CreateLoad(
                ptrTy, dataSlot, "iface_data");
            llvm::Value* vtablePtr = builder->CreateLoad(
                ptrTy, vtableSlot, "iface_vtable");

            int methodIdx = -1;
            int idx = 0;
            for (auto& im : this->getFlattenedInterfaceMethods()) {
                if (im->getName() == method->getName()) {
                    methodIdx = idx;
                    break;
                }
                ++idx;
            }

            if (methodIdx >= 0) {
                // +1 to skip the drop-fn slot at vtable[0].
                llvm::Value* methodSlot = builder->CreateInBoundsGEP(
                    ptrTy, vtablePtr,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx),
                        (uint64_t) (methodIdx + 1)),
                    "iface_method_slot");
                callee = builder->CreateLoad(ptrTy, methodSlot, "iface_method_fn");
            } else {
                std::ostringstream w;
                w << "warning: [iface-dispatch-index-miss] "
                  << this->getQName()->toCanonical() << "::"
                  << method->getName()
                  << " not found in the flattened interface method list"
                  << " — interface dispatch will crash\n";
                logLine("warn", w.str());
            }

            if ((int) methodArgs.size() > sretOffset) {
                methodArgs[sretOffset] = dataPtr;
            }
        } else if (useVtable) {
            llvm::Function* lookupFn = emitMod->getRuntimeFunction("__cajeta_vtable_lookup");
            if (lookupFn) {
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Value* vtable = builder->CreateLoad(ptrTy, thisValue, "vtable");
                int64_t hash;
                // Every Class<T> shares ONE runtime vtable (the Class<?> one), so a
                // concrete receiver must dispatch on the origin-relative alias hash.
                auto tOrigin = this->getTemplateOrigin();
                bool sharesWildcardVtable = tOrigin
                    && tOrigin->toCanonical() == "cajeta.reflect.Class";
                if ((this->isWildcardInstantiation() || sharesWildcardVtable)
                        && tOrigin) {
                    string aliasCanon = Method::buildTemplateOriginCanonical(
                        static_pointer_cast<CajetaClass>(shared_from_this()),
                        method->getName(),
                        method->getParameterList(),
                        /*labeled=*/false);
                    hash = signatureHash(aliasCanon);
                } else {
                    hash = signatureHash(method->toCanonical(/*labeled=*/false));
                }
                llvm::Value* fnPtr = builder->CreateCall(lookupFn,
                    {vtable,
                     llvm::ConstantInt::get(i64Ty, llvm::APInt(64, (uint64_t) hash, false))},
                    "vmethod_fn");
                callee = fnPtr;
            }
        }

        // Shift `this` to the declaring ancestor's sub-object: the parent's
        // pre-compiled IR GEPs with the parent's own slot indices.
        if (thisValue && !isStatic && !isInterfaceRecv && method->getParent()) {
            const CajetaClass* declaring = method->getParent().get();
            if (declaring && declaring != this) {
                uint64_t off = this->getSubObjectByteOffset(declaring);
                if (off != 0 && (int) methodArgs.size() > sretOffset) {
                    llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
                    methodArgs[sretOffset] = builder->CreateInBoundsGEP(
                        i8Ty, methodArgs[sretOffset],
                        llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(llvmCtx), off),
                        "subobj_this");
                }
            }
        }

        if (!isStatic && (int) methodArgs.size() > sretOffset
                && methodArgs[sretOffset]
                && methodArgs[sretOffset]->getType()->isPointerTy()) {
            auto formals = method->getParameterList();
            if (!formals.empty() && formals.front()
                    && formals.front()->getName() == "this"
                    && formals.front()->getType()) {
                llvm::Type* wantTy = formals.front()->getType()->getLlvmType();
                if (wantTy && !wantTy->isPointerTy()) {
                    methodArgs[sretOffset] = builder->CreateLoad(
                        wantTy, methodArgs[sretOffset], "enum.this");
                }
            }
        }

        llvm::CallInst* callInst = builder->CreateCall(method->getLlvmFunctionType(),
            callee, llvm::ArrayRef<llvm::Value*>(methodArgs));
        if (usesSret && sretSlot) {
            // The call site MUST carry StructRet on argument 0: aarch64 routes sret
            // through x8 while a plain ptr argument goes in x0, so without it the
            // callee reads `this` from the return slot.
            if (auto* rt = method->getReturnType().get()) {
                if (llvm::Type* sretStructTy = rt->getLlvmType()) {
                    callInst->addParamAttr(0, llvm::Attribute::get(
                        *emitMod->getLlvmContext(), llvm::Attribute::StructRet,
                        sretStructTy));
                }
            }
            return sretSlot;
        }
        return callInst;
    }

    /** Placeholder for class metadata emission; not implemented. */
    void CajetaClass::generateMetadata() {
    }
}