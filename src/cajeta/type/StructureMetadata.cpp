//
// Created by James Klappenbach on 11/20/22.
//

#include "StructureMetadata.h"

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

namespace cajeta {

    // FNV-1a 64-bit over a canonical method signature, `#` skipped so dispatch is
    // mode-erased. Must stay in lockstep with CajetaClass.cpp's copy and the runtime's
    // __cajeta_signature_hash, or compile-time and dispatch-time hashes diverge.
    static int64_t signatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            if (c == '#') continue;
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }

    // ---- Fixed-layout RTTI emitters (REFL-1) --------------------------------
    llvm::Constant* StructureMetadata::emitCString(const std::string& s) {
        auto& ctx = *module->getLlvmContext();
        llvm::Constant* data = llvm::ConstantDataArray::getString(ctx, s, true);
        auto* g = new llvm::GlobalVariable(
            *module->getLlvmModule(), data->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, data, ".rtti.str");
        g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return g;
    }

    llvm::Constant* StructureMetadata::emitCStringArray(const vector<std::string>& strings) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        if (strings.empty()) {
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        }
        vector<llvm::Constant*> ptrs;
        for (auto& s : strings) ptrs.push_back(emitCString(s));
        llvm::ArrayType* arrTy = llvm::ArrayType::get(ptrTy, ptrs.size());
        auto* g = new llvm::GlobalVariable(
            *module->getLlvmModule(), arrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, ptrs), ".rtti.strs");
        g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return g;
    }

    int32_t StructureMetadata::packModifiers(const std::set<Modifier>& modifiers) {
        int32_t bits = 0;
        for (auto m : modifiers) bits |= (int32_t) m;
        return bits;
    }

    // One named descriptor struct each, shared by every class and kept in lock-step with
    // the C mirrors in cajeta_runtime.c (the per-class data lives in table globals).

    // #FieldDesc: { ptr name, ptr type, i32 modifiers, i16 annotationCount,
    //   ptr annotations, i32 byteOffset, i64 typeFlags }. byteOffset is the offset within
    // the instance struct (-1 for statics); typeFlags is the type's TYPE_ID flag word.
    llvm::StructType* StructureMetadata::getFieldStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#FieldDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, ptrTy, llvmInt32Type, llvmInt16Type, ptrTy, llvmInt32Type, llvmInt64Type},
            "cajeta.reflect.#FieldDesc");
    }
    // #ParameterDesc keeps the shared 5-field shape: name, type, modifiers, annotations.
    llvm::StructType* StructureMetadata::getParameterStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#ParameterDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, ptrTy, llvmInt32Type, llvmInt16Type, ptrTy}, "cajeta.reflect.#ParameterDesc");
    }
    // #MethodDesc: { ptr name, ptr returnType, i64 sigHash, i32 modifiers,
    //   i16 parameterCount, ptr parameters, i16 annotationCount, ptr annotations }.
    // The annotation pair is appended last so existing readers keep their offsets.
    llvm::StructType* StructureMetadata::getMethodStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#MethodDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, ptrTy, llvmInt64Type, llvmInt32Type, llvmInt16Type, ptrTy,
             llvmInt16Type, ptrTy},
            "cajeta.reflect.#MethodDesc");
    }
    // The #Rtti header, one fixed LLVM struct for every class; the slot list below is in
    // lock-step with CajetaRtti in cajeta_runtime.c. invokeAdapter / newInstanceAdapter are
    // forward-declared by populate and given bodies post-quiescence; either may be null.
    llvm::StructType* StructureMetadata::getRttiStructType() {
        if (llvmRttiType) return llvmRttiType;
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#Rtti")) {
            llvmRttiType = e; return e;
        }
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvmRttiType = llvm::StructType::create(ctx, {
            llvmInt64Type,  // 0  allocationSize
            ptrTy,          // 1  typeName
            llvmInt32Type,  // 2  modifiers
            llvmInt16Type,  // 3  classAnnotationCount
            ptrTy,          // 4  classAnnotations
            llvmInt16Type,  // 5  propertyCount
            ptrTy,          // 6  properties
            llvmInt16Type,  // 7  methodCount
            ptrTy,          // 8  methods
            llvmInt16Type,  // 9  parentCount
            ptrTy,          // 10 parentNames
            ptrTy,          // 11 vtable
            ptrTy,          // 12 invokeAdapter
            ptrTy,          // 13 newInstanceAdapter
            llvmInt16Type,  // 14 constructorCount
            ptrTy,          // 15 constructors (#MethodDesc[])
            llvmInt16Type,  // 16 templateParamCount (REFL-7)
            ptrTy,          // 17 templateParams (#TemplateParamDesc[])
            llvmInt16Type,  // 18 templateArgCount
            ptrTy,          // 19 templateArgs (i8*[] canonical names)
        }, "cajeta.reflect.#Rtti");
        return llvmRttiType;
    }

    // #AnnotationArgDesc: { ptr name, i32 kind, i64 i64Val, ptr strVal, i8 boolVal,
    //   i32 listCount, ptr listData }. `kind` mirrors AnnotationArgKind; the *List kinds
    // point listData at [N x i64], [N x i8*] or [N x i8], scalars leave it null.
    llvm::StructType* StructureMetadata::getAnnotationArgStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#AnnotationArgDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, llvmInt32Type, llvmInt64Type, ptrTy, llvmInt8Type,
             llvmInt32Type, ptrTy},
            "cajeta.reflect.#AnnotationArgDesc");
    }
    // #AnnotationDesc: { ptr name, i16 argCount, ptr args } — the shape every owner's
    // `annotations` pointer targets; args is null for an annotation with no arguments.
    llvm::StructType* StructureMetadata::getAnnotationStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#AnnotationDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, llvmInt16Type, ptrTy},
            "cajeta.reflect.#AnnotationDesc");
    }

    llvm::Constant* StructureMetadata::emitAnnotationArgArray(const vector<AnnotationArg>& args) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        if (args.empty())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* argTy = getAnnotationArgStructType();
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        vector<llvm::Constant*> rows;
        for (auto& a : args) {
            llvm::Constant* strConst =
                (a.kind == AnnotationArgKind::String || a.kind == AnnotationArgKind::ClassRef)
                    ? emitCString(a.strVal)
                    : nullPtr;
            int32_t listCount = 0;
            llvm::Constant* listData = nullPtr;
            if (a.kind == AnnotationArgKind::Int64List && !a.i64List.empty()) {
                listCount = (int32_t) a.i64List.size();
                vector<llvm::Constant*> elems;
                for (int64_t v : a.i64List)
                    elems.push_back(llvm::ConstantInt::get(llvmInt64Type, (uint64_t) v));
                llvm::ArrayType* at = llvm::ArrayType::get(llvmInt64Type, elems.size());
                listData = new llvm::GlobalVariable(*module->getLlvmModule(), at, true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(at, elems), ".rtti.annargi64");
            } else if (a.kind == AnnotationArgKind::BoolList && !a.boolList.empty()) {
                listCount = (int32_t) a.boolList.size();
                vector<llvm::Constant*> elems;
                for (bool v : a.boolList)
                    elems.push_back(llvm::ConstantInt::get(llvmInt8Type, v ? 1 : 0));
                llvm::ArrayType* at = llvm::ArrayType::get(llvmInt8Type, elems.size());
                listData = new llvm::GlobalVariable(*module->getLlvmModule(), at, true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantArray::get(at, elems), ".rtti.annargbool");
            } else if (a.kind == AnnotationArgKind::StringList && !a.strList.empty()) {
                listCount = (int32_t) a.strList.size();
                listData = emitCStringArray(a.strList);   // [N x i8*]
            }
            rows.push_back(llvm::ConstantStruct::get(argTy, {
                emitCString(a.name),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) (int32_t) a.kind),
                llvm::ConstantInt::get(llvmInt64Type, (uint64_t) a.i64Val),
                strConst,
                llvm::ConstantInt::get(llvmInt8Type, a.boolVal ? 1 : 0),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) (uint32_t) listCount),
                listData,
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(argTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.annargs");
    }

    llvm::Constant* StructureMetadata::emitAnnotationArray(
            const list<QualifiedNamePtr>& names,
            const vector<AnnotationInstancePtr>& instances) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        if (names.empty())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getAnnotationStructType();
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        // Annotations are not repeatable in v1, so the canonical name is a unique key.
        std::map<std::string, AnnotationInstancePtr> byCanonical;
        for (auto& inst : instances) {
            if (inst && inst->getName())
                byCanonical.emplace(inst->getName()->toCanonical(), inst);
        }
        vector<llvm::Constant*> rows;
        for (auto& q : names) {
            std::string canonical = q->toCanonical();
            llvm::Constant* argsPtr = nullPtr;
            size_t argCount = 0;
            auto it = byCanonical.find(canonical);
            if (it != byCanonical.end() && it->second) {
                const auto& args = it->second->getArgs();
                argCount = args.size();
                argsPtr = emitAnnotationArgArray(args);
            }
            rows.push_back(llvm::ConstantStruct::get(descTy, {
                emitCString(canonical),
                llvm::ConstantInt::get(llvmInt16Type, argCount),
                argsPtr,
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(descTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.anns");
    }

    // #TemplateParamDesc: { ptr name, i16 boundCount, ptr bounds, i8 isNonType,
    //   ptr nonTypePrimitive } — bounds are canonical names, nonTypePrimitive is set only
    // for a value parameter. Lock-step with CajetaTemplateParamDesc in cajeta_runtime.c.
    llvm::StructType* StructureMetadata::getTemplateParamStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#TemplateParamDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, llvmInt16Type, ptrTy, llvmInt8Type, ptrTy},
            "cajeta.reflect.#TemplateParamDesc");
    }

    llvm::Constant* StructureMetadata::emitTemplateParamTable(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& params = structure->getTypeParameters();
        if (params.empty())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getTemplateParamStructType();
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        vector<llvm::Constant*> rows;
        for (auto& p : params) {
            vector<std::string> bounds;
            for (auto& b : p.bounds) if (b) bounds.push_back(b->toCanonical());
            llvm::Constant* nonType = p.isNonType
                ? emitCString(p.nonTypePrimitive)
                : nullPtr;
            rows.push_back(llvm::ConstantStruct::get(descTy, {
                emitCString(p.name),
                llvm::ConstantInt::get(llvmInt16Type, bounds.size()),
                emitCStringArray(bounds),
                llvm::ConstantInt::get(llvmInt8Type, p.isNonType ? 1 : 0),
                nonType,
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(descTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.tparams");
    }

    llvm::Constant* StructureMetadata::emitTemplateArgArray(CajetaClassPtr structure) {
        vector<std::string> names;
        for (auto& a : structure->getTypeArguments())
            if (a) names.push_back(a->toCanonical());
        return emitCStringArray(names);
    }

    llvm::Constant* StructureMetadata::emitParameterTable(MethodPtr method) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& params = method->getParameterList();
        // Skip the implicit leading `this` so the list is the user-visible signature.
        size_t start = (!params.empty() && params.front()->getName() == "this") ? 1 : 0;
        if (params.size() <= start)
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getParameterStructType();
        vector<llvm::Constant*> rows;
        for (size_t pi = start; pi < params.size(); ++pi) {
            auto& p = params[pi];
            rows.push_back(llvm::ConstantStruct::get(descTy, {
                emitCString(p->getName()),
                emitCString(p->getType()->toCanonical()),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) packModifiers(p->getModifiers())),
                llvm::ConstantInt::get(llvmInt16Type, p->getAnnotationList().size()),
                emitAnnotationArray(p->getAnnotationList(), p->getAnnotationInstances()),
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(descTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.params");
    }

    llvm::Constant* StructureMetadata::emitFieldTable(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& props = structure->getPropertyList();
        if (props.empty())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getFieldStructType();
        auto* instStruct = llvm::dyn_cast_or_null<llvm::StructType>(structure->getLlvmType());
        const llvm::StructLayout* layout = instStruct
            ? module->getLlvmModule()->getDataLayout().getStructLayout(instStruct)
            : nullptr;
        vector<llvm::Constant*> rows;
        for (auto& p : props) {
            // -1 for a static field, or when the layout/index is unavailable.
            int32_t byteOffset = -1;
            int llvmIdx = structure->getFieldLlvmIndex(p);
            if (layout && llvmIdx >= 0 && (unsigned) llvmIdx < instStruct->getNumElements())
                byteOffset = (int32_t) layout->getElementOffset((unsigned) llvmIdx);
            uint64_t typeFlags = p->getType() ? (uint64_t) p->getType()->getTypeFlags() : 0;
            rows.push_back(llvm::ConstantStruct::get(descTy, {
                emitCString(p->getName()),
                emitCString(p->getType()->toCanonical()),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) packModifiers(p->getModifiers())),
                llvm::ConstantInt::get(llvmInt16Type, p->getAnnotationList().size()),
                emitAnnotationArray(p->getAnnotationList(), p->getAnnotationInstances()),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) (uint32_t) byteOffset),
                llvm::ConstantInt::get(llvmInt64Type, typeFlags),
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(descTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.fields");
    }

    llvm::Constant* StructureMetadata::emitMethodTable(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& methods = structure->getMethodList();
        if (methods.empty())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getMethodStructType();
        vector<llvm::Constant*> rows;
        for (auto& m : methods) {
            int64_t hash = signatureHash(m->toCanonical(/*labeled=*/false));
            const auto& mp = m->getParameterList();
            size_t userParams = mp.size();
            if (!mp.empty() && mp.front()->getName() == "this") userParams -= 1;
            rows.push_back(llvm::ConstantStruct::get(descTy, {
                emitCString(m->toCanonical()),
                emitCString(m->getReturnType()->toCanonical()),
                llvm::ConstantInt::get(llvmInt64Type, (uint64_t) hash),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) packModifiers(m->getModifiers())),
                llvm::ConstantInt::get(llvmInt16Type, userParams),
                emitParameterTable(m),
                llvm::ConstantInt::get(llvmInt16Type, m->getAnnotationList().size()),
                emitAnnotationArray(m->getAnnotationList(), m->getAnnotationInstances()),
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(descTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.methods");
    }

    llvm::Constant* StructureMetadata::emitConstructorTable(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        auto ctors = structure->getReflectConstructorList();
        if (ctors.empty())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getMethodStructType();
        vector<llvm::Constant*> rows;
        for (auto& m : ctors) {
            int64_t hash = signatureHash(m->toCanonical(/*labeled=*/false));
            const auto& mp = m->getParameterList();
            size_t userParams = mp.size();
            if (!mp.empty() && mp.front()->getName() == "this") userParams -= 1;
            rows.push_back(llvm::ConstantStruct::get(descTy, {
                emitCString(m->toCanonical()),
                emitCString(m->getReturnType()->toCanonical()),
                llvm::ConstantInt::get(llvmInt64Type, (uint64_t) hash),
                llvm::ConstantInt::get(llvmInt32Type, (uint64_t) packModifiers(m->getModifiers())),
                llvm::ConstantInt::get(llvmInt16Type, userParams),
                emitParameterTable(m),
                llvm::ConstantInt::get(llvmInt16Type, m->getAnnotationList().size()),
                emitAnnotationArray(m->getAnnotationList(), m->getAnnotationInstances()),
            }));
        }
        llvm::ArrayType* arrTy = llvm::ArrayType::get(descTy, rows.size());
        return new llvm::GlobalVariable(*module->getLlvmModule(), arrTy, true,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(arrTy, rows), ".rtti.ctors");
    }

    // ---- RTTI: per-class metadata blob (fixed-offset header) ----------------

    void StructureMetadata::createRttiType(CajetaClassPtr structure) {
        llvmRttiType = getRttiStructType();
        structure->setRttiType(llvmRttiType);
    }

    llvm::Constant* StructureMetadata::createRttiConstant(
            vector<llvm::Constant*>& args,
            CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::StructType* rttiTy = getRttiStructType();

        uint64_t allocSize = 0;
        if (structure->getLlvmType() && llvm::isa<llvm::StructType>(structure->getLlvmType())) {
            allocSize = module->getLlvmModule()->getDataLayout()
                .getTypeAllocSize(structure->getLlvmType());
        }

        size_t classAnnCount = structure->getAnnotationList().size();
        vector<std::string> parentNames;
        for (auto& p : structure->getSuperClasses()) parentNames.push_back(p->toCanonical());
        // Interfaces are parents for the runtime is-a question, which walks by name.
        for (auto& p : structure->getImplementedInterfaces()) {
            if (p) parentNames.push_back(p->toCanonical());
        }

        llvm::Constant* vtable = structure->getVirtualTableGlobal();
        if (!vtable) {
            vtable = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        }

        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        // Declarations only, so the #Rtti constant can take their address: bodies are
        // filled by a post-quiescence pass, once every method function exists.
        llvm::Constant* invokeAdapter = structure->getOrCreateReflectInvokeDecl();
        if (!invokeAdapter) invokeAdapter = nullPtr;
        llvm::Constant* newInstanceAdapter = structure->getOrCreateReflectNewDecl();
        if (!newInstanceAdapter) newInstanceAdapter = nullPtr;
        size_t ctorCount = structure->getReflectConstructorList().size();

        args.clear();
        return llvm::ConstantStruct::get(rttiTy, {
            llvm::ConstantInt::get(llvmInt64Type, allocSize),                  // 0
            emitCString(structure->toCanonical()),                            // 1
            llvm::ConstantInt::get(llvmInt32Type,                             // 2
                (uint64_t) packModifiers(structure->getModifiers())),
            llvm::ConstantInt::get(llvmInt16Type, classAnnCount),             // 3
            emitAnnotationArray(structure->getAnnotationList(),               // 4
                structure->getAnnotationInstances()),
            llvm::ConstantInt::get(llvmInt16Type,                             // 5
                structure->getPropertyList().size()),
            emitFieldTable(structure),                                        // 6
            llvm::ConstantInt::get(llvmInt16Type,                             // 7
                structure->getMethodList().size()),
            emitMethodTable(structure),                                       // 8
            llvm::ConstantInt::get(llvmInt16Type, parentNames.size()),        // 9
            emitCStringArray(parentNames),                                    // 10
            vtable,                                                           // 11
            invokeAdapter,                                                    // 12
            newInstanceAdapter,                                              // 13
            llvm::ConstantInt::get(llvmInt16Type, ctorCount),                // 14
            emitConstructorTable(structure),                                 // 15
            llvm::ConstantInt::get(llvmInt16Type,                            // 16
                structure->getTypeParameters().size()),
            emitTemplateParamTable(structure),                              // 17
            llvm::ConstantInt::get(llvmInt16Type,                            // 18
                structure->getTypeArguments().size()),
            emitTemplateArgArray(structure),                                // 19
        });
    }

    void StructureMetadata::populate(CajetaClassPtr structure) {
        // Builds three globals per structure — vtable, RTTI blob, #ClassObject — whose
        // references form a cycle, so all three are forward-declared and only THEN given
        // initializers. Idempotent: each bail-out is independent, so re-entry recovers.
        auto& ctx = *module->getLlvmContext();
        llvm::Module* lmod = module->getLlvmModule();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Emit into the structure's EMIT module: a stdlib-template instantiation over a
        // user type emits into the user module, and the fixups below key off that module.
        llvm::Module* emitLm = structure->getEmitModule()->getLlvmModule();

        bool initVtable = false;
        bool initRtti = false;
        bool initClassObject = false;

        // --- 1. forward-declare vtable global ---------------------------------
        if (structure->getVirtualTableGlobal() == nullptr) {
            string vtableName = structure->symbolBase() + string("#VTable");
            if (auto* existing = emitLm->getGlobalVariable(vtableName)) {
                structure->setVirtualTableGlobal(existing);
            } else {
                // The type-build reads virtualMethodList, which writeVirtualTable
                // populated via CajetaClass::buildVirtualTable before calling here.
                createVirtualTableType(structure);
                auto* g = (llvm::GlobalVariable*) emitLm->
                    getOrInsertGlobal(vtableName, structure->getVirtualTableType());
                structure->setVirtualTableGlobal(g);
                initVtable = true;
            }
        }

        // --- 2. forward-declare RTTI global -----------------------------------
        if (structure->getRttiGlobal() == nullptr) {
            string rttiName = structure->symbolBase() + string("#RttiGlobal");
            if (auto* existing = emitLm->getGlobalVariable(rttiName)) {
                structure->setRttiGlobal(existing);
            } else {
                createRttiType(structure);
                auto* g = (llvm::GlobalVariable*) emitLm->
                    getOrInsertGlobal(rttiName, structure->getRttiType());
                structure->setRttiGlobal(g);
                initRtti = true;
            }
        }

        // --- 3. forward-declare #ClassObject (reflect Class instance) ---------
        // Layout matches a cajeta.reflect.Class instance: { ptr vtable, ptr rtti }.
        if (structure->getClassObjectGlobal() == nullptr) {
            string classObjName = structure->symbolBase() + string("#ClassObject");
            if (auto* existing = emitLm->getGlobalVariable(classObjName)) {
                structure->setClassObjectGlobal(existing);
            } else {
                llvm::StructType* classObjTy =
                    llvm::StructType::get(ctx, {ptrTy, ptrTy});
                auto* g = (llvm::GlobalVariable*) emitLm->
                    getOrInsertGlobal(classObjName, classObjTy);
                structure->setClassObjectGlobal(g);
                initClassObject = true;
            }
        }

        // --- 4. fill initializers (all handles now exist) ---------------------
        if (initVtable) {
            vtableNullSlots.clear();
            structure->getVirtualTableGlobal()->setInitializer(
                createVirtualTableConstant(structure));
            // Raise HERE, not inside the builder: the global now has its initializer, so
            // unwinding leaves no half-built state.
            if (!vtableNullSlots.empty()) {
                std::string slots;
                for (auto& s : vtableNullSlots) {
                    if (!slots.empty()) slots += ", ";
                    slots += s;
                }
                vtableNullSlots.clear();
                throw Exception(
                    "vtable for `"
                        + (structure->getQName()
                            ? structure->getQName()->toCanonical()
                            : std::string("<anonymous>"))
                        + "` has no function for " + slots
                        + ". The method is registered but was never prototyped, so"
                          " dispatch through the slot would jump to null. This is a"
                          " COMPILER bug, not a source error. Known trigger: an"
                          " incremental build reaching a vtable before the method is"
                          " prototyped. Workaround: build clean (remove"
                          " `.cajeta/cache`) — a full build does not reproduce it.",
                    "CAJETA_ERROR_VTABLE_SLOT_UNRESOLVED");
            }
        }
        if (initRtti) {
            vector<llvm::Constant*> args;
            structure->getRttiGlobal()->setInitializer(
                createRttiConstant(args, structure));
        }
        if (initClassObject) {
            llvm::StructType* classObjTy = llvm::cast<llvm::StructType>(
                structure->getClassObjectGlobal()->getValueType());
            llvm::Constant* rttiRef = structure->getRttiGlobal();

            // Slot 0 is cajeta.reflect.Class<?>'s vtable — all Class<T> share one method
            // body, so every #ClassObject embeds that single wildcard instantiation, which
            // is force-built before codegen. NULL only for stdlib classes parsed before it.
            llvm::Constant* classVtableRef = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrTy));
            static const std::string kClassCanonical = "cajeta.reflect.Class<?>";
            auto& s2m = CajetaModule::getStructureToModule();
            auto mit = s2m.find(kClassCanonical);
            if (mit != s2m.end() && mit->second) {
                auto& structs = mit->second->getStructures();
                auto sit = structs.find(kClassCanonical);
                if (sit != structs.end() && sit->second) {
                    // Lookup-only: force-building Class here would re-enter runtime
                    // linkage mid-prototype and corrupt the module (see drop_fn below).
                    if (llvm::GlobalVariable* cv =
                            sit->second->getVirtualTableGlobal()) {
                        classVtableRef = CajetaModule::ensureGlobalInModule(
                            emitLm, cv);
                    }
                }
            }

            structure->getClassObjectGlobal()->setInitializer(
                llvm::ConstantStruct::get(classObjTy, {classVtableRef, rttiRef}));

            // The Class.forName registration ctor is emitted by finalizeClassObject, not
            // here (populate runs before the keep-set exists), and only when slot 0 is
            // non-null: a null Class#VTable entry crashes any accessor on the result.
        }
    }

    // Build the class's #VTable struct type. Entries are sorted by signature hash so
    // __cajeta_vtable_lookup can binary-search them, which gives every method one identity
    // however many bases it inherits from; member order matches CAJETA_VTABLE_* offsets.
    llvm::Type* StructureMetadata::createVirtualTableType(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);

        const auto& slots = structure->getVirtualMethodList();

        llvm::StructType* entryTy = llvm::StructType::get(ctx, {i64Ty, ptrTy});
        llvm::ArrayType* entriesTy = llvm::ArrayType::get(entryTy, slots.size());

        // Slot order is in lock-step with the runtime's CAJETA_VTABLE_* offsets: parent
        // drives ancestor walks, drop_fn routes scope-exit drops through the dynamic type,
        // classObject caches the reflect Class instance (NULL for pre-reflect classes).
        vector<llvm::Type*> members{
            llvmInt16Type,    // 0. version
            llvmInt16Type,    // 1. count
            ptrTy,            // 2. parent_vtable
            ptrTy,            // 3. drop_fn
            ptrTy,            // 4. classObject
            entriesTy,        // 5. entries
        };
        llvm::StructType* result = llvm::StructType::create(ctx,
            llvm::ArrayRef<llvm::Type*>(members),
            structure->symbolBase() + string("#VTable"));
        structure->setVirtualTableType(result);
        return result;
    }

    llvm::Constant* StructureMetadata::createVirtualTableConstant(CajetaClassPtr structure) {
        // populate has already built the type; the hash-sorted slot list is on the class.
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& slots = structure->getVirtualMethodList();

        // Entries are member 5, after version, count, parent_vtable, drop_fn, classObject.
        llvm::ArrayType* entriesArrTy = llvm::cast<llvm::ArrayType>(
            structure->getVirtualTableType()->getTypeAtIndex(5));
        llvm::StructType* entryTy = llvm::cast<llvm::StructType>(
            entriesArrTy->getElementType());

        // Use buildVirtualTable's hash list, not a recomputed one: an interface slot holds
        // the implementing Method, whose canonical differs from the interface method's.
        const auto& slotHashes = structure->getVirtualSlotHashList();
        vector<llvm::Constant*> entryConstants;
        entryConstants.reserve(slots.size());
        size_t hashIdx = 0;
        // Cross-module fixup: an inherited slot's llvm::Function lives in the parent's
        // module, so foreign references become extern decls the merge step resolves.
        llvm::Module* hostModule = structure->getEmitModule()->getLlvmModule();
        for (auto& method : slots) {
            int64_t hash = (hashIdx < slotHashes.size())
                ? slotHashes[hashIdx]
                : signatureHash(method->toCanonical(/*labeled=*/false));
            // getLlvmFunction() is RAW — null until the method is prototyped. Ask for the
            // type first: getLlvmFunctionType() lazily prototypes, leaving a declaration
            // this slot can reference and Phase 2 fills in.
            if (!method->getLlvmFunction() && !method->isMethodTemplate()) {
                method->getLlvmFunctionType();
            }
            llvm::Function* fn = method->getLlvmFunction();
            llvm::Function* resolved = CajetaModule::ensureFunctionInModule(
                hostModule, fn);
            // A raw nullptr is dereferenced by AsmPrinter at doFinalization — a SIGSEGV
            // naming nothing — so emit `ptr null` and record the slot for populate to raise.
            llvm::Constant* fnConst = resolved;
            if (!fnConst) {
                vtableNullSlots.push_back(
                    "slot " + std::to_string(hashIdx) + " `"
                    + method->toCanonical(/*labeled=*/false) + "`");
                fnConst = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(llvmPointerType));
            }
            entryConstants.push_back(llvm::ConstantStruct::get(entryTy, {
                llvm::ConstantInt::get(i64Ty, llvm::APInt(64, (uint64_t) hash, false)),
                fnConst,
            }));
            ++hashIdx;
        }
        llvm::Constant* entriesArr = llvm::ConstantArray::get(
            entriesArrTy, llvm::ArrayRef<llvm::Constant*>(entryConstants));

        // parent_vtable: the direct superclass's vtable global, NULL at the root and the
        // first superclass when there are several. Same cross-module fixup as above.
        llvm::Constant* parentVtable =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        const auto& parents = structure->getSuperClasses();
        if (!parents.empty()) {
            auto firstParent = parents.front();
            if (llvm::GlobalVariable* pv = firstParent->getVirtualTableGlobal()) {
                parentVtable = CajetaModule::ensureGlobalInModule(
                    hostModule, pv);
            }
        }

        // Slot 3 stays NULL here: building the drop function during vtable construction
        // cascades into runtime linkage before the host module has linked it.
        // CajetaClass::patchVirtualTableDropFn fills it in; the runtime null-checks it.
        llvm::Constant* dropFnConstant =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));

        // Slot 4: the #ClassObject populate() forward-declared, kept module-local.
        llvm::Constant* classObjectConstant =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        if (llvm::GlobalVariable* co = structure->getClassObjectGlobal()) {
            classObjectConstant = CajetaModule::ensureGlobalInModule(hostModule, co);
        }

        vector<llvm::Constant*> args{
            llvm::ConstantInt::get(llvmInt16Type, llvm::APInt(16, 0, false)),
            llvm::ConstantInt::get(llvmInt16Type,
                llvm::APInt(16, slots.size(), false)),
            parentVtable,
            dropFnConstant,
            classObjectConstant,
            entriesArr,
        };
        return llvm::ConstantStruct::get(structure->getVirtualTableType(),
            llvm::ArrayRef<llvm::Constant*>(args));
    }
} // code