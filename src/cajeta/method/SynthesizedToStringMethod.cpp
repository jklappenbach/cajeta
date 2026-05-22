#include "SynthesizedToStringMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    // FNV-1a 64-bit. Local copy that matches the runtime's
    // __cajeta_vtable_lookup and the same helper in SynthesizedHashMethod
    // so vtable lookup keys agree byte-for-byte.
    static int64_t toStringSignatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }

    // Find the no-arg toString on `klass` (parameterList has just `this`
    // after generatePrototype, or is empty before). Walks the parent
    // chain so a class inheriting Object's toString without overriding
    // still finds it. Returns nullptr if absent — the synthesizer falls
    // back to printing "null".
    static MethodPtr findToStringMethod(const CajetaClassPtr& klass) {
        if (!klass) return nullptr;
        for (auto& m : klass->getMethodList()) {
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "toString") continue;
            // Accept both the pre-prototype shape (no params) and the
            // post-prototype shape (`this` injected at position 0).
            auto params = m->getParameterList();
            if (params.empty()) return m;
            if (params.size() == 1 && params.front()
                    && params.front()->getName() == "this") {
                return m;
            }
        }
        for (auto& sup : klass->getSuperClasses()) {
            if (auto found = findToStringMethod(sup)) return found;
        }
        return nullptr;
    }

    [[noreturn]] static void rejectToStringField(
            const CajetaClassPtr& parent,
            const std::string& fieldName,
            const std::string& fieldTypeName,
            const std::string& reason,
            const std::string& remediation) {
        std::string msg = "@ToString on `";
        msg += parent->getQName()->toCanonical();
        msg += "`: field `";
        msg += fieldName;
        msg += "` (type `";
        msg += fieldTypeName;
        msg += "`) cannot be rendered — ";
        msg += reason;
        msg += "; ";
        msg += remediation;
        throw Exception(msg, "CAJETA_ERROR_TOSTRING_FIELD");
    }

    enum class ToStringKind {
        PRIM_INT8_SIGNED,    // sext to i64, __cajeta_i64_to_str
        PRIM_INT8_UNSIGNED,  // zext to i64, __cajeta_i64_to_str
        PRIM_INT16_SIGNED,
        PRIM_INT16_UNSIGNED,
        PRIM_INT32_SIGNED,
        PRIM_INT32_UNSIGNED,
        PRIM_INT64_SIGNED,
        PRIM_INT64_UNSIGNED,
        PRIM_BOOLEAN,        // __cajeta_bool_to_str
        PRIM_FLOAT32,        // fpext to f64, __cajeta_f64_to_str
        PRIM_FLOAT64,        // __cajeta_f64_to_str
        STRING,              // already char*, use directly
        CLASS_REF,           // null-check + virtual .toString()
    };

    static ToStringKind classifyToStringFieldOrReject(
            const CajetaClassPtr& parent,
            const std::string& fieldName,
            CajetaTypePtr type) {
        if (!type) {
            rejectToStringField(parent, fieldName, "<null>",
                "field type is null at synthesis time",
                "ensure the field's type is registered before @ToString fires");
        }
        std::string typeName = type->getQName()
            ? type->getQName()->toCanonical() : "<anonymous>";

        bool isView = dynamic_pointer_cast<CajetaView>(type) != nullptr;
        bool isArr = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool isClassLike = dynamic_pointer_cast<CajetaClass>(type) != nullptr;

        if (isView) {
            // Views are already rejected as class fields at the layout
            // pass; this branch should never fire in practice.
            rejectToStringField(parent, fieldName, typeName,
                "view-typed fields can't be embedded in classes "
                "(see cajeta-docs/stdlib/Views.md)",
                "store the underlying byte[] and reconstruct the view "
                "in your manual toString()");
        }
        if (isArr) {
            rejectToStringField(parent, fieldName, typeName,
                "array-field rendering is not yet implemented in @ToString v1 "
                "(needs an element walk + size formatting)",
                "declare toString() manually on the enclosing class, or "
                "annotate the field with @ToString.Exclude to skip it");
        }
        // String is a POINTER_TYPE_ID with the canonical name "String"
        // — treat it specially before falling into the class branch.
        if (typeName == "cajeta.lang.String" || typeName == "String") {
            return ToStringKind::STRING;
        }
        if (isClassLike) {
            auto cls = dynamic_pointer_cast<CajetaClass>(type);
            if (cls && cls->isInterface()) {
                rejectToStringField(parent, fieldName, typeName,
                    "interface-typed fields would need fat-pointer "
                    "vtable dispatch in toString(), not yet supported",
                    "declare toString() manually, or @ToString.Exclude the field");
            }
            return ToStringKind::CLASS_REF;
        }

        switch (type->getTypeFlags() & TYPE_ID_MASK) {
            case BOOLEAN_ID:  return ToStringKind::PRIM_BOOLEAN;
            case INT8_ID:     return ToStringKind::PRIM_INT8_SIGNED;
            case UINT8_ID:    return ToStringKind::PRIM_INT8_UNSIGNED;
            case INT16_ID:    return ToStringKind::PRIM_INT16_SIGNED;
            case UINT16_ID:   return ToStringKind::PRIM_INT16_UNSIGNED;
            case INT32_ID:    return ToStringKind::PRIM_INT32_SIGNED;
            case UINT32_ID:   return ToStringKind::PRIM_INT32_UNSIGNED;
            case INT64_ID:    return ToStringKind::PRIM_INT64_SIGNED;
            case UINT64_ID:   return ToStringKind::PRIM_INT64_UNSIGNED;
            case FLOAT32_ID:  return ToStringKind::PRIM_FLOAT32;
            case FLOAT64_ID:  return ToStringKind::PRIM_FLOAT64;
            default:
                rejectToStringField(parent, fieldName, typeName,
                    "no @ToString primitive handler for this type yet "
                    "(extended-precision floats, 128-bit integers, and "
                    "bare `pointer` are not covered in v1)",
                    "declare toString() manually, or @ToString.Exclude the field");
        }
    }

    static llvm::Function* getOrDeclareTSFn(
            CajetaModulePtr module,
            const std::string& symbol,
            llvm::FunctionType* fnTy) {
        llvm::Module* lmod = module->getLlvmModule();
        if (llvm::Function* existing = lmod->getFunction(symbol)) {
            return existing;
        }
        return llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, symbol, lmod);
    }

    // Build a ConstantDataArray of the literal, GEP-load via i8*. The
    // returned llvm::Value* is a `ptr` to the first byte of the global,
    // suitable as a `__cajeta_str_concat` arg.
    static llvm::Value* emitLiteralPtr(llvm::IRBuilder<>& b,
                                        llvm::Module* lmod,
                                        const std::string& s,
                                        const std::string& name) {
        auto& ctx = lmod->getContext();
        llvm::Constant* strConst = llvm::ConstantDataArray::getString(ctx, s, true);
        // Dedupe via module's global pool — name-mangled by content hash
        // wouldn't add much; uniqueness via auto-renaming is fine for now.
        auto* g = new llvm::GlobalVariable(
            *lmod, strConst->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, strConst,
            std::string(".ts.lit.") + name);
        g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        // GEP &g[0][0] to get an i8*
        llvm::Value* zero = llvm::ConstantInt::get(
            llvm::IntegerType::getInt64Ty(ctx), 0);
        return b.CreateInBoundsGEP(strConst->getType(), g, {zero, zero},
            std::string("ts.litptr.") + name);
    }

    SynthesizedToStringMethod::SynthesizedToStringMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            ToStringFormat format)
        : Method(module, std::string("toString"),
                 CajetaType::of("String"), parent),
          format(format) {
        this->parent = parent;
    }

    void SynthesizedToStringMethod::generateCode() {
        // Signature post-prototype: (this) -> String/ptr. arg(0) is this.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();

        llvm::Type* i8Ty  = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* f64Ty = llvm::Type::getDoubleTy(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Runtime helper declarations.
        auto* i64ToStrTy   = llvm::FunctionType::get(ptrTy, {i64Ty}, false);
        auto* f64ToStrTy   = llvm::FunctionType::get(ptrTy, {f64Ty}, false);
        auto* boolToStrTy  = llvm::FunctionType::get(ptrTy, {i32Ty}, false);
        auto* concatTy     = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
        auto* lookupTy     = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);
        auto* toStringCallTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
        auto* jsonQuoteBufTy = llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false);

        llvm::Function* i64ToStr   = getOrDeclareTSFn(module, "__cajeta_i64_to_str", i64ToStrTy);
        llvm::Function* f64ToStr   = getOrDeclareTSFn(module, "__cajeta_f64_to_str", f64ToStrTy);
        llvm::Function* boolToStr  = getOrDeclareTSFn(module, "__cajeta_bool_to_str", boolToStrTy);
        llvm::Function* strConcat  = getOrDeclareTSFn(module, "__cajeta_str_concat", concatTy);
        llvm::Function* vtLookup   = getOrDeclareTSFn(module, "__cajeta_vtable_lookup", lookupTy);
        llvm::Function* jsonQuoteBuf = getOrDeclareTSFn(module, "__cajeta_json_quote_buf", jsonQuoteBufTy);

        llvm::Value* thisPtr = llvmFunction->getArg(0);

        bool isJson = (format == ToStringFormat::JSON);

        // Format-dependent opener:
        //   PROPERTIES: `ClassName(`
        //   JSON:       `{`
        std::string simpleName = parent->getQName()
            ? parent->getQName()->getTypeName() : "<anon>";
        std::string opener = isJson ? std::string("{") : (simpleName + "(");

        llvm::Value* acc = emitLiteralPtr(b, lmod, opener, "open");

        // Walk fields in declaration order. Skip @ToString.Exclude'd
        // fields and skip statics. Insert "," between adjacent fields.
        std::vector<StructurePropertyPtr> rendered;
        for (auto& prop : parent->getPropertyList()) {
            if (!prop || prop->isStatic()) continue;
            if (prop->findAnnotation("Exclude") != nullptr) continue;
            rendered.push_back(prop);
        }

        for (size_t i = 0; i < rendered.size(); ++i) {
            auto& prop = rendered[i];
            CajetaTypePtr ftype = prop->getType();
            ToStringKind kind = classifyToStringFieldOrReject(
                parent, prop->getName(), ftype);

            // Separator before all but the first field.
            if (i > 0) {
                llvm::Value* comma = emitLiteralPtr(b, lmod, ",", "sep");
                acc = b.CreateCall(strConcat, {acc, comma}, "ts.acc");
            }

            // Field label:
            //   PROPERTIES: `fieldName=`
            //   JSON:       `"fieldName":`
            std::string labelText = isJson
                ? (std::string("\"") + prop->getName() + "\":")
                : (prop->getName() + "=");
            llvm::Value* fieldLabel = emitLiteralPtr(
                b, lmod, labelText,
                std::string("lbl.") + prop->getName());
            acc = b.CreateCall(strConcat, {acc, fieldLabel}, "ts.acc");

            int idx = parent->getFieldLlvmIndex(prop);
            if (idx < 0) {
                throw Exception(
                    "@ToString synthesizer: field '" + prop->getName()
                    + "' has no LLVM index on '"
                    + parent->getQName()->toCanonical() + "'",
                    "CAJETA_ERROR_TOSTRING_FIELD_INDEX");
            }
            llvm::Value* fieldPtr = b.CreateStructGEP(
                parent->getLlvmType(), thisPtr, (unsigned) idx,
                std::string("ts.f.") + prop->getName());

            llvm::Value* fieldStr = nullptr;

            if (kind == ToStringKind::CLASS_REF) {
                // Load the field's pointer (class refs store as `ptr`).
                llvm::Value* objPtr = b.CreateLoad(
                    ptrTy, fieldPtr,
                    std::string("ts.objp.") + prop->getName());
                // Null check: if null, render "null".
                llvm::Function* curFn = b.GetInsertBlock()->getParent();
                llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(ctx,
                    std::string("ts.null.") + prop->getName(), curFn);
                llvm::BasicBlock* callBB = llvm::BasicBlock::Create(ctx,
                    std::string("ts.call.") + prop->getName(), curFn);
                llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx,
                    std::string("ts.mrg.") + prop->getName(), curFn);

                llvm::Value* isNull = b.CreateICmpEQ(
                    objPtr,
                    llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy)),
                    std::string("ts.isnull.") + prop->getName());
                b.CreateCondBr(isNull, nullBB, callBB);

                // null arm: produce a literal "null".
                b.SetInsertPoint(nullBB);
                llvm::Value* nullLit = emitLiteralPtr(b, lmod, "null", "null");
                b.CreateBr(mergeBB);

                // non-null arm: vtable dispatch toString().
                b.SetInsertPoint(callBB);
                auto fieldKlass = dynamic_pointer_cast<CajetaClass>(ftype);
                MethodPtr ts = findToStringMethod(fieldKlass);
                llvm::Value* callStr;
                if (!ts) {
                    // No toString anywhere in the chain — produce a
                    // placeholder. Object.toString() WILL exist once
                    // Object's synth lands; this is a safety net.
                    callStr = emitLiteralPtr(b, lmod, "<no-toString>", "noTs");
                } else {
                    int64_t sigHash = toStringSignatureHash(
                        ts->toCanonical(/*labeled=*/false));
                    // Load vtable from obj (slot 0).
                    llvm::Value* vtPtr = b.CreateLoad(
                        ptrTy, objPtr,
                        std::string("ts.vt.") + prop->getName());
                    llvm::Value* fnPtr = b.CreateCall(vtLookup, {
                        vtPtr,
                        llvm::ConstantInt::get(i64Ty,
                            llvm::APInt(64, (uint64_t) sigHash, false))
                    }, std::string("ts.fn.") + prop->getName());
                    callStr = b.CreateCall(toStringCallTy, fnPtr, {objPtr},
                        std::string("ts.cr.") + prop->getName());
                }
                b.CreateBr(mergeBB);

                b.SetInsertPoint(mergeBB);
                llvm::PHINode* phi = b.CreatePHI(ptrTy, 2,
                    std::string("ts.phi.") + prop->getName());
                phi->addIncoming(nullLit, nullBB);
                phi->addIncoming(callStr, callBB);
                fieldStr = phi;
            } else if (kind == ToStringKind::STRING) {
                // String is stored as i8* in the slot (POINTER_TYPE_ID).
                llvm::Value* sPtr = b.CreateLoad(
                    ptrTy, fieldPtr,
                    std::string("ts.s.") + prop->getName());
                if (isJson) {
                    // JSON path: a String field actually holds a
                    // pointer to a `cajeta.lang.String` struct
                    // `{ vtable*, int8[] bytes, int32 byteLength, int32 mode,
                    // int32 cachedCpLength }`. Pull out `bytes` (slot 1) and
                    // `byteLength` (slot 2), skip past the CajetaArray
                    // header `{ i64 count, [N x i8] data }` (8 bytes) to get
                    // the raw UTF-8 payload, and hand both to
                    // `__cajeta_json_quote_buf(data, length)`.
                    //
                    // Branches on a null receiver (the slot itself may be
                    // null for default-constructed strings) — the helper's
                    // `(null, 0)` path returns the literal `null` token,
                    // matching the rest of the synthesizer's null
                    // convention.
                    auto stringType = CajetaType::of("String");
                    auto stringClass = dynamic_pointer_cast<CajetaClass>(stringType);
                    if (!stringClass || !stringClass->getLlvmType()) {
                        throw Exception(
                            "@ToString(JSON) on `"
                            + parent->getQName()->toCanonical()
                            + "`: cajeta.lang.String type not registered "
                            "at synthesis time",
                            "CAJETA_ERROR_TOSTRING_JSON_STRING_MISSING");
                    }
                    llvm::Type* stringStructTy = stringClass->getLlvmType();

                    llvm::Function* curFn = b.GetInsertBlock()->getParent();
                    llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(ctx,
                        std::string("ts.jnull.") + prop->getName(), curFn);
                    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx,
                        std::string("ts.jok.") + prop->getName(), curFn);
                    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx,
                        std::string("ts.jmrg.") + prop->getName(), curFn);

                    llvm::Value* isNull = b.CreateICmpEQ(sPtr,
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy)),
                        std::string("ts.jin.") + prop->getName());
                    b.CreateCondBr(isNull, nullBB, okBB);

                    // null arm: pass (null, 0) — helper renders `null`.
                    b.SetInsertPoint(nullBB);
                    llvm::Value* nullRes = b.CreateCall(jsonQuoteBuf, {
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy)),
                        llvm::ConstantInt::get(i64Ty, 0)
                    }, std::string("ts.jnr.") + prop->getName());
                    b.CreateBr(mergeBB);

                    // non-null arm: GEP bytes / byteLength, skip array hdr.
                    b.SetInsertPoint(okBB);
                    llvm::Value* bytesPP = b.CreateStructGEP(
                        stringStructTy, sPtr, 1,
                        std::string("ts.bpp.") + prop->getName());
                    llvm::Value* bytesPtr = b.CreateLoad(
                        ptrTy, bytesPP,
                        std::string("ts.bp.") + prop->getName());
                    llvm::Value* lenP = b.CreateStructGEP(
                        stringStructTy, sPtr, 2,
                        std::string("ts.lp.") + prop->getName());
                    llvm::Value* len32 = b.CreateLoad(
                        i32Ty, lenP,
                        std::string("ts.l32.") + prop->getName());
                    llvm::Value* len64 = b.CreateSExt(len32, i64Ty,
                        std::string("ts.l64.") + prop->getName());
                    // Skip the 8-byte CajetaArray count header.
                    llvm::Value* dataPtr = b.CreateInBoundsGEP(
                        i8Ty, bytesPtr,
                        llvm::ConstantInt::get(i64Ty, 8),
                        std::string("ts.dp.") + prop->getName());
                    llvm::Value* okRes = b.CreateCall(jsonQuoteBuf,
                        {dataPtr, len64},
                        std::string("ts.jor.") + prop->getName());
                    b.CreateBr(mergeBB);

                    b.SetInsertPoint(mergeBB);
                    llvm::PHINode* phi = b.CreatePHI(ptrTy, 2,
                        std::string("ts.jph.") + prop->getName());
                    phi->addIncoming(nullRes, nullBB);
                    phi->addIncoming(okRes, okBB);
                    fieldStr = phi;
                } else {
                    // PROPERTIES path: null-check; render "null" for
                    // null strings (consistent with class-ref handling).
                    llvm::Function* curFn = b.GetInsertBlock()->getParent();
                    llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(ctx,
                        std::string("ts.snull.") + prop->getName(), curFn);
                    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(ctx,
                        std::string("ts.sok.") + prop->getName(), curFn);
                    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx,
                        std::string("ts.smrg.") + prop->getName(), curFn);
                    llvm::Value* isNull = b.CreateICmpEQ(sPtr,
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy)),
                        std::string("ts.sisnull.") + prop->getName());
                    b.CreateCondBr(isNull, nullBB, okBB);

                    b.SetInsertPoint(nullBB);
                    llvm::Value* nullLit = emitLiteralPtr(b, lmod, "null", "snull");
                    b.CreateBr(mergeBB);

                    b.SetInsertPoint(okBB);
                    b.CreateBr(mergeBB);

                    b.SetInsertPoint(mergeBB);
                    llvm::PHINode* phi = b.CreatePHI(ptrTy, 2,
                        std::string("ts.sphi.") + prop->getName());
                    phi->addIncoming(nullLit, nullBB);
                    phi->addIncoming(sPtr, okBB);
                    fieldStr = phi;
                }
            } else {
                // Primitive path — load, coerce, call __cajeta_X_to_str.
                llvm::Type* loadTy = ftype->getLlvmType();
                llvm::Value* val = b.CreateLoad(loadTy, fieldPtr,
                    std::string("ts.v.") + prop->getName());

                llvm::Value* callArg = val;
                llvm::Type* callArgTy = loadTy;
                llvm::Function* tsFn = nullptr;
                switch (kind) {
                    case ToStringKind::PRIM_INT8_SIGNED:
                    case ToStringKind::PRIM_INT16_SIGNED:
                    case ToStringKind::PRIM_INT32_SIGNED:
                        callArg = b.CreateSExt(val, i64Ty);
                        tsFn = i64ToStr;
                        break;
                    case ToStringKind::PRIM_INT8_UNSIGNED:
                    case ToStringKind::PRIM_INT16_UNSIGNED:
                    case ToStringKind::PRIM_INT32_UNSIGNED:
                    case ToStringKind::PRIM_INT64_UNSIGNED:
                        callArg = b.CreateZExt(val, i64Ty);
                        tsFn = i64ToStr;
                        break;
                    case ToStringKind::PRIM_INT64_SIGNED:
                        tsFn = i64ToStr;
                        break;
                    case ToStringKind::PRIM_FLOAT32:
                        callArg = b.CreateFPExt(val, f64Ty);
                        tsFn = f64ToStr;
                        break;
                    case ToStringKind::PRIM_FLOAT64:
                        tsFn = f64ToStr;
                        break;
                    case ToStringKind::PRIM_BOOLEAN:
                        // Booleans load as i1; zext to i32 for the helper.
                        callArg = b.CreateZExt(val, i32Ty);
                        tsFn = boolToStr;
                        break;
                    default:
                        // Unreachable — classify rejected anything else.
                        continue;
                }
                (void) callArgTy;
                fieldStr = b.CreateCall(tsFn, {callArg},
                    std::string("ts.h.") + prop->getName());
            }

            acc = b.CreateCall(strConcat, {acc, fieldStr}, "ts.acc");
        }

        // Close:
        //   PROPERTIES: `)`
        //   JSON:       `}`
        llvm::Value* closeLit = emitLiteralPtr(
            b, lmod, isJson ? std::string("}") : std::string(")"), "close");
        acc = b.CreateCall(strConcat, {acc, closeLit}, "ts.acc");
        b.CreateRet(acc);
    }

}
