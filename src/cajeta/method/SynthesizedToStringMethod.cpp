#include "SynthesizedToStringMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    /// FNV-1a 64-bit over a canonical signature. Must stay byte-for-byte
    /// identical to the runtime's __cajeta_vtable_lookup hash.
    static int64_t toStringSignatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }

    /// Finds the no-arg toString on `klass`, walking the parent chain so an
    /// inherited one is found too. Returns nullptr when there is none, which
    /// makes the synthesizer fall back to printing "null".
    static MethodPtr findToStringMethod(const CajetaClassPtr& klass) {
        if (!klass) return nullptr;
        for (auto& m : klass->getMethodList()) {
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "toString") continue;
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

    /// Throws CAJETA_ERROR_TOSTRING_FIELD naming the field, why it cannot be
    /// rendered, and what the author should do instead. Never returns.
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

    /// Maps a field's type to the render strategy, rejecting every type
    /// @ToString v1 cannot render. Throws rather than returning a sentinel.
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
            rejectToStringField(parent, fieldName, typeName,
                "view-typed fields can't be embedded in classes "
                "(see docs/specification/lang/Views.md)",
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
        // String is a POINTER_TYPE_ID, so it must be matched by name before
        // the class branch claims it.
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

    /// Returns the module's declaration of `symbol`, creating an external
    /// one with `fnTy` if this module has not declared it yet.
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

    /// Interns `s` as a private constant global and returns a `ptr` to its
    /// first byte, ready to pass to `__cajeta_str_concat`.
    static llvm::Value* emitLiteralPtr(llvm::IRBuilder<>& b,
                                        llvm::Module* lmod,
                                        const std::string& s,
                                        const std::string& name) {
        auto& ctx = lmod->getContext();
        llvm::Constant* strConst = llvm::ConstantDataArray::getString(ctx, s, true);
        auto* g = new llvm::GlobalVariable(
            *lmod, strConst->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, strConst,
            std::string(".ts.lit.") + name);
        g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        llvm::Value* zero = llvm::ConstantInt::get(
            llvm::IntegerType::getInt64Ty(ctx), 0);
        return b.CreateInBoundsGEP(strConst->getType(), g, {zero, zero},
            std::string("ts.litptr.") + name);
    }

    SynthesizedToStringMethod::SynthesizedToStringMethod(
            CajetaModulePtr module, CajetaClassPtr parent,
            ToStringFormat format,
            std::vector<StructurePropertyPtr> selectedFields,
            bool hasExplicitFieldSelection,
            bool callSuper)
        : Method(module, std::string("toString"),
                 CajetaType::of("String"), parent),
          format(format),
          selectedFields(std::move(selectedFields)),
          hasExplicitFieldSelection(hasExplicitFieldSelection),
          callSuper(callSuper) {
        this->parent = parent;
    }

    /// Emits the whole body: opener, optional `super=`, one entry per
    /// rendered field, closer, all concatenated, then wrapped into a real
    /// cajeta.lang.String. Runs post-prototype, so arg(0) is `this`.
    void SynthesizedToStringMethod::generateCode() {
        auto& llvmFunction = llvmFunctionRef();
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();

        llvm::Type* i8Ty  = llvm::Type::getInt8Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* f64Ty = llvm::Type::getDoubleTy(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

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
        llvm::Function* strCstr    = getOrDeclareTSFn(module, "__cajeta_string_cstr", toStringCallTy);

        llvm::Value* thisPtr = llvmFunction->getArg(0);

        bool isJson = (format == ToStringFormat::JSON);

        std::string simpleName = parent->getQName()
            ? parent->getQName()->getTypeName() : "<anon>";
        std::string opener = isJson ? std::string("{") : (simpleName + "(");

        llvm::Value* acc = emitLiteralPtr(b, lmod, opener, "open");

        // An explicit `of={...}` allowlist wins over @Exclude, and `of={}`
        // legitimately renders zero fields.
        std::vector<StructurePropertyPtr> rendered;
        if (hasExplicitFieldSelection) {
            rendered = selectedFields;
        } else {
            for (auto& prop : parent->getPropertyList()) {
                if (!prop || prop->isStatic()) continue;
                if (prop->findAnnotation("Exclude") != nullptr) continue;
                rendered.push_back(prop);
            }
        }

        // The super entry is a DIRECT call, not a vtable lookup: a lookup
        // would resolve to our own toString and recurse forever.
        bool superEmitted = false;
        if (callSuper) {
            CajetaClassPtr superClass;
            for (auto& sup : parent->getSuperClasses()) {
                if (!sup) continue;
                auto qn = sup->getQName();
                if (qn && qn->toCanonical() == "cajeta.lang.Object") continue;
                superClass = sup;
                break;
            }
            MethodPtr superToString;
            if (superClass) superToString = findToStringMethod(superClass);
            if (superToString && superToString->getLlvmFunction()) {
                llvm::Function* sFn = CajetaModule::ensureFunctionInModule(
                    lmod, superToString->getLlvmFunction());

                std::string superLabel = isJson
                    ? std::string("\"super\":")
                    : std::string("super=");
                llvm::Value* lbl = emitLiteralPtr(b, lmod, superLabel, "supLbl");
                acc = b.CreateCall(strConcat, {acc, lbl}, "ts.acc");

                llvm::Value* superStr = b.CreateCall(
                    sFn, {thisPtr}, "ts.supcall");
                superStr = b.CreateCall(strCstr, {superStr}, "ts.supcstr");
                acc = b.CreateCall(strConcat, {acc, superStr}, "ts.acc");
                superEmitted = true;
            }
        }

        for (size_t i = 0; i < rendered.size(); ++i) {
            auto& prop = rendered[i];
            CajetaTypePtr ftype = prop->getType();
            ToStringKind kind = classifyToStringFieldOrReject(
                parent, prop->getName(), ftype);

            if (i > 0 || superEmitted) {
                llvm::Value* comma = emitLiteralPtr(b, lmod, ",", "sep");
                acc = b.CreateCall(strConcat, {acc, comma}, "ts.acc");
            }

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
                llvm::Value* objPtr = b.CreateLoad(
                    ptrTy, fieldPtr,
                    std::string("ts.objp.") + prop->getName());
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

                b.SetInsertPoint(nullBB);
                llvm::Value* nullLit = emitLiteralPtr(b, lmod, "null", "null");
                b.CreateBr(mergeBB);

                b.SetInsertPoint(callBB);
                auto fieldKlass = dynamic_pointer_cast<CajetaClass>(ftype);
                MethodPtr ts = findToStringMethod(fieldKlass);
                llvm::Value* callStr;
                if (!ts) {
                    callStr = emitLiteralPtr(b, lmod, "<no-toString>", "noTs");
                } else {
                    int64_t sigHash = toStringSignatureHash(
                        ts->toCanonical(/*labeled=*/false));
                    // The vtable pointer is slot 0 of the object.
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
                    callStr = b.CreateCall(strCstr, {callStr},
                        std::string("ts.crc.") + prop->getName());
                }
                b.CreateBr(mergeBB);

                b.SetInsertPoint(mergeBB);
                llvm::PHINode* phi = b.CreatePHI(ptrTy, 2,
                    std::string("ts.phi.") + prop->getName());
                phi->addIncoming(nullLit, nullBB);
                phi->addIncoming(callStr, callBB);
                fieldStr = phi;
            } else if (kind == ToStringKind::STRING) {
                llvm::Value* sPtr = b.CreateLoad(
                    ptrTy, fieldPtr,
                    std::string("ts.s.") + prop->getName());
                if (isJson) {
                    auto* quoteStrTy = llvm::FunctionType::get(
                        ptrTy, {ptrTy}, false);
                    llvm::Function* quoteStr = getOrDeclareTSFn(
                        module, "__cajeta_json_quote_string", quoteStrTy);
                    fieldStr = b.CreateCall(quoteStr, {sPtr},
                        std::string("ts.jq.") + prop->getName());
                } else {
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

                    // __cajeta_str_concat consumes C strings; handing it a
                    // String OBJECT pointer reads the vtable bytes as text.
                    b.SetInsertPoint(okBB);
                    llvm::Value* okCstr = b.CreateCall(strCstr, {sPtr},
                        std::string("ts.scstr.") + prop->getName());
                    b.CreateBr(mergeBB);

                    b.SetInsertPoint(mergeBB);
                    llvm::PHINode* phi = b.CreatePHI(ptrTy, 2,
                        std::string("ts.sphi.") + prop->getName());
                    phi->addIncoming(nullLit, nullBB);
                    phi->addIncoming(okCstr, okBB);
                    fieldStr = phi;
                }
            } else {
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
                        callArg = b.CreateZExt(val, i32Ty);
                        tsFn = boolToStr;
                        break;
                    default:
                        continue;
                }
                (void) callArgTy;
                fieldStr = b.CreateCall(tsFn, {callArg},
                    std::string("ts.h.") + prop->getName());
            }

            acc = b.CreateCall(strConcat, {acc, fieldStr}, "ts.acc");
        }

        llvm::Value* closeLit = emitLiteralPtr(
            b, lmod, isJson ? std::string("}") : std::string(")"), "close");
        acc = b.CreateCall(strConcat, {acc, closeLit}, "ts.acc");

        // The concat chain built a malloc'd C string, but callers read the
        // cajeta.lang.String object layout, so it is wrapped at the boundary.
        auto* wrapTy = llvm::FunctionType::get(
            ptrTy, {ptrTy, ptrTy, i32Ty}, false);
        llvm::Function* strWrap = getOrDeclareTSFn(
            module, "__cajeta_string_wrap_cstr", wrapTy);
        llvm::Value* vtableRef = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));
        if (auto stringClass = dynamic_pointer_cast<CajetaClass>(
                CajetaType::of("String"))) {
            if (auto* vt = stringClass->getVirtualTableGlobal()) {
                vtableRef = CajetaModule::ensureGlobalInModule(
                    module->emitTargetLlvmModule(), vt);
            }
        }
        acc = b.CreateCall(strWrap,
            {acc, vtableRef, llvm::ConstantInt::get(i32Ty, 1)}, "ts.wrapstr");
        b.CreateRet(acc);
    }

}
