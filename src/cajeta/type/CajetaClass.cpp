//
// Created by James Klappenbach on 10/24/22.
//

#include "CajetaClass.h"
#include "CajetaFunctionType.h"
#include "CajetaView.h"
#include "StructureMetadata.h"
#include "../field/Field.h"
#include "../method/Method.h"
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
#include "CajetaArray.h"
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
    // Forward declaration — defined further down. Needed by
    // generateStaticInitializers, which lives above the definition.
    static llvm::Constant* foldStaticInitializer(
        AbstractSyntaxNodePtr init, llvm::Type* storedType);
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

    llvm::Type* CajetaClass::getLlvmType() {
        if (llvmType) return llvmType;
        if (placeholderFlag && module) {
            return llvm::PointerType::get(*module->getLlvmContext(), 0);
        }
        return llvmType;
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

    bool CajetaClass::isWildcardInstantiation() const {
        for (auto& a : typeArguments) {
            if (a && a->isWildcard()) return true;
        }
        return false;
    }

    bool CajetaClass::isAssignableToWildcard(
            CajetaClassPtr from, CajetaClassPtr wildcardInst) {
        if (!from || !wildcardInst) return false;
        if (!wildcardInst->isWildcardInstantiation()) return false;
        auto fromOrigin = from->getTemplateOrigin();
        auto destOrigin = wildcardInst->getTemplateOrigin();
        if (!fromOrigin || !destOrigin) return false;
        return fromOrigin.get() == destOrigin.get();
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
        // Two-layer naming: instantiations include the method-arg
        // suffix so two instantiations of a same-canonical template
        // (T-vars not in value params) coexist in the inner map.
        // resolveMethod looks up plain canonical for ordinary methods,
        // which is what getMapKey returns for non-instantiations.
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

    void CajetaClass::addMethod(MethodPtr method) {
        // Two-layer naming: instantiations of a same-canonical template
        // (T-vars not in value params) get distinct keys via the
        // method-arg suffix in getMapKey(). Ordinary methods key on
        // their plain canonical, so reachable via resolveMethod's normal
        // lookup paths. See MethodLevelTemplate.md § two-layer naming.
        methods[method->getMapKey()] = method;

        if (method->isConstructor()) {
            map<string, MethodPtr> canonical = unlabeledConstructorMap[method->toGeneric(false)];
            if (canonical.find(method->getMapKey(false)) != canonical.end()) {
                throw "Constructor already exists";
            }
            mapMethod(method, labeledConstructorMap, true);
            mapMethod(method, unlabeledConstructorMap, false);
        } else {
            if (method->isStatic()) {
                map<string, MethodPtr> canonical = unlabeledMethodMap[method->toGeneric(false)];
                if (canonical.find(method->getMapKey(false)) != canonical.end()) {
                    throw "A static method with this signature already exists.  Static methods can not be overridden.";
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

    void CajetaClass::addProperty(StructurePropertyPtr field) {
        properties[field->getName()] = field;
        propertyList.push_back(field);
    }

    uint64_t CajetaClass::getSubObjectByteOffset(const CajetaClass* ancestor) const {
        if (!ancestor || ancestor == this) return 0;
        auto it = subObjectSlotMap.find(ancestor);
        if (it == subObjectSlotMap.end() || it->second == 0) return 0;
        if (!llvmType || !llvm::isa<llvm::StructType>(llvmType)) return 0;
        auto* st = llvm::cast<llvm::StructType>(llvmType);
        if (!st->isSized()) return 0;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        return dl.getStructLayout(st)->getElementOffset((unsigned) it->second);
    }

    std::vector<CajetaClass::NonFirstSubObject>
    CajetaClass::getNonFirstSubObjects() {
        std::vector<NonFirstSubObject> result;
        if (!llvmType || !llvm::isa<llvm::StructType>(llvmType)) return result;
        auto* st = llvm::cast<llvm::StructType>(llvmType);
        if (!st->isSized()) return result;
        const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
        const auto* layout = dl.getStructLayout(st);
        // Walk the layout in the SAME order as embedSubObject in
        // generatePrototype. Emit (ancestor, slot, byteOffset) for every
        // sub-object that has its own vtable slot — i.e., not the
        // primary chain (self + first-parent + first-parent's first-
        // parent ...). The slot ALWAYS corresponds to a vptr because we
        // only push when ownVtable=true.
        //
        // We can't reuse subObjectSlotMap here because in diamond-shaped
        // hierarchies (any class implicitly extending Object via two
        // chains, for example) the same ancestor appears multiple times
        // and the map keeps only the last visit. Walking the layout
        // directly is the only way to enumerate each vptr-bearing
        // sub-object once.
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
                // MultiClassing Phase 3 v4: each class's slice in the
                // flattened layout ends with one ptr per transitive
                // non-self ancestor (vbase pointers). Advance `slot`
                // past them so subsequent sub-objects' slot indices
                // are correct.
                slot += (int) cls->getVbaseAncestors().size();
            };
        walk(std::static_pointer_cast<CajetaClass>(shared_from_this()),
            /*ownVtable=*/true);
        return result;
    }

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
        // Interface upcast uses the fat-pointer path (see
        // LocalVariableDeclaration § interface block).
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

    static std::string sanitizeSymbol(std::string s) {
        for (char& c : s) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                    || c == ',' || c == ' ' || c == '(' || c == ')') {
                c = '_';
            }
        }
        return s;
    }

    llvm::Function* CajetaClass::synthesizeOffsetThunk(
            CajetaClassPtr parent,
            MethodPtr impl,
            uint64_t parentOffsetInThis) {
        if (!impl || !impl->getLlvmFunctionType()) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
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

    llvm::GlobalVariable* CajetaClass::getOrCreateSecondaryVTable(
            CajetaClassPtr parent) {
        if (!parent) return nullptr;
        std::string parentCanon = parent->getQName()->toCanonical();
        auto cached = secondaryVTables.find(parentCanon);
        if (cached != secondaryVTables.end()) return cached->second;

        // Make sure parent's standalone vtable exists so we can reuse
        // its LLVM struct type for layout-compatible dispatch.
        if (!parent->getVirtualTableGlobal()) {
            parent->writeVirtualTable();
        }
        llvm::StructType* parentVtableType = parent->getVirtualTableType();
        if (!parentVtableType) return nullptr;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
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

        // Index our own primary vtable by hash so we can pick up
        // most-derived overrides (buildVirtualTable already aliased
        // parent canonicals to our impls).
        std::map<int64_t, MethodPtr> ourByHash;
        {
            auto mIt = virtualMethodList.begin();
            for (size_t i = 0; i < virtualSlotHashList.size()
                    && mIt != virtualMethodList.end(); ++i, ++mIt) {
                ourByHash[virtualSlotHashList[i]] = *mIt;
            }
        }

        llvm::ArrayType* entriesArrTy = llvm::cast<llvm::ArrayType>(
            parentVtableType->getTypeAtIndex(4));
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

        std::vector<llvm::Constant*> initArgs{
            llvm::ConstantInt::get(i16Ty, llvm::APInt(16, 0, false)),
            llvm::ConstantInt::get(i16Ty,
                llvm::APInt(16, parentSlots.size(), false)),
            parentVtableRef,
            dropFnConst,
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

    void CajetaClass::generatePrototype() {
        // Idempotent — the deferred-prototype machinery
        // (CajetaModule::buildPendingPrototypes) may attempt this multiple
        // times as parents fill in. The flag is set at the end so a
        // partial / aborted run doesn't claim done.
        if (prototypeBuilt) return;

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

        // Interfaces participate as types (registered in canonicalMap so
        // variables can be declared at the interface type, and methods can
        // be looked up by signature) but have no instance layout, no vtable
        // of their own, and no default constructor. Their methods are
        // abstract — Method::generatePrototype skips LLVM-function creation
        // for them. Implementing classes' vtables carry concrete entries
        // keyed under the interface methods' canonical hashes (see
        // CajetaClass::buildVirtualTable).
        if (isInterface()) {
            // Interface fat pointer (S9.5.1): { ptr data, ptr vtable, i64 kind }
            // = 24 bytes. data points at the underlying class instance or
            // struct body; vtable points at the per-(impl, iface) global
            // synthesized by S9.2 / S9.5.2; kind is one of
            // IFACE_KIND_BORROWED_CLASS / OWNED_CLASS / BORROWED_STRUCT
            // and drives drop-chain dispatch at scope exit (S10.4).
            llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
            typeMap[TypeKey(llvmType)] = shared_from_this();
            llvm::Type* ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(*module->getLlvmContext());
            vector<llvm::Type*> members{ ptrTy, ptrTy, i64Ty };
            ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(members), false);

            canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
            canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
            typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
            module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
            // Resolve interface-extends-interface chains so an
            // implementing class's vtable can walk transitively
            // through getSuperClasses() and pick up parent-interface
            // method obligations.
            resolveSuperClasses();
            for (auto& m : methods) {
                m.second->generatePrototype();
            }
            CajetaModule::getStructureToModule()[canonical] = module;
            prototypeBuilt = true;
            return;
        }

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

        // Register self in the module's structure map BEFORE method/vtable
        // generation so any later-declared subclass that lists us in its
        // `extends` clause can find us by name. Also: resolve our own
        // parents now — they must have been registered by their own
        // prototype generation, which means declared earlier in the source.
        module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
        resolveSuperClasses();
        resolveImplementedInterfaces();

        // Class instance layout: { ptr vtable, <inherited fields…>, <own
        // fields…> }. The vtable pointer at LLVM index 0 is set by `new
        // ClassName()` (see ClassCreatorRest). Inherited fields land
        // immediately after the vtable at the SAME indices they occupy
        // in their declaring parent's struct — so a GEP for an inherited
        // field works on a subclass instance the same way it works on
        // the parent (single-inheritance C++-style layout). Subclass own
        // fields come after the inherited block; getFieldLlvmIndex's
        // `countInheritedFields() + order + 1` formula accounts for the
        // shift.
        // Pick the LLVM type for a property when laying it out inside
        // the enclosing class struct. Reference-typed fields — arrays
        // AND plain class refs — are stored as **pointers** to the
        // heap-allocated body, not inline.
        //
        // Why: embedding the body inline (a) leaves nowhere for the
        // heap-returned pointer to land (the slot's bit-pattern would
        // be the inline body, not a reference), and (b) for template
        // instantiations whose body is a named/anonymous struct (e.g.
        // `Stream<int32>` lowers to a `{ ptr vtable }` `%union.anon`),
        // any call site passing the loaded field value to a `ptr`-typed
        // parameter trips the LLVM verifier ("Call parameter type does
        // not match function signature"). The methods-only / single-
        // word case happens to round-trip under opaque pointers because
        // the slot's bit-pattern is one pointer wide; classes with
        // additional fields or distinct named types would crash
        // unpredictably. Uniform `ptr` storage matches the Method
        // signature convention (class params/returns are `ptr` — see
        // Method.cpp:451) and the existing GEP+load code path that
        // already loads class-ref fields through as `ptr`.
        //
        // CajetaView (zero-copy struct) and CajetaInterface (24-byte
        // fat pointer) keep inline storage: views are by-design typed
        // overlays on a wire-format buffer, and interface values are
        // their fat pointer body.
        auto* lctx = module->getLlvmContext();
        auto fieldLayoutType = [&](const StructurePropertyPtr& p) -> llvm::Type* {
            CajetaTypePtr t = p->getType();
            if (dynamic_pointer_cast<CajetaArray>(t)) {
                return llvm::PointerType::get(*lctx, 0);
            }
            if (auto cls = dynamic_pointer_cast<CajetaClass>(t)) {
                if (dynamic_pointer_cast<CajetaView>(t)) {
                    return t->getLlvmType();  // inline view body
                }
                if (cls->isInterface()) {
                    return t->getLlvmType();  // inline 24-byte fat pointer
                }
                return llvm::PointerType::get(*lctx, 0);
            }
            return t->getLlvmType();
        };

        // Per-parent sub-object layout (Gap 8). For C extends A, B:
        //   { vtable_primary, A's-sub-object-content (shares vtable),
        //     vtable_secondary_for_B, B's-sub-object-content,
        //     C's own fields }
        // The FIRST parent shares this class's primary vtable pointer
        // (saves a slot and matches C++'s single-inheritance fast path).
        // Every non-first parent gets a fresh vtable slot at the start
        // of its sub-object so a `Parent*` view into that region looks
        // structurally identical to `Parent` standalone — which means
        // the parent's pre-compiled ctor/method IR (using the parent's
        // own struct GEP indices) "just works" when handed a pointer
        // adjusted by getSubObjectByteOffset.
        //
        // We populate subObjectSlotMap in lockstep so getSubObjectByteOffset
        // can compute the byte adjustment for any ancestor.
        vector<llvm::Type*> llvmMembers;
        subObjectSlotMap.clear();
        llvm::PointerType* vptrTy = llvm::PointerType::get(*lctx, 0);

        // 'enclosingStart' = slot where the surrounding sub-object's vtable
        // sits. Used to record `subObjectSlotMap[firstParent]` (which shares).
        //
        // MultiClassing Phase 3 v1 (cajeta-docs/stdlib/MultiClassing.md § P-4):
        // when an ancestor is reachable through multiple paths (true diamond),
        // record the CANONICAL (first-encountered) offset in subObjectSlotMap.
        // Without this guard, the second walk would overwrite with a later
        // offset and `getSubObjectByteOffset(A)` would return the wrong
        // position — `this<A>.x` and `this.x` would land on different
        // storage. Layout still emits A's content twice per the v1 scope
        // (full dedup deferred to v2 because non-first parents'
        // standalone IR assumes inline A — removing it without ABI rework
        // would break `this<C>.sharedField` and inherited methods on
        // non-first parents that mutate the ancestor via `this.x`).
        vbaseAncestors.clear();
        vbaseSlotMap.clear();
        auto selfRaw = static_cast<const CajetaClass*>(this);

        // Helper: transitive non-self ancestors in DFS order, deduped.
        // Each class's standalone layout reserves one vbase ptr slot
        // per such ancestor at the end of its own contribution; when a
        // class is embedded as a sub-object, the same slots are
        // physically present in the descendant's layout (so the parent's
        // standalone IR's GEPs land correctly).
        //
        // Object is excluded: every user class auto-extends Object (see
        // CajetaLlvmVisitor.h `extends Object` injection), Object has no
        // instance fields, and no method ever GEPs through `this` for an
        // Object-declared field. Including it would add one ptr per class
        // for no benefit and balloon struct sizes / break tests that
        // assume specific layouts.
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
                subObjectStart = (int) llvmMembers.size();  // about-to-add vtable slot
                llvmMembers.push_back(vptrTy);
            } else {
                subObjectStart = enclosingStart;
            }
            // Phase 3 v1: only record on FIRST encounter. Diamond
            // ancestors keep their canonical offset; subsequent
            // emissions add storage but don't move the map entry.
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
            // MultiClassing Phase 3 v4 vbase pointers — one per cls's
            // transitive non-self ancestor. Placed after cls's own
            // properties so existing field GEP indices stay stable in
            // cls's standalone layout AND in the descendant's
            // flattened layout. Only the outermost (self) class
            // records vbaseSlotMap entries — embedded sub-objects'
            // vbases use their OWN class's map (populated when that
            // class's standalone generatePrototype runs / ran).
            auto ancestors = collectAncestors(cls);
            for (auto& anc : ancestors) {
                if (cls.get() == selfRaw) {
                    vbaseSlotMap[anc.get()] = (int) llvmMembers.size();
                    vbaseAncestors.push_back(anc);
                }
                llvmMembers.push_back(vptrTy);
            }
        };
        // View-as-class-field rejection (Views.md § Errors caught statically).
        // Views are buffer overlays with borrowed lifetime; embedding one in
        // a class field — directly or via an array element — creates an
        // unresolvable lifetime hazard (the class can outlive the buffer)
        // AND breaks the compiler's calling convention (function signatures
        // pick `ptr` for class-typed fields/returns, but the actual storage
        // for a view field is the inline struct body — every templated method
        // that touches a view T trips the LLVM verifier with
        // "Function return type does not match operand type of return inst!"
        // or "Call parameter type does not match function signature!").
        //
        // Fires for:
        //   - direct view fields: `class C { view V v; }`
        //   - array-of-view fields: `class C { view V[] vs; }`
        //   - template instantiations: HashMap<int32, view> instantiates
        //     `V[] vals` as `view[]`, caught by the array-of-view branch.
        //
        // Skips views themselves (a nested view inside a view is legitimate
        // — see Views.md § Nested views).
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
                        "borrowed lifetime — see cajeta-docs/stdlib/Views.md "
                        "(Errors caught statically). Workaround: store the "
                        "underlying byte[] in the class and construct the "
                        "view per access; or pass the view by value across "
                        "method boundaries without storing it.",
                        "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
                }
            }
        }

        embedSubObject(static_pointer_cast<CajetaClass>(shared_from_this()),
            /*ownVtable=*/true, /*enclosingStart=*/0);

        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        // Lombok ctor annotations run BEFORE ensureDefaultConstructor —
        // if @NoArgsConstructor (etc.) adds a ctor, the populated map
        // tells ensureDefaultConstructor to skip its auto-default add.
        // Otherwise an unannotated ctor-less class still gets the auto-
        // default. (Lombok's semantic: @AllArgsConstructor alone gives
        // only the all-args ctor, not also an implicit no-arg.)
        synthesizeNoArgsConstructor();
        synthesizeAllArgsConstructor();
        synthesizeRequiredArgsConstructor();
        ensureDefaultConstructor();
        synthesizeAutoHash();
        synthesizeGetters();
        synthesizeSetters();
        synthesizeToString();
        synthesizeWith();
        // @Encoding adds instance methods (`T(byte[])` ctor + `byte[]
        // toBytes()`) that need to land in the vtable. Run BEFORE the
        // method-prototype loop so the new methods get prototyped here
        // and BEFORE writeVirtualTable (called later) sees their slot.
        // The synthesizer's generateCode (which looks up the encoder's
        // encode/decode by class-ref) runs in a later codegen pass —
        // by then all classes are prototyped.
        synthesizeEncoding();

        for (auto methodEntry: methods) {
            methodEntry.second->generatePrototype();
        }
        // Builder runs AFTER method prototypes so the all-args ctor's
        // llvm::Function* is already built — the synthesized build()
        // method calls it by pointer. Builder's own generatePrototype
        // (called inside synthesizeBuilder) handles its method LLVM
        // function creation.

        // Vtable build runs AFTER every method has its LLVM Function — the
        // constant needs `getLlvmFunction()` to return non-null for each slot.
        // Classes with only static methods/constructors produce a 2-slot
        // header-only vtable (`{ i16 version, i16 count = 0 }`); meaningful
        // dispatch arrives when inheritance and virtual calls are wired up.
        writeVirtualTable();

        // S9.5.2 — synthesize per-(class, interface) vtable globals for
        // every interface this class implements. Lands here, after method
        // prototypes + writeVirtualTable, so each implementing method's
        // LLVM function exists for fn-pointer harvesting and the per-pair
        // tables can sit alongside the legacy hash-keyed vtable.
        synthesizeInterfaceVTables();

        CajetaModule::getStructureToModule()[canonical] = module;
        prototypeBuilt = true;

        // @Builder runs LAST — after this class's prototype + vtable
        // are fully built. The Builder synthesizer creates a fresh
        // nested CajetaClass and walks IT through generatePrototype,
        // which would recurse into our own un-built state if invoked
        // earlier. Builder also adds a `builder()` static method to
        // this class; that method's LLVM function gets created via a
        // direct generatePrototype call inside synthesizeBuilder.
        synthesizeBuilder();
    }

    std::vector<MethodPtr> CajetaClass::getFlattenedInterfaceMethods() {
        // BFS: this interface, then its parents (transitively). Each
        // method is emitted in the order it's first encountered;
        // a name seen at a leaf is NOT emitted again when we reach a
        // parent that declares the same method (the leaf override
        // wins). Constructors and statics are filtered.
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

    void CajetaClass::synthesizeInterfaceVTables() {
        if (implementedInterfaces.empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
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

        // Walk implementedInterfaces transitively so a per-(impl, iface)
        // vtable is synthesized for EACH ancestor interface in the
        // implements chain — not just the directly-named ones. Without
        // this, `class C implements Codec<int32>` where
        // `interface Codec<T> extends Reader<T>` builds a vtable for
        // Codec<int32> only; an upcast `Reader<int32> r = c` finds no
        // vtable, stores null in the fat pointer, and dispatch through
        // r crashes.
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

        for (auto& iface : allIfaces) {
            std::string ifaceCanonical = iface->getQName()->toCanonical();

            // Walk interface methods in declaration order; for each one
            // find a same-name concrete method on this class. Signature
            // compatibility for class implementers is already enforced
            // at hash-vtable construction time (a mismatched signature
            // would hash differently and fail to bind), so the lookup
            // here is by name only — by the time we get here the user
            // either has a matching impl or the hash-vtable build has
            // already complained.
            auto findByName = [&](const std::string& name) -> MethodPtr {
                for (auto& [canon, m] : methods) {
                    if (!m || m->isConstructor()) continue;
                    if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                    if (m->getName() == name) return m;
                }
                // Walk superclasses too — inherited implementations
                // count for satisfying an interface contract.
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
            // S10.4 — slot 0 of every per-(impl, iface) vtable holds the
            // implementer's drop function. The interface drop helper
            // (__cajeta_iface_drop) reads this slot when kind ==
            // OWNED_CLASS. Method entries follow at slots 1..N; the
            // dispatch path adds +1 to its interface-method index.
            if (llvm::Function* dropFn = this->getOrCreateDropFunction()) {
                entries.push_back(dropFn);
            } else {
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            // Walk the interface's flattened method list (self + parent
            // interfaces, in vtable order). Same list invokeMethod walks
            // for methodIdx, so an inherited method like `Stream<T>.next()`
            // on a `Splittable<T> extends Stream<T>` formal dispatches
            // correctly.
            for (auto& ifaceMethod : iface->getFlattenedInterfaceMethods()) {
                if (!ifaceMethod || ifaceMethod->isConstructor()) continue;
                if (ifaceMethod->getModifiers().find(STATIC)
                        != ifaceMethod->getModifiers().end()) continue;
                MethodPtr concrete = findByName(ifaceMethod->getName());
                if (!concrete || !concrete->getLlvmFunction()) {
                    // Missing or unbuilt — leave a null slot. The hash-
                    // vtable build will have surfaced the real diagnostic
                    // earlier; null here just keeps the global well-formed.
                    entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
                    continue;
                }
                entries.push_back(concrete->getLlvmFunction());
            }

            llvm::ArrayType* arrTy = llvm::ArrayType::get(
                ptrTy, entries.size());
            std::string globalName = std::string("class.")
                + sanitize(classCanonical) + "_iface_"
                + sanitize(ifaceCanonical) + "_VTable";
            if (llvm::GlobalVariable* existing = lmod->getNamedGlobal(globalName)) {
                interfaceVTables[ifaceCanonical] = existing;
                continue;
            }

            llvm::Constant* init = llvm::ConstantArray::get(arrTy, entries);
            // ExternalLinkage so cross-module references (e.g. a user
            // module's invokeMethod-built fat pointer that referenced
            // this vtable via ensureGlobalInModule) resolve at link
            // time to this definition. With InternalLinkage the donor's
            // definition gets renamed during Linker::linkModules and
            // the consumer's extern decl stays unsatisfied — manifests
            // as "Failed to materialize symbols" at JIT init.
            auto* gv = new llvm::GlobalVariable(
                *lmod, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage, init, globalName);
            interfaceVTables[ifaceCanonical] = gv;
        }
    }

    void CajetaClass::synthesizeAutoHash() {
        // Gated on @AutoHash class annotation. Inject a structural
        // hash() that walks fields, hashes each via the matching
        // __cajeta_hash_X primitive (or virtual dispatch for class-
        // typed fields), and combines via __cajeta_hash_combine
        // seeded with __cajeta_hash_seed.
        //
        // Skip when:
        //   - The annotation isn't present (default identity hash
        //     inherited from Object stands).
        //   - The class already declares hash() manually (user
        //     opt-out from the synthesizer for this class even with
        //     @AutoHash present — the manual version wins).
        //
        // The synthesizer throws on unsupported field kinds with a
        // diagnostic naming the class, field, and remediation. See
        // SynthesizedHashMethod::generateCode for the rules.
        // @Data and @Value bundles also imply @AutoHash. Direct
        // @AutoHash works on its own as before.
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

    // Resolve `access="…"` (per cajeta-docs/stdlib/Annotations.md §
    // Accessors) to the matching Modifier bit. Default is PUBLIC.
    // Unknown values raise CAJETA_ERROR_ACCESSOR_BAD_ACCESS with the
    // annotation site for the diagnostic context. v1 honors the
    // four-way `public` / `private` / `protected` / `package`
    // surface — the modifier lands on the synthesized method, and
    // any future visibility-enforcement pass at call sites consumes
    // it. (Today Cajeta does not enforce method visibility at call
    // sites; Reflection.md already assumes it will.)
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

    void CajetaClass::synthesizeGetters() {
        // @Getter at class level (every non-static field gets a getter)
        // OR at field level (only that field). User-declared methods
        // with the same name and zero params win — synthesizer skips.
        // Naming: getter for field `name` is `name()`, size()-style
        // (see cajeta-docs/stdlib/Annotations.md § Accessors).
        //
        // @Data and @Value bundles also enable class-level @Getter.
        auto classAnn = findAnnotation("Getter");
        bool classLevel = classAnn != nullptr
                       || findAnnotation("Data")   != nullptr
                       || findAnnotation("Value")  != nullptr;

        // Resolve class-level access default (defaults to PUBLIC; bundle
        // forms don't expose `access` because @Data/@Value imply the
        // accessor surface is part of the public contract).
        Modifier classAccess = resolveAccessModifier(
            classAnn, "Getter", qName->toCanonical());

        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            auto fieldAnn = prop->findAnnotation("Getter");
            bool fieldLevel = fieldAnn != nullptr;
            if (!classLevel && !fieldLevel) continue;

            // User declared a same-name zero-arg method? Skip.
            bool exists = false;
            for (auto& m : methodList) {
                if (!m || m->isConstructor()) continue;
                if (m->getName() != prop->getName()) continue;
                if (m->getParameters().size() != 0) continue;
                exists = true;
                break;
            }
            if (exists) continue;

            // Field-level @Getter(access=...) wins when present;
            // otherwise inherit the class-level default.
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

    void CajetaClass::synthesizeSetters() {
        // @Setter at class level (every non-static, non-final field) OR
        // at field level (only that field). Skipped for `final` fields
        // (Lombok parity — final fields aren't reassignable). User-
        // declared same-name single-arg method wins.
        //
        // @Data bundle enables class-level @Setter. @Value bundle does
        // NOT — @Value is the immutable variant, no setters by design.
        // If both @Value and explicit @Setter are present, @Value wins
        // (immutability contract is the stronger guarantee).
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

            // User-declared same-name single-arg method? Skip.
            bool exists = false;
            for (auto& m : methodList) {
                if (!m || m->isConstructor()) continue;
                if (m->getName() != prop->getName()) continue;
                // Match by arity — same name + 1 user-visible param.
                // (parameterList includes `this`, so a 1-arg user method
                // would have 2 entries. If generatePrototype hasn't run
                // yet, parameterList has just the 1 user param.)
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
            // Wire the value parameter's parent now that the shared_ptr
            // exists (FormalParameter.parent is what
            // StructureMetadata::createParameterType reads via parent->
            // toCanonical(); a null parent there segfaults RTTI build).
            setter->initParameter();
            addMethod(setter);
        }
    }

    void CajetaClass::synthesizeToString() {
        // @ToString class-level annotation only; field-level @ToString
        // doesn't make sense. Synthesize a single `String toString()`
        // walking non-static, non-excluded fields in declaration order.
        // User-declared toString() (with `this` or no params) wins.
        //
        // @Data and @Value bundles also enable @ToString (with default
        // PROPERTIES format). A direct @ToString annotation can pass
        // format=TO_STRING_JSON etc.; via bundle, only the default form
        // fires.
        auto ann = findAnnotation("ToString");
        bool bundleEnabled = findAnnotation("Data") || findAnnotation("Value");
        if (!ann && !bundleEnabled) return;

        // Resolve the requested format. Empty / TO_STRING_PROPERTIES =>
        // PROPERTIES. TO_STRING_JSON => JSON. Anything else is rejected.
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
            // User-declared toString() — skip synthesis.
            return;
        }

        // `of={...}` allowlist: pin the rendered fields and their
        // order. Unknown names are rejected here so the diagnostic
        // points at the annotation site, not at a confusing missing-
        // field error later. An EMPTY `of={}` means "render zero
        // fields" — distinct from "no `of` supplied" (which means
        // walk all non-static, non-@Exclude fields in declaration
        // order). The findArg/getStringList split is what
        // distinguishes the two: findArg("of") returns non-null
        // only when the user wrote `of=...`.
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
                    throw Exception(
                        "@ToString(of={...}) on `"
                        + qName->toCanonical()
                        + "` names unknown field `" + name + "`. "
                        "Allowlisted field names must match a "
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

    // Returns true if the unlabeled constructor map already holds a
    // ctor with the given user-visible-arg count (excluding `this`).
    // Used by the three ctor synthesizers to skip when the user already
    // declared a ctor with the same shape. Ctors don't go into
    // methodList — they only land in the constructor maps.
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

    // Shared ctor-synth dispatcher. Each of the three annotations
    // (@NoArgs / @AllArgs / @RequiredArgs) computes its field list
    // and calls this with the resolved access modifier + optional
    // staticName. When staticName is set, Lombok parity applies:
    //   - the ctor is forced PRIVATE (the user is opting into
    //     factory-style construction);
    //   - the `access` arg applies to the synthesized static factory
    //     (the public surface) rather than the now-private ctor;
    //   - a SynthesizedStaticFactoryMethod lands next to the ctor,
    //     allocating + invoking the ctor + returning the instance.
    void CajetaClass::emitCtorAndOptionalFactory(
            const AnnotationInstancePtr& ann,
            const std::string& annotationName,
            std::vector<StructurePropertyPtr> fields,
            Modifier access) {
        std::string staticName = ann ? ann->getString("staticName") : "";
        Modifier ctorAccess = staticName.empty() ? access : PRIVATE;
        Modifier factoryAccess = staticName.empty() ? access : access;
        (void) annotationName;  // reserved for future diagnostic use

        auto self = std::static_pointer_cast<CajetaClass>(shared_from_this());
        auto fieldsForFactory = fields;  // copy for the factory; the
                                          // ctor takes ownership of the
                                          // original via move below.
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

    void CajetaClass::synthesizeNoArgsConstructor() {
        auto ann = findAnnotation("NoArgsConstructor");
        if (!ann) return;
        if (ctorWithArityExists(0)) return;
        Modifier access = resolveAccessModifier(
            ann, "NoArgsConstructor", qName->toCanonical());
        emitCtorAndOptionalFactory(ann, "NoArgsConstructor",
            std::vector<StructurePropertyPtr>{}, access);
    }

    void CajetaClass::synthesizeAllArgsConstructor() {
        // @Value bundle and @Builder both implicitly enable
        // @AllArgsConstructor. (For @Builder, build()'s body calls
        // the outer's all-args ctor; for @Value, an all-args ctor is
        // the canonical way to initialize an immutable value.)
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

    void CajetaClass::synthesizeRequiredArgsConstructor() {
        // @Data bundle also enables @RequiredArgsConstructor.
        auto ann = findAnnotation("RequiredArgsConstructor");
        if (!ann && !findAnnotation("Data")) return;
        Modifier access = resolveAccessModifier(
            ann, "RequiredArgsConstructor", qName->toCanonical());
        // v1 selects `final` fields only. @NonNull (once implemented)
        // will widen the predicate per Lombok's contract.
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

    void CajetaClass::synthesizeWith() {
        // @With at class level (every non-static field gets a with) OR
        // at field level (only that field). User-declared method with
        // the same name+arity wins.
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

            // User-declared same-name single-arg method? Skip.
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

    void CajetaClass::synthesizeEncoding() {
        auto encAnn = findAnnotation("Encoding");
        if (!encAnn) return;

        // Mutual-exclusion check: @Encoding owns the wire format, so
        // it can't coexist with packed-layout annotations
        // (@BigEndian / @LittleEndian / @HostEndian / @Align).
        // See cajeta-docs/stdlib/Annotations.md § @Encoding for views
        // — § Composition with existing annotations.
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

        // Phase B: synthesize the byte[]-taking ctor + toBytes() that
        // delegate to the user-supplied encoder class's static
        // encode/decode methods.
        std::string encoderName = encAnn->getClassRef("value");
        if (encoderName.empty()) encoderName = encAnn->getString("value");
        if (encoderName.empty()) {
            throw Exception(
                "@Encoding on `" + qName->toCanonical()
                + "` is missing the encoder class arg: write "
                "`@Encoding(MyEncoder.class)`",
                "CAJETA_ERROR_ENCODING_NO_ARG");
        }

        // Look up the encoder class. Accept short name or canonical
        // — mirror the @Override(from=...) resolution pattern.
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

        // Ensure encoder's prototype is built so its encode/decode
        // method LLVM functions exist by the time our synth's
        // generateCode runs. Idempotent (prototypeBuilt guard).
        encoder->generatePrototype();

        // S10.5 — verify Encoder<T> conformance, IF the encoder opted in.
        // The check is *soft* on absence (the duck-typed dispatch path
        // is the v1 baseline and shipping users may not have added the
        // `implements` clause yet) but *hard* on mismatch: if the
        // encoder declared `implements Encoder<U>` and U doesn't match
        // the @Encoding parent, the @Encoding annotation is lying.
        // Catching it here beats catching it at first call with a
        // confusing decoded-bytes-don't-fit error.
        //
        // Resolution rule: a type-arg matches T when its short or
        // canonical name equals T's short or canonical name. Matches
        // the same liberal lookup that resolveImplementedInterfaces
        // and findStaticUnaryMethod already use.
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
                // Match `implements Encoder<...>` (short or canonical;
                // the encoder author may not have imported
                // cajeta.wire.Encoder explicitly).
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
                // First matching Encoder<...> entry wins; no need to
                // walk the rest of the implements list.
                break;
            }
        }

        // Skip when the user already declared the equivalents (same
        // arity match used by other synthesizers).
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
            // generatePrototype is called by the outer for-loop in
            // CajetaClass::generatePrototype (we run BEFORE that loop).
        }
        if (!toBytesExists) {
            auto tb = std::make_shared<SynthesizedEncodingToBytes>(
                module,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                encoder);
            addMethod(tb);
        }
    }

    void CajetaClass::synthesizeBuilder() {
        auto ann = findAnnotation("Builder");
        if (!ann) return;

        // Naming customizations (Lombok parity):
        //   - builderMethodName: static factory on outer, default "builder"
        //   - buildMethodName:   build() on Builder,      default "build"
        //   - setterPrefix:      chained setter prefix,    default "" (bare field name)
        // setterPrefix capitalizes the field name's first letter when
        // present, so `prefix="with"` + field `name` becomes `withName`.
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

        // Step 1: gather outer's non-static fields (in declaration order).
        std::vector<StructurePropertyPtr> outerFields;
        for (auto& prop : propertyList) {
            if (!prop || prop->isStatic()) continue;
            outerFields.push_back(prop);
        }

        // Step 2: create the Builder CajetaClass. QualifiedName uses
        // the outer's canonical as the package — same dotted shape that
        // visitClassDeclaration synthesizes for source-declared nested
        // classes (line 81-83 of CajetaLlvmVisitor.h).
        auto builderQName = QualifiedName::getOrInsert(
            "Builder", qName->toCanonical());
        std::list<QualifiedNamePtr> builderExtends{
            QualifiedName::getOrInsert("Object", "cajeta.lang")
        };
        std::list<QualifiedNamePtr> builderImplements;
        auto builder = std::make_shared<CajetaClass>(
            module, builderQName, builderExtends, builderImplements);

        // Step 3: register in canonicalMap under both the full canonical
        // and the short typeName (matches what generatePrototype does
        // for ordinary classes via canonicalMap[...] = ...).
        CajetaType::getCanonicalMap()[builderQName->toCanonical()] =
            std::static_pointer_cast<CajetaType>(builder);
        // Don't shadow the short name — there may be unrelated
        // `Builder` classes in other packages; the dotted canonical is
        // the disambiguator.

        // Step 4: mirror outer's non-static fields as Builder fields.
        // Builder fields are private (Lombok parity); the chained setter
        // is the public interface.
        int order = 0;
        for (auto& prop : outerFields) {
            auto mirror = std::make_shared<StructureProperty>(
                prop->getName(), prop->getType(), order++);
            mirror->addModifier(PRIVATE);
            builder->addProperty(mirror);
        }

        // Step 5: add the no-arg ctor (zero-init all fields). Mirrors
        // what @NoArgsConstructor would have produced — but we add it
        // directly rather than going through the annotation gating.
        {
            auto ctor = std::make_shared<SynthesizedConstructorMethod>(
                module, builder, std::vector<StructurePropertyPtr>{});
            ctor->initParameters();
            builder->addMethod(ctor);
        }

        // Step 6: add the chained setters (one per mirrored field).
        for (auto& mirror : builder->getPropertyList()) {
            if (!mirror) continue;
            auto setter = std::make_shared<SynthesizedBuilderSetterMethod>(
                module, builder, mirror,
                prefixedSetterName(mirror->getName()));
            setter->initParameter();
            builder->addMethod(setter);
        }

        // Step 7: add build(). Note this references outer's all-args
        // ctor by LLVM Function* — that ctor was prototyped during our
        // own method-prototype loop (before this synthesizeBuilder
        // call), so its llvmFunction is non-null.
        {
            auto buildMethod = std::make_shared<SynthesizedBuildMethod>(
                module, builder,
                std::static_pointer_cast<CajetaClass>(shared_from_this()),
                buildMethodName);
            builder->addMethod(buildMethod);
        }

        // Step 8: generate Builder's prototype. This builds Builder's
        // LLVM struct type, prototypes its methods, and emits its
        // vtable + RTTI. The recursive call inside generatePrototype
        // hits the prototypeBuilt guard for THIS class (we set
        // prototypeBuilt = true above before calling synthesizeBuilder).
        builder->generatePrototype();

        // Step 9: collect @Builder.Default field defaults. Match the
        // annotation by qualified shape — `@Builder.Default` parses to
        // {package="Builder", typeName="Default"}, and the alternate
        // single-identifier `@BuilderDefault` form (parses to
        // {package="", typeName="BuilderDefault"}) is accepted too so
        // users importing Builder.Default with a flat name don't lose
        // the semantic. A bare `@Default` is intentionally NOT a
        // match — too generic to safely steal from other annotation
        // surfaces.
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
            // outerFields and builder->getPropertyList() are aligned
            // 1:1 by construction (Step 4 mirrored them in order).
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

        // Step 10: add `static Outer.Builder builder()` to outer
        // (renamed if `builderMethodName` was supplied).
        auto factory = std::make_shared<SynthesizedBuilderFactoryMethod>(
            module,
            std::static_pointer_cast<CajetaClass>(shared_from_this()),
            builder,
            builderMethodName,
            std::move(defaults));
        addMethod(factory);
        // We're past our own method-prototype loop; manually prototype
        // the factory so its LLVM function lands in the module.
        factory->generatePrototype();
        // Add to module's structure roster so codegen picks it up.
        // Also add the Builder class so it shows up in the codegen
        // worklist alongside outer.
        module->getStructures()[builderQName->toCanonical()] = builder;
        CajetaModule::getStructureToModule()
            [builderQName->toCanonical()] = module;
    }

    void CajetaClass::ensureDefaultConstructor() {
        // Previous version looked up `qName->getTypeName()` in `methods`,
        // but `methods` is keyed by the full canonical (`"pkg.Class::Class()"`),
        // never by the bare type name — so this check missed every
        // user-declared constructor and a synthesized default was added
        // after the user's, which then threw "Constructor already
        // exists" from addMethod when canonical signatures collided.
        // Consult the constructor map directly instead.
        if (!unlabeledConstructorMap.empty()) return;
        addMethod(make_shared<DefaultConstructorMethod>(
            module, static_pointer_cast<CajetaClass>(shared_from_this())));
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
        // P6.2 — emit clinit for any static field whose initializer
        // didn't constant-fold at global-creation time. Runs after
        // method codegen so the IRBuilder + module state are fully
        // initialized; the clinit body uses the same expression
        // codegen pipeline as ordinary method bodies.
        generateStaticInitializers();
    }

    void CajetaClass::generateStaticInitializers() {
        // First pass — pick out static properties that have an
        // initializer AND whose shape isn't covered by the
        // constant-folder. Anything covered by foldStaticInitializer
        // is already baked into the global's initializer and would be
        // re-stored to the same value by the clinit, so skip it.
        std::vector<StructurePropertyPtr> needsClinit;
        for (auto& prop : propertyList) {
            if (!prop || !prop->isStatic()) continue;
            if (!prop->getInitializer()) continue;
            if (!prop->getType() || !prop->getType()->getLlvmType()) continue;
            llvm::Type* storedType = prop->getType()->getLlvmType();
            if (!(prop->getType()->getTypeFlags() & PRIMITIVE_FLAG)) {
                storedType = llvm::PointerType::get(
                    *module->getLlvmContext(), 0);
            }
            if (foldStaticInitializer(prop->getInitializer(), storedType)) {
                continue;
            }
            needsClinit.push_back(prop);
        }
        if (needsClinit.empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();

        std::string fnName = std::string("__cajeta_clinit_")
            + qName->toCanonical();
        for (char& c : fnName) {
            if (c == ':' || c == '.' || c == '<' || c == '>'
                    || c == ',' || c == ' ') {
                c = '_';
            }
        }
        if (lmod->getFunction(fnName)) return;  // re-entry guard

        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), /*isVarArg=*/false);
        llvm::Function* clinit = llvm::Function::Create(fnTy,
            llvm::Function::PrivateLinkage, fnName, lmod);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(
            ctx, "entry", clinit);

        // Save the module's current insert point so callers that emit
        // more code after generateCode don't see a clinit-internal
        // builder state.
        auto* builder = module->getBuilder();
        auto savedBB = builder->GetInsertBlock();
        auto savedIP = savedBB
            ? builder->GetInsertPoint()
            : llvm::BasicBlock::iterator();
        builder->SetInsertPoint(entry);

        // Push self onto the structure stack so bare identifier
        // references inside an initializer (e.g. `b = a + 5;`)
        // resolve against this class's static fields. Method::
        // generateCode does the same push for method bodies; without
        // it, IdentifierExpression returns null and BinaryOp crashes.
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
            // Unwrap VariableInitializer to get the actual expression.
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

            // Load-through if the expression returned an l-value
            // (DotExpression on a class-static returns the global, an
            // alloca, or a field GEP). For the supported v1 shapes
            // (arithmetic on int/float literals + static field refs),
            // we expect rvalues; only the static-field-ref case
            // returns a global directly that hasn't been loaded yet.
            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(val)) {
                val = builder->CreateLoad(gv->getValueType(), gv);
            }

            // Coerce to the stored type so the verifier accepts the store.
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
            if (val->getType() != storedType) continue;  // skip on mismatch
            builder->CreateStore(val, g);
        }
        builder->CreateRetVoid();

        if (pushedSelf) {
            module->getStructureStack().pop_back();
        }
        if (savedBB) {
            builder->SetInsertPoint(savedBB, savedIP);
        }

        // Register the clinit with llvm.global_ctors. Default priority
        // 65535 — same bucket as the runtime's __cajeta_runtime_init /
        // __cajeta_hash_seed_init, which is fine: the constructors
        // are independent and order among same-priority ctors is
        // unspecified but doesn't matter here.
        llvm::appendToGlobalCtors(*lmod, clinit, /*Priority=*/65535);
    }

    // Static class fields. Each STATIC property gets a dedicated LLVM
    // global variable, defined in the class's home module and named
    // `<class.canonical>.<propertyName>`. Lazy — created on first
    // access from any module. Cross-module callers receive an extern
    // decl in their own llvm::Module that resolves to the home
    // definition at JIT link.
    //
    // Storage shape mirrors what StackField uses for the equivalent
    // local: primitives store their value directly (i32, i64, double,
    // ...); reference types (String, class instances, arrays) store
    // a ptr — they're heap-allocated and the slot holds the address.
    //
    // v1 initializer: zero (`llvm::Constant::getNullValue`). User-
    // supplied initializers (`public static int32 base = 100;`) are
    // a follow-on landing — until then the initial value is 0 / null
    // and `Class.field = literal` writes from method body do the
    // initialization at runtime.
    // Fold a static-field initializer expression to an llvm::Constant if
    // the shape is a known compile-time literal. Returns null for any
    // shape we don't statically evaluate yet — caller falls back to
    // zeroinitializer. Supported v1:
    //   - IntegerLiteralExpression (any radix); coerced to `storedType`
    //     when the field is an integer
    //   - FloatLiteralExpression; coerced to `storedType` when the
    //     field is a floating-point type
    //   - PrefixExpression with NEGATIVE applied to either of the above
    // Out of scope (would need a `<clinit>`-style runtime init function):
    //   computed expressions, method calls, references to other
    //   statics, string literals (which materialize through global
    //   string constants — supportable but deferred).
    static llvm::Constant* foldStaticInitializer(
            AbstractSyntaxNodePtr init, llvm::Type* storedType) {
        if (!init || !storedType) return nullptr;
        // VariableInitializer wraps the actual expression as its single
        // child. Unwrap once.
        if (auto vi = dynamic_pointer_cast<VariableInitializer>(init)) {
            auto& kids = vi->getChildren();
            if (kids.empty()) return nullptr;
            init = kids[0];
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
            // Strip the radix prefix (`0b`, `0x`) and any trailing `L`
            // / `l` suffix the grammar may have captured.
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

    llvm::GlobalVariable* CajetaClass::getOrCreateStaticFieldGlobal(
            StructurePropertyPtr prop, CajetaModulePtr callerModule) {
        if (!prop || !prop->isStatic()) return nullptr;
        if (!prop->getType() || !prop->getType()->getLlvmType()) return nullptr;

        auto* lmod = module->getLlvmModule();
        const std::string globalName =
            qName->toCanonical() + "." + prop->getName();

        auto it = staticFieldGlobals.find(prop->getName());
        llvm::GlobalVariable* g;
        if (it != staticFieldGlobals.end()) {
            g = it->second;
        } else if (auto* existing = lmod->getNamedGlobal(globalName)) {
            // Already declared in this module (e.g. an earlier extern
            // ref) — adopt it. If it lacks an initializer we'll
            // promote it to a definition below.
            g = existing;
        } else {
            llvm::Type* storedType = prop->getType()->getLlvmType();
            if (!(prop->getType()->getTypeFlags() & PRIMITIVE_FLAG)) {
                storedType = llvm::PointerType::get(
                    *module->getLlvmContext(), 0);
            }
            // Constant-fold the declared initializer if shape allows;
            // otherwise zero-init. Folding handles `= 100`, `= -7`, and
            // float literals — broad enough to cover what user code
            // routinely writes for class-level constants.
            llvm::Constant* init = foldStaticInitializer(
                prop->getInitializer(), storedType);
            if (!init) init = llvm::Constant::getNullValue(storedType);
            g = new llvm::GlobalVariable(
                *lmod, storedType, /*isConstant=*/false,
                llvm::GlobalValue::ExternalLinkage,
                init, globalName);
            staticFieldGlobals[prop->getName()] = g;
        }

        // Cross-module: the caller is emitting IR into a different
        // llvm::Module. Insert an extern decl there and return that.
        if (callerModule && callerModule->getLlvmModule() != lmod) {
            llvm::Constant* shim = CajetaModule::ensureGlobalInModule(
                callerModule->getLlvmModule(), g);
            return llvm::cast<llvm::GlobalVariable>(shim);
        }
        return g;
    }

    // Gap 1 (virtual dispatch on drop). Patch the vtable global's drop_fn
    // slot (index 3 in the vtable struct layout — see StructureMetadata::
    // createVirtualTableType) so __cajeta_class_virtual_drop's
    // vtable->drop_fn load returns this class's heap-drop wrapper.
    //
    // The slot was set to NULL by createVirtualTableConstant: emitting
    // the drop wrapper from inside vtable construction breaks stdlib
    // linkage (see comment there). Instead, every heap class local's
    // drop-registration site at LocalVariableDeclaration::generateCode
    // calls patch + ensures the wrapper has been generated.
    //
    // Both globals (vtable + drop wrapper) live in this class's home
    // module by construction, so the patch is module-local. Idempotent
    // via llvmDropFunctionPatched.
    void CajetaClass::patchVirtualTableDropFn() {
        if (llvmDropFunctionPatched) return;
        llvm::Function* dropFn = getOrCreateDropFunction();
        if (!dropFn) return;
        if (!llvmVirtualTableGlobal || !llvmVirtualTableType) return;
        llvm::Constant* init = llvmVirtualTableGlobal->getInitializer();
        if (!init) return;
        auto* structInit = llvm::dyn_cast<llvm::ConstantStruct>(init);
        if (!structInit) return;
        // Rebuild the constant with drop_fn replaced (slot index 3).
        std::vector<llvm::Constant*> elems;
        elems.reserve(structInit->getNumOperands());
        for (unsigned i = 0; i < structInit->getNumOperands(); ++i) {
            elems.push_back(structInit->getOperand(i));
        }
        if (elems.size() < 4) return;
        elems[3] = dropFn;
        llvmVirtualTableGlobal->setInitializer(
            llvm::ConstantStruct::get(llvmVirtualTableType,
                llvm::ArrayRef<llvm::Constant*>(elems)));
        llvmDropFunctionPatched = true;
    }

    llvm::Function* CajetaClass::getOrCreateStackDropFunction() {
        if (llvmStackDropFunction) return llvmStackDropFunction;
        if (interfaceFlag) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
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
            llvm::Function::ExternalLinkage, dropName, lmod);
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

        // Call user-defined drop() if present. Same lookup pattern as
        // the heap drop above.
        MethodPtr userDrop;
        for (auto& entry : methods) {
            MethodPtr m = entry.second;
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "drop") continue;
            auto pl = m->getParameterList();
            if (pl.size() == 1) {
                userDrop = m;
                break;
            }
        }
        if (userDrop && userDrop->getLlvmFunction()) {
            b.CreateCall(userDrop->getLlvmFunctionType(),
                userDrop->getLlvmFunction(), {instance});
        }

        // Implicit destructor chaining (MemoryModel.md § 140, C++
        // semantics). Each ancestor's user `~Class()` body runs after
        // this class's body + own fields, in reverse-DFS reverse-decl
        // deduped order. Diamond ancestors run exactly once (shared via
        // vbase ABI). Adjusts `this` to each ancestor's canonical
        // sub-object position so the ancestor body's field GEPs land
        // on the right slots. Stack-local drops never call
        // __cajeta_free, so unlike the heap wrapper there's no
        // double-free to guard against — we still walk only ancestor
        // user bodies (not their full drop wrappers) because we
        // already handled this class's own-field walk above and
        // ancestor field-walks for class-ref fields would be done by
        // each level's separately-emitted stack-drop if needed (but
        // stack class-ref fields are uncommon enough that the existing
        // own-field-only emission below covers the practical cases).
        for (auto& ancestor : collectDestructorChain()) {
            MethodPtr parentUserDrop;
            for (auto& entry : ancestor->methods) {
                MethodPtr m = entry.second;
                if (!m || m->isConstructor()) continue;
                if (m->getName() != "drop") continue;
                if (m->getParameterList().size() == 1) {
                    parentUserDrop = m;
                    break;
                }
            }
            if (!parentUserDrop || !parentUserDrop->getLlvmFunction()) {
                continue;
            }
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
            llvm::Function* fn = CajetaModule::ensureFunctionInModule(
                lmod, parentUserDrop->getLlvmFunction());
            b.CreateCall(parentUserDrop->getLlvmFunctionType(),
                fn, {ancestorThis});
        }

        // Walk owned class-ref fields in REVERSE declaration order. For
        // each plain class-ref (not aggregate, not array, not interface),
        // GEP the slot, load the pointer, call the referent class's
        // own heap-drop fn. (Pre-unified-class history: this mirrored
        // the auto-walk-owned-fields behavior CajetaStruct's drop had
        // today, generalized to any class allocated on the stack.
        std::vector<StructurePropertyPtr> reversed(
            propertyList.begin(), propertyList.end());
        std::reverse(reversed.begin(), reversed.end());
        for (auto& property : reversed) {
            auto fieldType = property->getType();
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType);
            if (!fieldClass) continue;
            if (dynamic_pointer_cast<CajetaView>(fieldType)) continue;
            if (dynamic_pointer_cast<CajetaArray>(fieldType)) continue;
            if (fieldClass->isInterface()) continue;

            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);
            llvm::Value* slotPtr = b.CreateStructGEP(
                llvmType, instance, fieldIdx,
                std::string("stack_drop_field_") + property->getName());
            llvm::Value* refPtr = b.CreateLoad(ptrTy, slotPtr);

            llvm::Function* refDrop = fieldClass->getOrCreateDropFunction();
            if (!refDrop) continue;
            refDrop = CajetaModule::ensureFunctionInModule(lmod, refDrop);
            b.CreateCall(refDrop, {refPtr});
        }

        // (Historical recurse-into-embedded-struct-fields path retired
        // with CajetaStruct under the unified-class model — embedded
        // class-ref fields above already handle reachable owned state.)

        // No __cajeta_free — stack body is reclaimed by the function
        // epilogue. This is the only structural difference from
        // getOrCreateDropFunction.

        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmStackDropFunction;
    }

    std::vector<CajetaClassPtr> CajetaClass::collectDestructorChain() {
        // Reverse DFS over direct parents in reverse declaration order,
        // skipping interfaces and ancestors already visited. Implements
        // the doctrine in MemoryModel.md § 140: "direct parent
        // destructors in reverse declaration order" with diamond dedup
        // (each shared ancestor runs exactly once).
        //
        // Example: D extends B, C; both B and C extend A.
        //   D.superClasses = [B, C]; iterate reversed → [C, B].
        //   Visit C → add. Recurse into C's parents (reverse) = [A] →
        //     add A. Recurse: A has no parents.
        //   Visit B → add. Recurse into B's parents (reverse) = [A] →
        //     A already visited, skip.
        //   Result: [C, A, B] — A runs once.
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

    void CajetaClass::emitDropBodyInline(llvm::IRBuilder<>& b,
                                          llvm::Value* instance,
                                          CajetaModulePtr cajModule) {
        // (1) User-written destructor body. Looked up directly (not via
        // resolveMethod) — overload resolution hasn't run at the point
        // drop wrappers get synthesized.
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
                cajModule->getLlvmModule(), userDrop->getLlvmFunction());
            b.CreateCall(userDrop->getLlvmFunctionType(), fn, {instance});
        }

        // (2) Own-field auto-drops in REVERSE declaration order.
        // propertyList is OWN fields only (addProperty is the only
        // populator; inherited fields are accessed via sub-object
        // layout, not stored here). Each ancestor's drop_body
        // contribution drops its OWN propertyList — derived classes
        // don't double-drop inherited fields.
        //
        // Per-field-type emission mirrors FieldOwnership.md § Solution B:
        // arrays → __cajeta_free_array, interfaces → __cajeta_iface_drop,
        // plain class-refs → __cajeta_class_virtual_drop. Primitives /
        // pointers / function-typed fields don't need drops.
        auto& ctx = *cajModule->getLlvmContext();
        auto* lmod = cajModule->getLlvmModule();
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
            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);

            if (dynamic_pointer_cast<CajetaArray>(fieldType)) {
                if (!freeArrayFn) {
                    freeArrayFn = cajModule->getRuntimeFunction("__cajeta_free_array");
                }
                if (!freeArrayFn) continue;
                llvm::Value* slot = b.CreateStructGEP(
                    llvmType, instance, fieldIdx,
                    std::string("drop_arr_slot_") + property->getName());
                llvm::Value* arrPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_arr_ptr_") + property->getName());
                b.CreateCall(freeArrayFn, {arrPtr});
                continue;
            }

            if (auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType)) {
                if (dynamic_pointer_cast<CajetaView>(fieldType)) continue;
                if (fieldClass->isInterface()) {
                    if (!ifaceDropFn) {
                        ifaceDropFn = cajModule->getRuntimeFunction("__cajeta_iface_drop");
                    }
                    if (!ifaceDropFn) continue;
                    llvm::Value* bodyPtr = b.CreateStructGEP(
                        llvmType, instance, fieldIdx,
                        std::string("drop_iface_body_") + property->getName());
                    b.CreateCall(ifaceDropFn, {bodyPtr});
                    continue;
                }
                if (!fieldClass->hasVtablePointerAtSlotZero()) continue;
                if (!virtualDropFn) {
                    virtualDropFn = cajModule->getRuntimeFunction(
                        "__cajeta_class_virtual_drop");
                }
                if (!virtualDropFn) continue;
                fieldClass->patchVirtualTableDropFn();
                llvm::Value* slot = b.CreateStructGEP(
                    llvmType, instance, fieldIdx,
                    std::string("drop_ref_slot_") + property->getName());
                llvm::Value* refPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_ref_ptr_") + property->getName());
                b.CreateCall(virtualDropFn, {refPtr});
                continue;
            }
        }
    }

    llvm::Function* CajetaClass::getOrCreateDropFunction() {
        if (llvmDropFunction) return llvmDropFunction;
        if (interfaceFlag) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);

        // Build the symbol name from the canonical class name with
        // separators sanitized to underscores so it's a valid C
        // identifier (and reads sensibly in stack traces).
        std::string dropName = std::string("__cajeta_") + qName->toCanonical() + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }

        // If the same JIT module has built this drop fn before (e.g. a
        // template instantiation revisited), reuse it.
        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmDropFunction = existing;
            return existing;
        }

        llvmDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::ExternalLinkage, dropName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* instance = llvmDropFunction->getArg(0);

        // Null guard. The drop chain only ever fires on values pushed
        // by __cajeta_drop_push, but a user-declared `drop()` method
        // might run inside another drop (chain teardown) on a borrowed
        // null receiver. Cheap to add, prevents a crash on the unhappy
        // path.
        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(instance,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);

        b.SetInsertPoint(doDrop);

        // C++ destruction semantics (MemoryModel.md § 140):
        //   (1) this class's user ~Class() body + own field auto-drops,
        //   (2) every transitive ancestor in reverse-DFS deduped order,
        //       each contributing its own user body + own field drops,
        //   (3) __cajeta_free(instance) — runs ONCE at the most-derived
        //       wrapper, not in any of the ancestor drop-body inlines.
        //
        // The deduped ancestor walk is the diamond fix: shared ancestors
        // (which the vbase ABI represents as a single sub-object) get
        // destructed exactly once. Iteration order: collectDestructorChain
        // does reverse DFS over direct parents in reverse declaration
        // order, skipping interfaces and already-visited ancestors.
        emitDropBodyInline(b, instance, module);

        for (auto& ancestor : collectDestructorChain()) {
            // Adjust `this` to the ancestor's canonical sub-object
            // position. First-parent ancestors (offset 0) need no GEP.
            // Multi-inheritance non-first parents and diamond vbases
            // each have their canonical position in subObjectSlotMap.
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

        // Free the heap allocation. Reuses __cajeta_free from the
        // closure-drop runtime; all generic heap blocks share one
        // free symbol.
        llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
        if (freeFn) {
            b.CreateCall(freeFn, {instance});
        }

        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmDropFunction;
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
            if (found) continue;
            // Cross-module / forward-reference fallback: consult the
            // process-global canonicalMap. The parent might be in a
            // different module's structures, or it might still be a
            // placeholder created by fromContext. Picking up the
            // placeholder here keeps superClasses non-empty so
            // tryGeneratePrototype can spot the placeholder and defer
            // until the parent fills in.
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
            // Truly unresolved (no archive entry, no placeholder) parents
            // silently skip today; CajetaModule::validatePlaceholders
            // catches the post-parse case where a placeholder was created
            // but never filled in.
        }
    }

    bool CajetaClass::tryGeneratePrototype() {
        if (prototypeBuilt) return true;
        // Resolve parents first so we can inspect them. resolveSuperClasses
        // is idempotent and reflects whatever canonicalMap shows now.
        resolveSuperClasses();
        resolveImplementedInterfaces();
        // Defer until every superclass has its struct laid out. A
        // placeholder isn't enough — an already-filled-in but
        // not-yet-prototyped parent's propertyList might still be empty
        // OR the inheritance chain above it might not be built, which
        // would leave OUR layout missing inherited fields. Wait until
        // the parent's prototypeBuilt latches true (which only happens
        // at the END of generatePrototype, after the parent's full
        // ancestor walk has run).
        for (auto& parent : superClasses) {
            if (!parent) continue;
            // Templates (uninstantiated) never set prototypeBuilt — they
            // short-circuit in generatePrototype without laying out a
            // struct. Treat them as "done" so a templated subclass like
            // `class List<T> extends Container<T>` doesn't defer forever
            // waiting on its template parent.
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
        generatePrototype();
        return true;
    }

    void CajetaClass::resolveImplementedInterfaces() {
        // Mirrors resolveSuperClasses but for `implements I1, I2`. Each name
        // is looked up first in the module's structures map, then in the
        // global canonical type registry so cross-module interfaces (e.g. a
        // user class implementing a stdlib interface like
        // cajeta.hash.Hasher) resolve correctly. Entries flagged
        // isInterface() are pushed into implementedInterfaces. Non-interface
        // names in the implements list are silently skipped today (a future
        // version should raise CAJETA_ERROR_NOT_AN_INTERFACE).
        implementedInterfaces.clear();
        auto& structures = module->getStructures();
        // qImplementedTypeArgs is parallel to qImplemented (one entry
        // per implements clause). For `implements Codec<int32>`, the
        // outer list has one entry `{int32}`. We walk both side-by-side
        // so we can swap the bare-template lookup for an instantiated
        // interface when type args are present.
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
                // Cross-module fallback: the interface may live in stdlib
                // or in a sibling user module. CajetaType::getCanonicalMap
                // is the global type registry that every parsed
                // CajetaClass / CajetaInterface registers itself in
                // (CajetaClass::generatePrototype line ~485 + the
                // CajetaLlvmVisitor onInterfaceDeclaration hook), so a
                // by-canonical lookup there finds it regardless of which
                // module declared it.
                auto& canonMap = CajetaType::getCanonicalMap();
                auto cit = canonMap.find(qn->toCanonical());
                if (cit != canonMap.end()) {
                    found = std::dynamic_pointer_cast<CajetaClass>(cit->second);
                }
                if (!found) {
                    // Short-name fallback: user wrote `implements Hasher`
                    // and the import resolution left qn with just
                    // "Hasher". Walk the global map looking for a
                    // matching typeName (the package-stripped tail).
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
            // When `implements Codec<int32>` brings type arguments,
            // the found CajetaClass at this point is still the bare
            // template `Codec`. Instantiate it with the per-clause
            // type args so the implementer's `implementedInterfaces`
            // points at a real `Codec<int32>` whose method list reflects
            // T → int32. Without this step the vtable walk below sees
            // a method-less template and silently drops the entry.
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
        virtualSlotHashList.clear();
        map<string, MethodPtr> uniqueByCanonical;

        // Override detection: a child's override has a DIFFERENT canonical
        // than the parent's method because canonical includes `parent::`
        // (e.g. `test.Animal::speak()` vs `test.Dog::speak()`). To make
        // overrides replace the parent's vtable slot, we match by SUFFIX
        // (name + params, stripping the leading `parent::`). When the
        // suffix is already in `uniqueByCanonical`, remove the existing
        // entry (the parent's) and insert the child's keyed by the
        // child's canonical. Result: dispatch by either the parent's or
        // child's canonical-hash lands on the child's method.
        auto suffixOf = [](const string& canon) -> string {
            auto pos = canon.rfind("::");
            return (pos == string::npos) ? canon : canon.substr(pos + 2);
        };

        // MultiClassing R-3 (cajeta-docs/stdlib/MultiClassing.md): when
        // a method on THIS class carries @Override(from=X), verify X is
        // an ancestor of this class AND X declares a same-suffix
        // method. Both the identifier form (`from=B`) and class-literal
        // form (`from=B.class`) are accepted — the visitor classifies
        // them as String and ClassRef respectively, so the lookup
        // checks both kinds.
        //
        // The check fires here (before the main walk + alias walk) so
        // it doesn't get tangled with override resolution. It's a
        // documentation+verification annotation; absence of @Override
        // or absence of `from=` is fine and skips the check.
        for (auto& m : methodList) {
            if (!m) continue;
            auto overrideAnn = m->findAnnotation("Override");
            if (!overrideAnn) continue;
            std::string fromName = overrideAnn->getClassRef("from");
            if (fromName.empty()) fromName = overrideAnn->getString("from");
            if (fromName.empty()) continue;
            // Resolve `fromName` against the canonical map (short or
            // canonical name both accepted), then walk this class's
            // ancestor chain to verify it's actually an ancestor.
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
            // Check that fromClass declares a method whose suffix matches
            // ours (same name + same parameter types).
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
                if (m->isAbstract()) continue;   // interface markers don't sit here
                if (m->isMethodTemplate()) continue;  // templated methods are non-virtual (no vtable slot)
                string canon = m->toCanonical(/*labeled=*/false);
                string suffix = suffixOf(canon);
                // Find + replace any existing entry with the same suffix
                // (the parent's method this overrides).
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

        // MultiClassing Phase 1 (P-1, cajeta-docs/stdlib/MultiClassing.md):
        // detect collisions between sibling parents BEFORE the alias walk
        // hides them. The pre-existing walk above is last-write-wins by
        // declaration order (B silently shadows A when both are siblings
        // of this class); without this check, `c.kind()` would dispatch
        // to B with no diagnostic. Two distinct ambiguity flavors and a
        // separate return-type-collision flavor are all raised here.
        //
        // The check walks the full ancestor closure and gathers, per
        // suffix, the set of distinct declaring classes that contribute
        // a concrete method body. A class with method `step()` declared
        // on two siblings (neither is ancestor of the other) and no
        // override on the current class is structurally ambiguous.
        {
            // suffix → list of (declaringClass, methodPtr) across all
            // ancestors AND self. Self-entries shadow ancestor entries
            // and resolve the ambiguity.
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

            // "is `anc` an ancestor of `desc`?" — walks desc's super
            // chain looking for anc. Self-equal counts as ancestor so
            // a self-declared method always wins over inherited ones.
            std::function<bool(CajetaClassPtr, CajetaClassPtr)> isAncestor =
                [&](CajetaClassPtr anc, CajetaClassPtr desc) -> bool {
                    if (!anc || !desc) return false;
                    if (anc.get() == desc.get()) return true;
                    for (auto& sup : desc->getSuperClasses()) {
                        if (isAncestor(anc, sup)) return true;
                    }
                    return false;
                };

            // Return-type canonical for comparison. Null returns
            // (void) get a distinct sentinel so void-vs-int conflicts
            // are detected.
            auto retCanon = [](const MethodPtr& m) -> std::string {
                auto rt = m->getReturnType();
                if (!rt) return std::string("<void>");
                return rt->toCanonical();
            };
            // Two return types are compatible if either canonical
            // matches OR both are CajetaClass instances and one is
            // the ancestor of the other (covariant override). The
            // narrower type can flow into the wider via implicit
            // upcast at the call site.
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

                // Return-type collision check — applies regardless of
                // override, because the vtable slot's signature has to
                // be one thing and conflicting return types make any
                // call structurally broken. Covariant overrides
                // (subclass narrows the return type) are allowed.
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

                // Method ambiguity check — fires only when there's no
                // self override and the contributing classes include
                // two siblings (no inheritance relationship between
                // them). Abstract-only declarations on a side are
                // treated as obligations rather than impls, so they
                // don't create ambiguity by themselves.
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

                // Find any pair (a, b) where neither is ancestor of
                // the other — that's a sibling collision.
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

        // Also re-key the resulting entries so a dispatch using the
        // BASE class's canonical-hash also finds the override. Build a
        // parallel map keyed by suffix; for each entry, walk all its
        // superclasses' equivalent (same-suffix) methods and re-publish
        // under THEIR canonical hashes pointing at the same impl.
        {
            map<string, MethodPtr> bySuffix;
            for (auto& [canon, m] : uniqueByCanonical) {
                bySuffix[suffixOf(canon)] = m;
            }
            // Walk superclasses and add entries under any base-canonical
            // that has a matching suffix. Multiple superclasses with the
            // same-suffix method all alias to the most-derived impl.
            //
            // MultiClassing P-5 fix: abstract methods are NOT skipped
            // here. When A declares `abstract step()` and a sibling B
            // declares concrete `step()`, the doc rule says B's impl
            // satisfies A's obligation in `C extends A, B`. The
            // abstract-obligation check below already accepts this
            // class. But dispatch can still happen against A.step's
            // canonical (e.g., when method resolution walks A's chain
            // first), so A.step's canonical must also alias to B's
            // concrete impl. Pre-fix the abstract-skip kept the
            // canonical out of the vtable and `c.step()` aborted at
            // dispatch with no entry for the looked-up hash.
            std::function<void(CajetaClassPtr)> aliasWalk = [&](CajetaClassPtr c) {
                for (auto& sup : c->getSuperClasses()) {
                    for (auto& m : sup->getMethodList()) {
                        if (m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                        if (m->isMethodTemplate()) continue;
                        string supCanon = m->toCanonical(/*labeled=*/false);
                        auto it = bySuffix.find(suffixOf(supCanon));
                        if (it != bySuffix.end()) {
                            // Add alias entry under the parent's canonical
                            // pointing at the most-derived impl.
                            uniqueByCanonical[supCanon] = it->second;
                        }
                    }
                    aliasWalk(sup);
                }
            };
            aliasWalk(static_pointer_cast<CajetaClass>(shared_from_this()));
        }

        // Interface dispatch: for each implemented interface method, find
        // the class's matching concrete method (by short name + param
        // signature) and register it in the vtable under the *interface*
        // method's canonical. The receiver's static type at the call site
        // determines which hash gets used — a call through an interface-
        // typed variable uses the interface's canonical and lands on the
        // entry we add here.
        auto findConcreteFor = [&](MethodPtr abstractM) -> MethodPtr {
            string targetSig = abstractM->toCanonical(/*labeled=*/false);
            // Strip the leading `parent::` (anything before the last "::")
            // so we match by method name + params only.
            auto pos = targetSig.rfind("::");
            string suffix = (pos == string::npos) ? targetSig : targetSig.substr(pos + 2);
            for (auto& [canon, m] : uniqueByCanonical) {
                auto p = canon.rfind("::");
                if (p == string::npos) continue;
                if (canon.substr(p + 2) == suffix) return m;
            }
            return nullptr;
        };
        // Walk implementedInterfaces transitively: each implemented
        // interface contributes its OWN method obligations, AND its
        // extends chain (parent interfaces) contributes theirs. Without
        // the transitive walk, `class C implements Codec<int32>` where
        // `interface Codec<T> extends Reader<T>` would not register a
        // vtable entry for `Reader<int32>::read`, so dispatch through
        // a Reader<int32>-typed reference would fail at runtime.
        std::set<CajetaClass*> visitedIfaces;
        std::function<void(CajetaClassPtr)> walkIface = [&](CajetaClassPtr iface) {
            if (!iface) return;
            if (!visitedIfaces.insert(iface.get()).second) return;
            for (auto& m : iface->getMethodList()) {
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                if (auto concrete = findConcreteFor(m)) {
                    uniqueByCanonical[m->toCanonical(/*labeled=*/false)] = concrete;
                    continue;
                }
                std::string msg = "class '" + qName->toCanonical()
                    + "' implements interface '" + iface->getQName()->toCanonical()
                    + "' but does not provide '" + m->toCanonical(/*labeled=*/false)
                    + "'";
                throw Exception(msg, "CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED");
            }
            // Recurse through interface-extends-interface chain.
            for (auto& parent : iface->getSuperClasses()) {
                if (parent && parent->isInterface()) walkIface(parent);
            }
        };
        for (auto& iface : implementedInterfaces) {
            walkIface(iface);
        }

        // Abstract-method enforcement: every abstract method declared on
        // any ancestor must have a concrete override on the implementing
        // chain. Without this, `class Bad extends Base` where Base has
        // `abstract int32 must()` and Bad has no override would compile,
        // and `new Bad().must()` would dispatch to a missing vtable slot
        // and crash at runtime.
        //
        // Heuristic for "this class is itself abstract" until class-level
        // abstract modifier tracking lands: if THIS class declares its
        // own abstract method, treat the class as abstract and skip the
        // enforcement (it's intentionally incomplete; subclasses are
        // expected to fill the gap and they'll get re-checked). Same
        // rationale Java has — abstract classes are allowed to have
        // unimplemented abstract methods; concrete classes are not.
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
            virtualSlotHashList.push_back(hash);
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

    // Subtype distance between an arg type and a method's declared param
    // type. Returns 0 when types match exactly, 1+ when `argType` derives
    // from `declaredType` (one per inheritance hop), -1 when there's no
    // assignable relationship. Used by the subtype-aware fallback in
    // resolveMethod so an `ArrayStream<int32>` arg can bind to a
    // `Stream<int32>` param.
    //
    // Non-class types (primitives, arrays, etc.) only match by exact
    // canonical-name equality — no upcast walk. Class types try identity
    // first, then BFS up `superClasses`.
    static int subtypeDistance(CajetaTypePtr declaredType, CajetaTypePtr argType) {
        if (!declaredType || !argType) return -1;
        if (declaredType->getQName() && argType->getQName()
                && declaredType->getQName()->toCanonical()
                    == argType->getQName()->toCanonical()) {
            return 0;
        }
        auto argClass = dynamic_pointer_cast<CajetaClass>(argType);
        auto declaredClass = dynamic_pointer_cast<CajetaClass>(declaredType);
        if (!argClass || !declaredClass) return -1;
        // BFS up the hierarchy from argClass; return the shallowest hop
        // count to declaredClass. Walks both the extends chain
        // (getSuperClasses) AND the implements chain
        // (getImplementedInterfaces) at each level. Without the
        // implements walk, `class FooImpl implements IFoo` won't
        // match an `IFoo` formal — passing a FooImpl to a function
        // taking IFoo gets rejected by resolveMethod as a no-match.
        // Templated stdlib classes can have diamond shapes (a class
        // and an interface both extending Stream<T>), hence BFS
        // rather than first-match DFS.
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

    // Scan a method bucket for a positional-args match where each arg is
    // type-compatible (same or subclass) with the declared param. Returns
    // the method whose cumulative subtype distance is smallest — i.e. the
    // most specific override. Skips the implicit `this` parameter the
    // ctor-detection logic doesn't strip.
    static MethodPtr findSubtypeMatch(
            const map<string, map<string, MethodPtr>>& genericMap,
            const string& methodName,
            const vector<ParameterEntry>& parameters) {
        MethodPtr best;
        int bestScore = std::numeric_limits<int>::max();
        const size_t argCount = parameters.size();
        for (auto& bucket : genericMap) {
            for (auto& entry : bucket.second) {
                MethodPtr method = entry.second;
                if (!method) continue;
                // Method name match required — the genericMap aggregates
                // every method on the class, so without this filter a
                // `count()` call could pick up a 0-arg `next()` method
                // and trip the JIT verifier when their return types
                // disagree.
                if (method->getName() != methodName) continue;
                // Method::generatePrototype injects `this` at position 0
                // for instance methods; the caller's parameters list never
                // includes `this`, so skip it when present. Stays robust if
                // generatePrototype hasn't run yet (registered key was
                // built without `this`).
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
                        parameters[i].type);
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

    // Unify one (formal, arg) pair against the bindings map. Recurses
    // into function types (`(R, T) -> R` formals must walk their param
    // + return types). Returns false on contradiction (same T-var bound
    // to two distinct concrete types), true otherwise. Bindings is
    // updated in place when a new T-var name is matched.
    //
    // Non-placeholder formals are *presumed compatible* — the post-
    // instantiation type-check catches anything wrong. The unifier's
    // only job is to discover T-var bindings.
    //
    // void-arg leniency: a lambda body without scope context resolves
    // its return type to void when the actual return-expression type
    // can't be inferred (LambdaExpression::resolveTypes falls back to
    // void when body->resolvedType is null). When unifying a T-var
    // formal against a void arg, prefer the existing binding (or skip
    // if unbound) rather than rejecting — the actual lambda body will
    // produce the right type at codegen time and the bound R drives
    // the monomorphization.
    static bool unifyMethodTemplateFormal(
        CajetaTypePtr formal, CajetaTypePtr arg,
        const std::set<std::string>& tparamNames,
        std::map<std::string, CajetaTypePtr>& bindings) {
        if (!formal || !arg) return false;

        // Placeholder T-var match — bind (or check consistency).
        if (auto fc = std::dynamic_pointer_cast<CajetaClass>(formal)) {
            if (fc->isPlaceholder()
                    && tparamNames.count(fc->getQName()->getTypeName())) {
                const std::string& name = fc->getQName()->getTypeName();
                auto existing = bindings.find(name);
                bool argIsVoid = (arg->toCanonical() == "cajeta.void"
                    || arg->toCanonical() == "void");
                if (existing == bindings.end()) {
                    // First binding: skip if the arg-side is the void
                    // fallback (no real info). Wait for a more
                    // informative arg (e.g. the seed in fold<R>).
                    if (argIsVoid) return true;
                    bindings[name] = arg;
                    return true;
                }
                // Already bound. First binding wins — defer to it. Lambda
                // return-type inference (done in LambdaExpression::
                // resolveTypes without the lambda's parameter scope
                // active) produces a wrong/narrower type often enough
                // that re-checking the second binding causes false
                // rejections, e.g. `fold(0, (int32 acc, Counter c) ->
                // acc + c.v)` reports the lambda return as Counter
                // (because BinaryOp resolves to the RHS type when the
                // LHS scope-bound type is unknown), conflicting with
                // R=int32 set by the seed. The actual lambda body
                // produces the right type at codegen time and the bound
                // R drives monomorphization correctly. Hard conflicts
                // surface at codegen / verify rather than here.
                return true;
            }
            // Class-template instantiation formal — recurse into the
            // type arguments so T-vars embedded inside `Collector<T, R>`,
            // `Stream<T>`, etc. bind from the corresponding positions in
            // the arg's instantiation. Both sides must be instantiations
            // of the same template; if not, presume compatibility and
            // let codegen surface mismatches.
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
            // Non-placeholder concrete class formal — presumed compatible.
            return true;
        }

        // Function-typed formal — recurse into params + return so T-vars
        // appearing inside `(R, T) -> R` get bound from the lambda arg's
        // signature. The lambda's resolved type must also be a function
        // type (its parameter types come from the explicit annotations
        // in the lambda expression).
        if (auto ffn = std::dynamic_pointer_cast<CajetaFunctionType>(formal)) {
            auto afn = std::dynamic_pointer_cast<CajetaFunctionType>(arg);
            if (!afn) return true;  // arg isn't a fn — fall through, type-check catches it
            const auto& fps = ffn->getParameterTypes();
            const auto& aps = afn->getParameterTypes();
            if (fps.size() != aps.size()) return true;  // arity mismatch — same fallthrough
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

        // Other type shapes (primitive, array, struct, view, interface):
        // not currently walked for T-var placeholders. Presumed compatible.
        return true;
    }

    // Try to match a method-template candidate by name + arg arity, unify
    // method-level T-vars against the supplied arg types, and return a
    // freshly instantiated concrete Method. Returns nullptr if no template
    // candidate exists, the arity differs, or unification fails. Walks the
    // class's own methodList only — parent walks happen at the caller.
    //
    // Implemented here rather than as a method-template free function so
    // CajetaClass owns the per-class instantiation cache via the Method's
    // own cache.
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

            // Explicit-type-args path: `expr.<T1, T2>method(...)`. The
            // arity must match the declared method-level type parameters;
            // we hand them straight to instantiateMethodTemplate, bypassing
            // unification entirely. This is what lets call sites where
            // inference can't reach a binding (no value args, or
            // ambiguous lambda return) still resolve.
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
            // Fallback: if the formal's class template arguments got
            // stripped at parse time (a class-template formal like
            // `Collector<T, R>` sometimes loses its type args, leaving
            // a placeholder-shaped `Collector` that can't bind R via
            // recursion), try a second unification pass that uses the
            // ARG's type-argument list directly. This is positional
            // and presumes one-to-one correspondence with the method's
            // declared type parameters when the formal contributes no
            // bindings on its own — narrow enough to skip when any
            // T-var did bind via the structural walk.
            if (bindings.size() < tparams.size()) {
                for (size_t i = 0; i < formals.size(); ++i) {
                    auto fc = std::dynamic_pointer_cast<CajetaClass>(
                        formals[i]->getType());
                    auto ac = std::dynamic_pointer_cast<CajetaClass>(
                        parameters[i].type);
                    if (!fc || !ac) continue;
                    if (!fc->getTypeArguments().empty()) continue;
                    if (ac->getTypeArguments().empty()) continue;
                    // Positionally project the arg's type-args onto
                    // any unbound method tparams in declaration order.
                    // The check below is conservative: only fires when
                    // exactly one tparam is unbound and the arg supplies
                    // a single type-arg that hasn't been seen yet.
                    const auto& aArgs = ac->getTypeArguments();
                    if (aArgs.empty()) continue;
                    for (auto& tp : tparams) {
                        if (bindings.count(tp.name)) continue;
                        // Pick the last arg (most-specific tail slot —
                        // class-level T-vars take the prefix slots, the
                        // method-level R-vars take the tail).
                        bindings[tp.name] = aArgs.back();
                        break;
                    }
                    if (bindings.size() >= tparams.size()) break;
                }
            }
            // All declared T-vars must be bound (no leftover unbound R).
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

    // Shared helper: register a fresh method-template instantiation on
    // its host class, generate its prototype + body, and restore the
    // module's builder / currentMethod state around the body codegen
    // (which mutates those globals). Without the save/restore, the
    // outer method body that triggered the instantiation would emit
    // into the wrong insert point afterward.
    //
    // Idempotent on re-entry: instantiateMethodTemplate caches per-arg,
    // so a second call site requesting the same instantiation gets the
    // same MethodPtr back. Skip addMethod (and the codegen calls below)
    // when the instantiation is already registered — addMethod's
    // duplicate-static check otherwise rejects.
    static void bringMethodTemplateInstantiationToLife(
            CajetaClass* host, MethodPtr inst) {
        // Probe with the two-layer key so distinct instantiations of a
        // same-canonical template (T-vars not in value params) each
        // get their own registration. See Method::getMapKey.
        if (host->getMethods().find(inst->getMapKey()) != host->getMethods().end()) {
            return;  // already registered + emitted on a prior call
        }
        host->addMethod(inst);
        auto hostMod = inst->getModule();
        llvm::IRBuilder<>* savedBuilder = hostMod ? hostMod->getBuilder() : nullptr;
        MethodPtr savedCurrent = hostMod ? hostMod->getCurrentMethod() : nullptr;
        llvm::BasicBlock* savedInsertBB = savedBuilder
            ? savedBuilder->GetInsertBlock() : nullptr;
        // Scope-stack barrier — the inner method's resolveTypes pass
        // must NOT find the caller's locals via the parent chain.
        // Without this save/clear, e.g. a nested-class JSON synth call
        // sees the outer method's `out` (different type) when looking
        // up its own `out` before its LocalVariableDeclaration runs
        // at codegen — and pins a wrong resolvedType on its
        // IdentifierExpression AST nodes. See JsonSynthesizer.cpp's
        // nested-class field arm and the parseObjectFromReaderT body
        // for the originally-affected shape.
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
        if (savedBuilder && savedInsertBB) {
            savedBuilder->SetInsertPoint(savedInsertBB);
        }
    }

    MethodPtr CajetaClass::resolveMethod(string& methodName, vector<ParameterEntry>& parameters,
            bool isConstructor, bool floatingParams,
            const vector<CajetaTypePtr>& explicitMethodTypeArgs) {
        // Explicit-type-args fast path: when the call site spells the
        // method-level type args (`expr.<T>method(args)`), skip exact-
        // signature lookup (which can't match — the explicit form is
        // exclusively a template-instantiation request) and go straight
        // to the templated-method scan with the explicit args.
        if (!isConstructor && !explicitMethodTypeArgs.empty()) {
            if (MethodPtr inst = tryInstantiateMethodTemplate(
                    this, methodName, parameters, explicitMethodTypeArgs)) {
                bringMethodTemplateInstantiationToLife(this, inst);
                return inst;
            }
            for (auto& parent : superClasses) {
                if (MethodPtr inst = tryInstantiateMethodTemplate(
                        parent.get(), methodName, parameters, explicitMethodTypeArgs)) {
                    bringMethodTemplateInstantiationToLife(parent.get(), inst);
                    return inst;
                }
            }
            return nullptr;
        }
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

        // Subtype-aware fallback. The exact generic-key lookup matches by
        // arg type canonicals; an `ArrayStream<int32>` arg misses a
        // `Stream<int32>` param even though the upcast is well-defined.
        // Scan every bucket positionally and pick the most specific match
        // (smallest cumulative inheritance distance). Only runs for the
        // *current* class; the parent walk below picks up inherited
        // methods via recursion, which itself triggers this fallback at
        // each level.
        if (MethodPtr m = findSubtypeMatch(*genericMap, methodName, parameters)) {
            return m;
        }

        // Constructors are NOT inherited; only walk parents for instance methods.
        if (!isConstructor) {
            for (auto& parent : superClasses) {
                MethodPtr m = parent->resolveMethod(methodName, parameters, isConstructor, floatingParams);
                if (m) return m;
            }
        }

        // Method-template fallback (cajeta-docs/stdlib/MethodLevelTemplate.md):
        // if no exact / subtype match was found, look for a method-templated
        // candidate with the same name and arity whose T-vars unify with the
        // supplied arg types. On a hit, instantiate the template into a
        // concrete Method (cached per arg list on the template), register
        // the instantiation in this class's method maps so subsequent calls
        // hit it directly, generate its prototype + body so the call site
        // can emit a direct call to a fully-defined function, and return.
        if (!isConstructor) {
            if (MethodPtr inst = tryInstantiateMethodTemplate(
                    this, methodName, parameters)) {
                bringMethodTemplateInstantiationToLife(this, inst);
                return inst;
            }
            // Walk the parent chain looking for templated candidates too.
            for (auto& parent : superClasses) {
                if (MethodPtr inst = tryInstantiateMethodTemplate(
                        parent.get(), methodName, parameters)) {
                    bringMethodTemplateInstantiationToLife(parent.get(), inst);
                    return inst;
                }
            }
        }
        return nullptr;
    }

    llvm::Value* CajetaClass::invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisValue,
                                            CajetaModulePtr callerModule, bool forceDirectCall,
                                            const vector<CajetaTypePtr>& explicitMethodTypeArgs) {
        bool floatingParams = true;
        for (auto &param : parameters) {
            if (param.label.empty()) {
                floatingParams &= false;
            }
        }

        if (floatingParams) {
            sort(parameters.begin(), parameters.end(), [](const ParameterEntry& a, const ParameterEntry& b) -> bool { return a.label < b.label; });
        }

        MethodPtr method = resolveMethod(methodName, parameters, isConstructor, floatingParams, explicitMethodTypeArgs);
        if (!method) {
            return nullptr;
        }
        // Visibility enforcement. Caller's class is the top of the
        // module's structure stack (Method::generateCode pushes the
        // enclosing class before emitting the body). The check is
        // skipped when the caller frame is empty (e.g. compiler-
        // synthesized free-function emission) — treating that as
        // "trusted" rather than "external" matches the intent that
        // synthesized code doesn't get blocked by user-facing
        // access rules. Same-class via `this` is always allowed
        // (callerCls == methodOwner). Cross-class private is
        // forbidden. Protected is allowed for same-class, same-
        // package, or descendant. Package is allowed for same
        // package. Public is unrestricted. Constructors are
        // exempt here — the ctor-staticName factory path and the
        // synthesized factory invoke ctors directly, and a more
        // nuanced ctor-access policy lives with the new() site.
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
                // Walk callerCls's superClasses chain to see whether
                // methodOwner appears anywhere upstream.
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
        // Method::generatePrototype injects `this` as the first parameter for non-static
        // methods; prepend the instance pointer here so the call's argument list matches.
        bool isStatic = method->getModifiers().find(STATIC) != method->getModifiers().end();
        vector<llvm::Value*> methodArgs;
        if (thisValue && !isStatic) {
            methodArgs.push_back(thisValue);
        }
        // The caller's module owns the IRBuilder we need to insert into.
        // Pre-refactor every CajetaModule re-parsed the stdlib, so the
        // receiver class's own pModule == the caller's module and
        // `this->module` was a safe fallback. With one shared stdlib
        // module, that fallback would emit cross-module calls into the
        // *stdlib* module (whose builder still points at whichever
        // stdlib method was generateCode'd last) — appending instructions
        // after a foreign function's terminator. Callers in user code
        // pass their own module; the fallback is preserved for sites
        // that haven't been threaded through yet.
        CajetaModulePtr emitMod = callerModule ? callerModule : module;
        // Coerce each arg to match the function's parameter type. Integer
        // literals default to i64, but the function may expect i32 / i8 / etc.
        // Without coercion the JIT verifier rejects the call as a type
        // mismatch. Use the function's parameter types as the source of truth.
        auto* coerceBuilder = emitMod->getBuilder();
        llvm::FunctionType* mft = method->getLlvmFunctionType();
        int thisOffset = (thisValue && !isStatic) ? 1 : 0;
        // L-03 polymorphism / MI upcast at parameter-passing site.
        // Mirrors the same shift LocalVariableDeclaration, assignment,
        // and return apply when binding a descendant pointer to an
        // ancestor-typed slot. The formal parameter's CajetaType comes
        // from method->getParameterList(); the implicit `this` slot
        // sits at index 0 for non-static methods, so user args start
        // at index `thisOffset` in the formal list.
        auto formalParams = method->getParameterList();
        for (int i = 0; i < (int) parameters.size(); i++) {
            llvm::Value* v = parameters[i].value;
            // Upcast adjust BEFORE the integer/FP coerce so we shift
            // the pointer before any other transformation.
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
                // Class → interface upcast at parameter-passing site.
                // The formal is an interface fat-pointer body
                // (`{ ptr data, ptr vtable, i64 kind }`); the actual arg
                // is a class instance pointer. Build the body alloca
                // here and pass its address as the arg. Mirrors the
                // same fat-pointer construction LocalVariableDeclaration
                // does when a class RHS initializes an interface
                // local (see LocalVariableDeclaration.cpp § S9.5.4).
                // Without this, resolveMethod admits the call (via the
                // implementedInterfaces walk in subtypeDistance) but
                // the callee dereferences a class ptr as if it were a
                // 24-byte interface body, reading garbage as the
                // vtable and crashing on dispatch.
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
            }
            if (mft && (int) mft->getNumParams() > i + thisOffset) {
                llvm::Type* expected = mft->getParamType(i + thisOffset);
                if (v && v->getType() != expected) {
                    if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                        v = coerceBuilder->CreateIntCast(v, expected, /*isSigned=*/true);
                    } else if (expected->isFloatingPointTy() && v->getType()->isFloatingPointTy()) {
                        v = coerceBuilder->CreateFPCast(v, expected);
                    }
                }
            }
            methodArgs.push_back(v);
        }

        // Dynamic dispatch via the receiver's vtable: hash the method's
        // canonical signature, load the receiver's vtable pointer (instance
        // slot 0), call __cajeta_vtable_lookup to find the function pointer,
        // and indirect-call it. Statics and constructors stay direct — they
        // don't participate in the vtable. The compile-time `method` is the
        // statically-resolved entry; the runtime hash-based lookup is what
        // actually picks the override-correct function in a subclass.
        auto* builder = emitMod->getBuilder();
        auto& llvmCtx = *emitMod->getLlvmContext();
        // Cross-module dispatch: when the receiver class lives in a
        // different llvm::Module than where the call is being
        // emitted (e.g. App's run() calling Provider's __cajeta_inject
        // after multi-source compile), use a module-local extern
        // declaration as the callee rather than a Function* whose
        // parent is a foreign module. ensureFunctionVisible returns
        // method->getLlvmFunction() unchanged when caller and target
        // are co-resident.
        //
        // Stale-pointer guard: in multi-classing scenarios a Method
        // object inherited from a parent's vtable can survive a
        // generatePrototype miss in the current compile — its
        // cached llvmFunction then points at a Function* whose
        // Module was destroyed. Reading `original->getParent()` in
        // ensureFunctionVisible would dereference freed memory and
        // SIGSEGV (intermittent, since ASLR randomizes whether the
        // page is still mapped). Side-step by looking up the
        // method's canonical name in the current module FIRST. If
        // present (generatePrototype already ran in this compile),
        // we have a guaranteed-fresh Function*. Else, declare it
        // in the current module via getOrInsertFunction so we
        // never have to dereference the possibly-stale pointer.
        llvm::Module* currentLm = emitMod->getLlvmModule();
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
        // Views have no vtable header (they are typed overlays onto
        // byte buffers; the byte buffer is the value). Methods on
        // views are statically dispatched — the receiver's concrete
        // type is known at the call site. Skip the vtable path
        // entirely; LLVM gets a direct call to the resolved method.
        // (Pre-unified-class history: this branch also covered
        // CajetaStruct; under the unified model `struct` is just
        // `class` and gets the normal vtable-dispatch treatment.)
        bool isView = dynamic_cast<CajetaView*>(this) != nullptr;
        // Method-level templated methods are non-virtual (templating
        // excludes them from the vtable per cajeta-docs/stdlib/
        // MethodLevelTemplate.md). Always direct-dispatch — the
        // concrete instantiation's LLVM function is the static target.
        bool isMethodTemplateInst = method->isMethodTemplateInstantiation();
        bool useVtable = thisValue && !isStatic && !isConstructor && !isView
            && !forceDirectCall && !isMethodTemplateInst;
        bool isInterfaceRecv = this->isInterface();
        // Interface formal whose resolved method lives on a CLASS
        // ancestor (e.g. `Splittable<T> extends Stream<T>` and we're
        // calling `next()` — declared on Stream<T>, not Splittable<T>).
        // The interface vtable doesn't hold class-method slots; instead,
        // load the dataPtr (real class instance) from the fat pointer
        // and do hash-based class-vtable dispatch through it.
        bool methodOnClassAncestor = isInterfaceRecv && method->getParent()
            && !method->getParent()->isInterface();
        if (useVtable && isInterfaceRecv && methodOnClassAncestor) {
            llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
            llvm::Type* bodyTy = this->getLlvmType();
            llvm::Value* dataSlot = builder->CreateStructGEP(
                bodyTy, thisValue, 0, "iface_data_slot");
            llvm::Value* dataPtr = builder->CreateLoad(
                ptrTy, dataSlot, "iface_data");
            // dataPtr is a class instance; word 0 is the class vtable.
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
            if (!methodArgs.empty()) {
                methodArgs[0] = dataPtr;
            }
        } else if (useVtable && isInterfaceRecv) {
            // (Historical S11.2 "same-concrete-type return through dyn
            // dispatch" rejection retired with CajetaStruct under the
            // unified-class model — the equivalent check for a class
            // implementer returning its own class type would never have
            // fired because class instances are pass-by-pointer
            // throughout, dodging the sret-slot problem this branch
            // guarded against.)

            // S9.5.6 — interface receiver via fat pointer body. Load the
            // per-(impl, iface) vtable from word 1 (the runtime-resolved
            // pointer set by the assignment site that built the fat
            // pointer); load the underlying data from word 0 and use it
            // as the actual `this` for the implementer's function. The
            // vtable is a flat [N x ptr] array in interface-declaration
            // order (per S9.2 / S9.5.2), so dispatch is a GEP-to-index
            // load — no hash lookup needed.
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

            // Method's index in the interface's flattened method list
            // (this iface's own methods first, then parent interfaces').
            // synthesizeInterfaceVTables emits vtable entries in this same
            // order, so the index here picks up an inherited method like
            // `Stream<T>.next()` when the formal is `Splittable<T>` and
            // `Splittable<T> extends Stream<T>`. Without the flattened
            // walk the lookup would miss and dispatch would fall through
            // to the abstract base.
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
                // +1 to skip the drop-fn slot at vtable[0] (S10.4).
                llvm::Value* methodSlot = builder->CreateInBoundsGEP(
                    ptrTy, vtablePtr,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx),
                        (uint64_t) (methodIdx + 1)),
                    "iface_method_slot");
                callee = builder->CreateLoad(ptrTy, methodSlot, "iface_method_fn");
            }

            // Swap the body pointer for the data pointer at arg position
            // 0 — the implementer's function expects its concrete class
            // instance as `this`, not the interface body.
            if (!methodArgs.empty()) {
                methodArgs[0] = dataPtr;
            }
        } else if (useVtable) {
            llvm::Function* lookupFn = emitMod->getRuntimeFunction("__cajeta_vtable_lookup");
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

        // Per-parent sub-object adjustment for the dispatched `this` arg
        // (Gap 8). When the resolved method is declared on an ancestor
        // whose sub-object lives at non-zero offset inside this receiver
        // class, shift methodArgs[0] to point at the ancestor's
        // sub-object — the parent's pre-compiled IR uses the parent's
        // own struct slot indices and expects a pointer into that view.
        // Skip when:
        //   - no `this` (statics) — methodArgs[0] is a real parameter
        //   - interface dispatch — already handled by the iface fat-pointer
        //     dataPtr swap above
        //   - declaring class == this — first-parent share or self
        if (thisValue && !isStatic && !isInterfaceRecv && method->getParent()) {
            const CajetaClass* declaring = method->getParent().get();
            if (declaring && declaring != this) {
                uint64_t off = this->getSubObjectByteOffset(declaring);
                if (off != 0 && !methodArgs.empty()) {
                    llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
                    methodArgs[0] = builder->CreateInBoundsGEP(
                        i8Ty, methodArgs[0],
                        llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(llvmCtx), off),
                        "subobj_this");
                }
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