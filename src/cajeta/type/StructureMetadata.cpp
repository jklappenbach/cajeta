//
// Created by James Klappenbach on 11/20/22.
//

#include "StructureMetadata.h"

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

namespace cajeta {

    // FNV-1a 64-bit; matches the runtime's __cajeta_signature_hash so the
    // compile-time hash of a method's canonical signature equals the runtime
    // lookup hash at dispatch time.
    //
    // `#` is skipped (mode-erased dispatch, element-ownership §5.1.4) — see
    // the note on CajetaClass.cpp's signatureHash; the three copies (here,
    // there, and the runtime C function) must stay in lockstep.
    static int64_t signatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            if (c == '#') continue;
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }

    /**
     * 1. Parameter Name (string, array)
     * 2. Parameter Type (string, array)
     * 3. Modifier Count (int8)
     * 4. Modifiers (int8, array)
     * 5. Annotation Count (int8)
     * 6. Annotation Types (array of strings)
     *
     * @param module
     */
    // ---- Fixed-layout RTTI emitters (REFL-1) --------------------------------
    //
    // emit a private, null-terminated C string global; returns an i8* to its
    // bytes. UnnamedAddr lets LLVM merge identical strings across the module.
    llvm::Constant* StructureMetadata::emitCString(const std::string& s) {
        auto& ctx = *module->getLlvmContext();
        llvm::Constant* data = llvm::ConstantDataArray::getString(ctx, s, true);
        auto* g = new llvm::GlobalVariable(
            *module->getLlvmModule(), data->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, data, ".rtti.str");
        g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return g;
    }

    // emit a private [N x i8*] of the strings; returns ptr to it, or a null
    // ptr constant for an empty list.
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

    // Fixed descriptor struct types — one named struct each, shared by every
    // class (the per-class data lives in separate table globals). Kept in
    // lock-step with the C mirrors in cajeta_runtime.c.

    // CajetaField / CajetaParameter share a shape:
    //   { ptr name, ptr type, i32 modifiers, i16 annotationCount, ptr annotations }
    // CajetaFieldDesc:
    //   { ptr name, ptr type, i32 modifiers, i16 annotationCount, ptr annotations,
    //     i32 byteOffset, i64 typeFlags }
    // byteOffset is the field's offset within the instance struct (the data-driven
    // hook REFL-3's Field.get/set reads — no per-class accessor codegen needed for
    // fields). -1 for static fields (they live in globals, not the instance).
    // typeFlags is the field type's full CajetaType TYPE_ID flag word (encodes
    // size / int-vs-float / signed / primitive-vs-reference) for typed access.
    // NOTE: diverged from #ParameterDesc (which keeps the original 5-field shape) —
    // the C mirror splits CajetaFieldDesc / CajetaParamDesc to match.
    llvm::StructType* StructureMetadata::getFieldStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#FieldDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, ptrTy, llvmInt32Type, llvmInt16Type, ptrTy, llvmInt32Type, llvmInt64Type},
            "cajeta.reflect.#FieldDesc");
    }
    llvm::StructType* StructureMetadata::getParameterStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#ParameterDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, ptrTy, llvmInt32Type, llvmInt16Type, ptrTy}, "cajeta.reflect.#ParameterDesc");
    }
    // CajetaMethod:
    //   { ptr name, ptr returnType, i64 sigHash, i32 modifiers,
    //     i16 parameterCount, ptr parameters,
    //     i16 annotationCount, ptr annotations }
    // annotationCount/annotations (REFL-6a) carry the method's annotation NAMEs
    // (canonical strings, same shape as the field/parameter annotation lists) so
    // runtime annotation reflection can enumerate them. Appended after the
    // original 6 fields so existing readers (name/returnType/sigHash/params)
    // keep their offsets. The shared constructor table reuses this shape.
    llvm::StructType* StructureMetadata::getMethodStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#MethodDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, ptrTy, llvmInt64Type, llvmInt32Type, llvmInt16Type, ptrTy,
             llvmInt16Type, ptrTy},
            "cajeta.reflect.#MethodDesc");
    }
    // CajetaRtti header (#RttiGlobal), fixed for every class:
    //   { i64 allocationSize, ptr typeName, i32 modifiers,
    //     i16 classAnnotationCount, ptr classAnnotations,
    //     i16 propertyCount, ptr properties,
    //     i16 methodCount, ptr methods,
    //     i16 parentCount, ptr parentNames,
    //     ptr vtable, ptr invokeAdapter, ptr newInstanceAdapter }
    // invokeAdapter / newInstanceAdapter are the compiler-synthesized per-class
    // reflection adapters (REFL-2): a switch-over-index that dispatches a
    // reflective method call / construction to a direct LLVM call. Forward-
    // declared during populate (so the #Rtti constant can reference them) and
    // given a body in a post-quiescence pass once all method functions exist.
    // Either may be null (a class with no invokable methods / no constructor).
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

    // REFL-6b: #AnnotationArgDesc — one captured annotation argument value.
    //   { ptr name, i32 kind, i64 i64Val, ptr strVal, i8 boolVal,
    //     i32 listCount, ptr listData }
    // `kind` mirrors AnnotationArgKind (Int64=0, String=1, Bool=2, ClassRef=3,
    // Int64List=4, StringList=5, BoolList=6). For scalar kinds: `strVal` holds
    // the string payload (and, for ClassRef, the referenced type name); the
    // scalar fields hold the Int64/Bool payloads. For the *List kinds:
    // `listCount`/`listData` carry the elements — listData points at
    // [N x i64] (Int64List), [N x i8*] (StringList), or [N x i8] (BoolList).
    llvm::StructType* StructureMetadata::getAnnotationArgStructType() {
        auto& ctx = *module->getLlvmContext();
        if (auto* e = llvm::StructType::getTypeByName(ctx, "cajeta.reflect.#AnnotationArgDesc")) return e;
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        return llvm::StructType::create(ctx,
            {ptrTy, llvmInt32Type, llvmInt64Type, ptrTy, llvmInt8Type,
             llvmInt32Type, ptrTy},
            "cajeta.reflect.#AnnotationArgDesc");
    }
    // REFL-6b: #AnnotationDesc — { ptr name, i16 argCount, ptr args }.
    // The `annotations` pointer in every owner descriptor (#FieldDesc /
    // #MethodDesc / #ParameterDesc and the #Rtti class slot) points at an array
    // of these. `name` is the annotation's canonical type name (REFL-6a); `args`
    // is the [argCount x #AnnotationArgDesc] table (null when the annotation has
    // no arguments, e.g. a bare `@Tracked` or any parameter annotation).
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
            // strVal carries the String payload and the ClassRef type name; for
            // the other scalar kinds it stays empty.
            llvm::Constant* strConst =
                (a.kind == AnnotationArgKind::String || a.kind == AnnotationArgKind::ClassRef)
                    ? emitCString(a.strVal)
                    : nullPtr;
            // *List kinds: emit the element array as listData (shape depends on
            // the kind) and record listCount; scalar kinds leave both empty.
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
        // Pair each name with its argument-carrying instance by canonical name.
        // Annotations are not repeatable in v1, so the canonical is a unique key;
        // parameter owners pass no instances, so every lookup misses (names only).
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

    // REFL-7: #TemplateParamDesc — one declared template parameter.
    //   { ptr name, i16 boundCount, ptr bounds, i8 isNonType, ptr nonTypePrimitive }
    // bounds is an [N x i8*] of canonical bound type names; nonTypePrimitive is
    // the declared primitive name for a `<uint32 N>` value parameter (null
    // otherwise). Lock-step with CajetaTemplateParamDesc in cajeta_runtime.c.
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
        // Concrete template arguments (instantiation only) as canonical names.
        vector<std::string> names;
        for (auto& a : structure->getTypeArguments())
            if (a) names.push_back(a->toCanonical());
        return emitCStringArray(names);
    }

    llvm::Constant* StructureMetadata::emitParameterTable(MethodPtr method) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& params = method->getParameterList();
        // Skip the implicit leading `this` (instance methods carry it as a
        // FormalParameter named "this") so the reflected parameter list is the
        // user-visible signature, matching getMethodParamCount.
        size_t start = (!params.empty() && params.front()->getName() == "this") ? 1 : 0;
        if (params.size() <= start)
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        llvm::StructType* descTy = getParameterStructType();
        vector<llvm::Constant*> rows;
        for (size_t pi = start; pi < params.size(); ++pi) {
            auto& p = params[pi];
            // Parameter annotations carry names AND argument values (REFL-6b):
            // FormalParameter::fromContext now records them via the args-aware
            // addAnnotationInstance, so the instances pair with the name list in
            // emitAnnotationArray exactly like field/method owners.
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
        // Instance struct layout: lets us record each field's byte offset so
        // reflective field access is data-driven (no per-class accessor codegen).
        auto* instStruct = llvm::dyn_cast_or_null<llvm::StructType>(structure->getLlvmType());
        const llvm::StructLayout* layout = instStruct
            ? module->getLlvmModule()->getDataLayout().getStructLayout(instStruct)
            : nullptr;
        vector<llvm::Constant*> rows;
        for (auto& p : props) {
            // -1 for static fields (no instance-struct slot) and when the
            // layout/index is unavailable; otherwise the real byte offset.
            int32_t byteOffset = -1;
            int llvmIdx = structure->getFieldLlvmIndex(p);
            if (layout && llvmIdx >= 0 && (unsigned) llvmIdx < instStruct->getNumElements())
                byteOffset = (int32_t) layout->getElementOffset((unsigned) llvmIdx);
            uint64_t typeFlags = p->getType() ? (uint64_t) p->getType()->getTypeFlags() : 0;
            // Field annotations carry names (REFL-6a) AND argument values
            // (REFL-6b) — the field's AnnotationInstances are paired with its
            // annotationList by canonical name in emitAnnotationArray.
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
            // User-visible parameter count: exclude the implicit leading `this`.
            const auto& mp = m->getParameterList();
            size_t userParams = mp.size();
            if (!mp.empty() && mp.front()->getName() == "this") userParams -= 1;
            // Method/constructor annotations carry names (REFL-6a) AND argument
            // values (REFL-6b): emitAnnotationArray pairs the annotationList with
            // the args-carrying AnnotationInstances by canonical name.
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

    // REFL-2C: constructor descriptor table, reusing the #MethodDesc shape.
    // Ordered by getReflectConstructorList so its index matches the newInstance
    // adapter's switch.
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
            // Method/constructor annotations carry names (REFL-6a) AND argument
            // values (REFL-6b): emitAnnotationArray pairs the annotationList with
            // the args-carrying AnnotationInstances by canonical name.
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
    //
    // #RttiGlobal is the FIXED-layout `cajeta.reflect.#Rtti` header (the same
    // LLVM struct type for every class — see getRttiStructType). Variable-
    // length data — the type name, descriptor tables, annotation/parent-name
    // lists — live in separately-emitted private globals referenced by
    // pointer, so runtime natives can walk the metadata with constant offsets.
    //
    // What's here now:
    //   - Annotation NAMES + ARGUMENT VALUES. Every owner's `annotations`
    //     pointer (class slots 3/4, #FieldDesc/#MethodDesc/#ParameterDesc) is an
    //     [N x #AnnotationDesc] table — each row { canonical name, argCount,
    //     args }, with args an [M x #AnnotationArgDesc] of the captured scalar
    //     values (`@Order(2)`'s `2`, `@Component(name="disk")`'s `"disk"`,
    //     REFL-6b). The values come from the args-carrying AnnotationInstance,
    //     paired to the name list by canonical name.
    //     Parameter annotation argument values ride too — FormalParameter::
    //     fromContext captures full instances via the shared parseAnnotationInstance.
    //     LIST-valued arguments (`@SuppressLint({"a","b"})`) carry their element
    //     data in #AnnotationArgDesc.listCount/listData (i64 / i8* / i8 arrays).
    // What's NOT here yet:
    //   - Recursive Type descriptors for parents (canonical names instead —
    //     the runtime can look up a parent's RTTI by name when needed).

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

        // Class annotation count (names). The #AnnotationDesc table itself —
        // names (REFL-6a) plus argument values (REFL-6b) — is emitted below via
        // emitAnnotationArray, pairing the class's annotationList with its
        // args-carrying AnnotationInstances.
        size_t classAnnCount = structure->getAnnotationList().size();
        vector<std::string> parentNames;
        for (auto& p : structure->getSuperClasses()) parentNames.push_back(p->toCanonical());

        llvm::Constant* vtable = structure->getVirtualTableGlobal();
        if (!vtable) {
            vtable = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        }

        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        // REFL-2: forward-declare the reflection adapters (declarations only —
        // no body, no linkRuntime/vtable work, safe inside this populate window)
        // so the #Rtti constant can take their address. Bodies are filled later
        // (Compiler post-quiescence pass) once every method function exists.
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
        // Idempotent. Builds three globals per structure: the vtable, the
        // RTTI blob, and the cached reflect Class instance (#ClassObject).
        // They form a reference cycle —
        //   vtable.classObject -> #ClassObject.rtti -> #RttiGlobal.vtable
        //     -> #VTable.classObject
        // — so we forward-declare all three (getOrInsertGlobal yields a usable
        // handle before its initializer is set) and only THEN fill in the
        // initializers. Each bail-out check is independent so re-entry after a
        // partial build recovers cleanly.
        auto& ctx = *module->getLlvmContext();
        llvm::Module* lmod = module->getLlvmModule();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Emit the vtable + RTTI into the structure's EMIT module (test-reuse:
        // a stdlib-template instantiation over a user type emits into the user
        // module, not the cached stdlib). getEmitModule() == module in
        // production. The cross-module fixups below (ensureFunctionInModule /
        // ensureGlobalInModule) are keyed off this same module, so the vtable's
        // method-pointer + parent-vtable entries become local extern decls here
        // and resolve at the merge step.
        llvm::Module* emitLm = structure->getEmitModule()->getLlvmModule();

        bool initVtable = false;
        bool initRtti = false;
        bool initClassObject = false;

        // --- 1. forward-declare vtable global ---------------------------------
        if (structure->getVirtualTableGlobal() == nullptr) {
            string vtableName = structure->toCanonical() + string("#VTable");
            if (auto* existing = emitLm->getGlobalVariable(vtableName)) {
                structure->setVirtualTableGlobal(existing);
            } else {
                // Build the type first so the global has somewhere to land.
                // The type-build uses virtualMethodList (populated by
                // CajetaClass::buildVirtualTable, which writeVirtualTable
                // runs before calling this method).
                createVirtualTableType(structure);
                auto* g = (llvm::GlobalVariable*) emitLm->
                    getOrInsertGlobal(vtableName, structure->getVirtualTableType());
                structure->setVirtualTableGlobal(g);
                initVtable = true;
            }
        }

        // --- 2. forward-declare RTTI global -----------------------------------
        if (structure->getRttiGlobal() == nullptr) {
            string rttiName = structure->toCanonical() + string("#RttiGlobal");
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
        // Layout matches a cajeta.reflect.Class instance: { ptr vtable,
        // ptr rtti }. Slot 0 (the Class vtable) is patched in when reflect
        // support lands fully; NULL is safe because Class's own accessors are
        // non-virtual (statically dispatched), so the instance's slot 0 is
        // never dereferenced to call getName()/getModifiers()/etc.
        if (structure->getClassObjectGlobal() == nullptr) {
            string classObjName = structure->toCanonical() + string("#ClassObject");
            if (auto* existing = lmod->getGlobalVariable(classObjName)) {
                structure->setClassObjectGlobal(existing);
            } else {
                llvm::StructType* classObjTy =
                    llvm::StructType::get(ctx, {ptrTy, ptrTy});
                auto* g = (llvm::GlobalVariable*) lmod->
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
            // Raise HERE, not inside the builder: the global now has its
            // initializer, so unwinding leaves no half-built state. A null slot
            // means dispatch through it would jump to null, so refuse to emit it
            // rather than ship a binary that faults at the call.
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

            // Slot 0: cajeta.reflect.Class's vtable, so virtual dispatch on the
            // returned Class instance lands on Class's accessors (getName /
            // getFieldCount / ...). The stdlib — including cajeta.reflect.Class
            // — is fully compiled before user code, so by the time a user
            // class's #ClassObject is emitted, Class's vtable global already
            // exists; we look it up rather than force-build it (which would
            // re-enter populate mid-prototype, the cascade the drop_fn slot
            // warns against above). When populating Class itself, its own
            // vtable global was forward-declared in step 1 above, so the lookup
            // still finds it. Falls back to NULL only for the handful of stdlib
            // classes parsed before Class (not reflectively reachable in v1).
            llvm::Constant* classVtableRef = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrTy));
            // REFL-1.7: cajeta.reflect.Class is a template Class<T>; the bare
            // name names the (never-built) template. Every #ClassObject embeds
            // the ONE canonical wildcard instantiation Class<?>'s vtable — all
            // Class<T> share identical method code, so the phantom T is
            // irrelevant at the vtable. Class<?> is force-built before codegen
            // (Compiler::compile / JitTestHelper), so this lookup resolves.
            static const std::string kClassCanonical = "cajeta.reflect.Class<?>";
            auto& s2m = CajetaModule::getStructureToModule();
            auto mit = s2m.find(kClassCanonical);
            if (mit != s2m.end() && mit->second) {
                auto& structs = mit->second->getStructures();
                auto sit = structs.find(kClassCanonical);
                if (sit != structs.end() && sit->second) {
                    // Lookup-only — do NOT force-build Class here. Triggering
                    // Class's populate mid-prototype re-enters runtime linkage
                    // (getOrCreateDropFunction -> linkRuntime) and corrupts the
                    // module — the exact cascade the drop_fn slot warns about
                    // above. The whole stdlib (Class included) is built by
                    // ensureStdlibModule BEFORE user code compiles, so a user
                    // class's #ClassObject finds Class#VTable already present.
                    // When populating Class itself, its vtable was forward-
                    // declared in step 1 above, so the lookup still resolves.
                    // The only NULLs are stdlib classes parsed before Class,
                    // which v1 doesn't reflect on.
                    if (llvm::GlobalVariable* cv =
                            sit->second->getVirtualTableGlobal()) {
                        classVtableRef = CajetaModule::ensureGlobalInModule(lmod, cv);
                    }
                }
            }

            structure->getClassObjectGlobal()->setInitializer(
                llvm::ConstantStruct::get(classObjTy, {classVtableRef, rttiRef}));

            // REFL-8: register this class's #ClassObject under its canonical
            // name so Class.forName(name) can resolve it from a string with no
            // live instance. Emit a global ctor calling
            // __cajeta_register_class(name, classObject) and append it to
            // llvm.global_ctors, which the JIT runs at module-load and the AOT C
            // runtime runs at startup. One ctor per class — this block is the
            // single #ClassObject definition site (guarded by initClassObject),
            // so no duplicate registration is emitted.
            //
            // ONLY register when slot 0 (Class#VTable) is non-null. A class whose
            // #ClassObject has a null Class#VTable (the handful of stdlib classes
            // parsed before cajeta.reflect.Class — see above) is not reflectively
            // dispatchable: any accessor on the returned Class virtual-dispatches
            // through slot 0 and would crash. forName/allClasses/classesInPackage/
            // classesAnnotated (REFL-8/REFL-10) enumerate the registry and call
            // those accessors, so a null-vtable entry must not be discoverable.
            // Class.of never returns these (such classes are never reflected on
            // in v1), which is why earlier phases never tripped over them.
            //
            // DCE Tier-0b: reg ctor is emitted in finalizeClassObject, not here —
            // populate runs mid-codegen-loop, before the keep-set exists. We only
            // set the #ClassObject initializer above.
        }
    }

    // VTable layout:
    //   { i16 version, i16 count, ptr parent_vtable,
    //     [count x { i64 hash, ptr fn }] entries }
    //
    // Entries are sorted by hash (FNV-1a of method->toCanonical(false)) so
    // dispatch is a binary search via __cajeta_vtable_lookup. Hash-based
    // addressing means each method has a stable identity regardless of how
    // many bases the class inherits from — multiple inheritance just works,
    // no thunks or per-base offset trickery.
    //
    // parent_vtable points at the direct superclass's vtable global (or
    // NULL for the root). Used by __cajeta_is_unrecoverable to walk up the
    // hierarchy at runtime, checking if any ancestor matches the
    // UnrecoverableException sentinel — and by future RTTI helpers that
    // need arbitrary up-the-chain queries.
    //
    // Layout note: i16/i16 (4 bytes) + 4 bytes pad + ptr (8 bytes) puts
    // the entries array at byte offset 16. The runtime helper's pointer
    // arithmetic uses that constant.
    llvm::Type* StructureMetadata::createVirtualTableType(CajetaClassPtr structure) {
        auto& ctx = *module->getLlvmContext();
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);

        const auto& slots = structure->getVirtualMethodList();

        // Entry: { i64 hash, ptr fn }. Anonymous (literal) struct so the LLVM
        // module's type table dedupes; doesn't need a name of its own.
        llvm::StructType* entryTy = llvm::StructType::get(ctx, {i64Ty, ptrTy});
        llvm::ArrayType* entriesTy = llvm::ArrayType::get(entryTy, slots.size());

        // Layout (kept in sync with runtime CAJETA_VTABLE_* offsets):
        //   0. i16 version
        //   1. i16 count
        //   2. ptr parent_vtable   — drives ancestor walks (e.g. is-unrecoverable)
        //   3. ptr drop_fn         — points at this class's synthesized
        //                            drop wrapper; used by
        //                            __cajeta_class_virtual_drop to route
        //                            scope-exit drops through the dynamic
        //                            type (Gap 1: virtual dispatch on drop)
        //   4. ptr classObject     — this class's cached cajeta.reflect.Class
        //                            instance; the runtime hop for
        //                            Object.getClass() (Reflection REFL-1).
        //                            NULL for classes built before reflect
        //                            support / value types.
        //   5. entries [N x {i64 hash, ptr fn}]
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
            structure->toCanonical() + string("#VTable"));
        structure->setVirtualTableType(result);
        return result;
    }

    llvm::Constant* StructureMetadata::createVirtualTableConstant(CajetaClassPtr structure) {
        // Caller (populate) has already built the type — we do NOT recreate
        // it here. The slot list lives on the class (built by
        // CajetaClass::buildVirtualTable, already hash-sorted).
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i64Ty = llvm::IntegerType::getInt64Ty(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
        const auto& slots = structure->getVirtualMethodList();

        // Entries live at index 5 (after version, count, parent_vtable,
        // drop_fn, classObject).
        llvm::ArrayType* entriesArrTy = llvm::cast<llvm::ArrayType>(
            structure->getVirtualTableType()->getTypeAtIndex(5));
        llvm::StructType* entryTy = llvm::cast<llvm::StructType>(
            entriesArrTy->getElementType());

        // Use the lock-step hash list from buildVirtualTable rather than
        // recomputing — interface entries store the implementing class's
        // Method (so getLlvmFunction returns the concrete function) but
        // the hash needs to be the *interface* method's canonical, which
        // recomputing from `method` would lose.
        const auto& slotHashes = structure->getVirtualSlotHashList();
        vector<llvm::Constant*> entryConstants;
        entryConstants.reserve(slots.size());
        size_t hashIdx = 0;
        // Cross-module fixup: inherited-method slots reference the
        // parent's llvm::Function, which lives in the parent's
        // llvm::Module. The vtable constant lives in `module`'s
        // llvm::Module. Replace any foreign reference with an
        // extern decl in our module — the post-parse merge step
        // resolves it to the real definition.
        llvm::Module* hostModule = structure->getEmitModule()->getLlvmModule();
        for (auto& method : slots) {
            int64_t hash = (hashIdx < slotHashes.size())
                ? slotHashes[hashIdx]
                : signatureHash(method->toCanonical(/*labeled=*/false));
            // getLlvmFunction() is a RAW accessor — null until the method has
            // been prototyped. A replayed method-template instantiation reaches
            // the vtable before Phase 1 prototypes it, so ask for the type
            // first: getLlvmFunctionType() lazily runs generatePrototype (both
            // are idempotent — Phase 1 revisits them for free), leaving a
            // declaration the slot can reference and Phase 2's generateCode
            // fills in.
            if (!method->getLlvmFunction() && !method->isMethodTemplate()) {
                method->getLlvmFunctionType();
            }
            llvm::Function* fn = method->getLlvmFunction();
            llvm::Function* resolved = CajetaModule::ensureFunctionInModule(
                hostModule, fn);
            // A raw nullptr here goes into the constant as-is and LLVM's
            // AsmPrinter dereferences it walking the initializer at
            // doFinalization — a SIGSEGV deep in codegen naming nothing. Emit a
            // real `ptr null` so the constant completes, and record the slot;
            // populate raises it once the global is whole (see vtableNullSlots).
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

        // parent_vtable: pointer to the direct superclass's vtable global.
        // NULL when the class has no parent (e.g. Throwable). For multi-
        // parent (not supported today), we'd need a more elaborate
        // representation; the first superclass wins for now.
        //
        // Cross-module fixup mirrors the method-pointer case above —
        // the parent's vtable global lives in the parent's llvm::Module,
        // not necessarily ours.
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

        // Slot 3: drop_fn — populated lazily.
        //
        // Calling getOrCreateDropFunction here (eagerly, during vtable
        // construction) breaks the build: vtable populate runs as part
        // of prototype generation, before the host module has linked
        // the runtime — and the synthesized drop body references
        // __cajeta_free, so the call cascades into runtime linkage
        // that re-enters prototype work and loses the Object#VTable
        // initializer. Diagnosed by null-slot-only builds passing
        // (basic tests work) while eager-population builds fail with
        // "Symbols not found: cajeta.lang.Object#VTable" at JIT load.
        //
        // Instead we leave NULL here, and patch the vtable's drop_fn
        // slot from CajetaClass::patchVirtualTableDropFn at the first
        // site that needs it (the heap class local drop-registration
        // in LocalVariableDeclaration::generateCode). The vtable
        // global and its initializer live in the class's home module,
        // and so does the drop wrapper, so the patch stays
        // module-local and replaces the ConstantStruct's drop_fn
        // element with the real function pointer.
        //
        // The runtime dispatcher (__cajeta_class_virtual_drop)
        // null-checks the slot, so vtables for unused classes (no
        // heap local of that type) stay valid with a NULL slot.
        llvm::Constant* dropFnConstant =
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));

        // Slot 4: classObject — this class's cached cajeta.reflect.Class
        // instance, emitted alongside the vtable in populate(). Forward-
        // declared there before this constant is built, so the handle is
        // available even though its own initializer is set afterward.
        // ensureGlobalInModule keeps the reference module-local under the
        // cross-module merge.
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