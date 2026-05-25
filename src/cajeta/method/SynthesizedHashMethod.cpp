#include "SynthesizedHashMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaView.h"
#include "../type/CajetaArray.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    // FNV-1a 64-bit. Duplicated from the anon namespace in
    // CajetaClass.cpp where MethodCallExpression uses it for vtable
    // dispatch — keeping a private copy here avoids exposing the
    // helper externally and keeps the synthesizer's hash bytes byte-
    // identical to the dispatch site. If either copy ever drifts,
    // the runtime's __cajeta_vtable_lookup wouldn't find the slot.
    static int64_t synthSignatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }

    // Find the hash() method on `klass` — the no-formal-parameter form
    // (parameterList contains only the implicit `this` slot after
    // generatePrototype). Returns nullptr if absent. By the time the
    // synthesizer runs (during this class's own prototype generation),
    // class-typed field references resolve to classes whose own
    // hash() has been prototyped: either manually declared, or
    // (when @AutoHash is on those classes too) injected by their own
    // synthesizeAutoHash pass which runs in declaration order.
    static MethodPtr findHashMethod(const CajetaClassPtr& klass) {
        if (!klass) return nullptr;
        for (auto& m : klass->getMethodList()) {
            if (m->getName() != "hash") continue;
            if (m->isConstructor()) continue;
            if (m->getParameterList().size() != 1) continue;
            return m;
        }
        return nullptr;
    }

    // Throw a diagnostic naming the @AutoHash'd class, the offending
    // field, and a concrete remediation. The format intentionally
    // mirrors the example diagnostics in stdlib/ so users
    // see what the doc promises.
    [[noreturn]] static void rejectField(
            const CajetaClassPtr& parent,
            const std::string& fieldName,
            const std::string& fieldTypeName,
            const std::string& reason,
            const std::string& remediation) {
        std::string msg = "@AutoHash on `";
        msg += parent->getQName()->toCanonical();
        msg += "`: field `";
        msg += fieldName;
        msg += "` (type `";
        msg += fieldTypeName;
        msg += "`) cannot be auto-hashed — ";
        msg += reason;
        msg += "; ";
        msg += remediation;
        throw Exception(msg, "CAJETA_ERROR_AUTOHASH_FIELD");
    }

    enum class FieldHashKind {
        SKIP_UNREACHABLE,      // can't happen in v1; placeholder
        PRIM_INT8,             // hash via __cajeta_hash_int32 with sext
        PRIM_UINT8,            // hash via __cajeta_hash_int32 with zext
        PRIM_INT16,            // ditto
        PRIM_UINT16,           // ditto
        PRIM_INT32,            // direct
        PRIM_INT64,            // direct
        PRIM_BOOLEAN,          // __cajeta_hash_boolean (zext i1→i8)
        PRIM_FLOAT32,          // __cajeta_hash_float32
        PRIM_FLOAT64,          // __cajeta_hash_float64
        CLASS_INLINE,          // virtual field.hash() via vtable
    };

    // Decide how to hash a field's type, OR throw a diagnostic if the
    // type isn't supported in v1. CajetaArray inherits from CajetaClass,
    // so the array case must be checked before the plain class case.
    static FieldHashKind classifyOrReject(
            const CajetaClassPtr& parent,
            const std::string& fieldName,
            const CajetaTypePtr& type) {
        if (!type) {
            rejectField(parent, fieldName, "<unknown>",
                "field has no resolved type (compiler bug or unresolvable "
                "forward reference)",
                "remove @AutoHash or fix the field's type declaration");
        }
        std::string typeName = type->getQName()
            ? type->getQName()->toCanonical() : "<anonymous>";

        bool isStruct = dynamic_pointer_cast<CajetaView>(type) != nullptr;
        bool isArr = dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        bool isClassLike = dynamic_pointer_cast<CajetaClass>(type) != nullptr;

        if (isStruct) {
            rejectField(parent, fieldName, typeName,
                "struct-field hashing is not yet implemented in @AutoHash v1 "
                "(needs a recursive struct-layout walk)",
                "declare hash() manually on the enclosing class for now, or "
                "remove the struct field if it's not part of the keyed identity");
        }
        if (isArr) {
            rejectField(parent, fieldName, typeName,
                "array-field hashing is not yet implemented in @AutoHash v1 "
                "(needs an element walk that bounds-checks at runtime)",
                "declare hash() manually on the enclosing class for now, or "
                "move the array out of the key");
        }
        if (isClassLike) {
            return FieldHashKind::CLASS_INLINE;
        }

        switch (type->getTypeFlags() & TYPE_ID_MASK) {
            case BOOLEAN_ID:  return FieldHashKind::PRIM_BOOLEAN;
            case INT8_ID:     return FieldHashKind::PRIM_INT8;
            case UINT8_ID:    return FieldHashKind::PRIM_UINT8;
            case INT16_ID:    return FieldHashKind::PRIM_INT16;
            case UINT16_ID:   return FieldHashKind::PRIM_UINT16;
            case INT32_ID:
            case UINT32_ID:   return FieldHashKind::PRIM_INT32;
            case INT64_ID:
            case UINT64_ID:   return FieldHashKind::PRIM_INT64;
            case FLOAT32_ID:  return FieldHashKind::PRIM_FLOAT32;
            case FLOAT64_ID:  return FieldHashKind::PRIM_FLOAT64;
            default:
                // Extended-precision (fp4/6/8/16/128), int128/uint128,
                // bare `pointer`, and the `String` native alias (which
                // also has POINTER_TYPE_ID) land here. None has a
                // dedicated runtime hash helper today.
                rejectField(parent, fieldName, typeName,
                    "no @AutoHash primitive handler for this type yet "
                    "(extended-precision floats, 128-bit integers, "
                    "`pointer`, and `String` are not covered in v1)",
                    "declare hash() manually on the enclosing class, or "
                    "exclude this field from the key");
        }
    }

    static llvm::Function* getOrDeclareRuntimeFn(
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

    SynthesizedHashMethod::SynthesizedHashMethod(
            CajetaModulePtr module, CajetaClassPtr parent)
        : Method(module, std::string("hash"),
                 CajetaType::of("int64"), parent) {
        this->parent = parent;
        // Block stays null — generateCode emits IR directly.
    }

    void SynthesizedHashMethod::generateCode() {
        // Method::generatePrototype already injected `this` as the
        // first parameter and built llvmFunction with type
        // (ptr) -> i64. Emit a single entry block, walk fields, ret.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8Ty  = llvm::Type::getInt8Ty(ctx);
        llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
        llvm::Type* f64Ty = llvm::Type::getDoubleTy(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        llvm::FunctionType* seedTy =
            llvm::FunctionType::get(i64Ty, false);
        llvm::Function* seedFn = getOrDeclareRuntimeFn(
            module, "__cajeta_hash_seed", seedTy);
        llvm::Value* seedAcc = b.CreateCall(seedFn, {}, "hash.acc");

        llvm::FunctionType* combineTy =
            llvm::FunctionType::get(i64Ty, { i64Ty, i64Ty }, false);
        llvm::Function* combineFn = getOrDeclareRuntimeFn(
            module, "__cajeta_hash_combine", combineTy);

        llvm::FunctionType* lookupTy =
            llvm::FunctionType::get(ptrTy, { ptrTy, i64Ty }, false);
        llvm::Function* lookupFn = getOrDeclareRuntimeFn(
            module, "__cajeta_vtable_lookup", lookupTy);

        llvm::FunctionType* hashCallTy =
            llvm::FunctionType::get(i64Ty, { ptrTy }, false);

        llvm::Value* acc = seedAcc;

        // Walk inherited fields first (deepest ancestor's own fields,
        // then progressively nearer), finally this class's own fields.
        // Matches CajetaClass::generatePrototype's struct-layout
        // order so the hash includes every field exactly once.
        std::function<void(const CajetaClassPtr&)>
            walkInheritedThenOwn;
        walkInheritedThenOwn = [&](const CajetaClassPtr& cls) {
            for (auto& sup : cls->getSuperClasses()) {
                if (!sup) continue;
                walkInheritedThenOwn(sup);
            }
            for (auto& prop : cls->getPropertyList()) {
                int idx = parent->getFieldLlvmIndex(prop);
                if (idx < 0) continue;
                CajetaTypePtr fieldType = prop->getType();
                FieldHashKind kind = classifyOrReject(
                    parent, prop->getName(), fieldType);

                llvm::Value* thisPtr = llvmFunction->getArg(0);
                llvm::Value* fieldPtr = b.CreateStructGEP(
                    parent->getLlvmType(), thisPtr,
                    (unsigned) idx,
                    std::string("hash.f.") + prop->getName());

                llvm::Value* fieldHash = nullptr;

                if (kind == FieldHashKind::CLASS_INLINE) {
                    // Class-typed field stored INLINE in the parent
                    // struct. The embedded class's vtable* lives at
                    // its slot 0 — i.e. exactly at fieldPtr. Null-
                    // check the vtable before dispatch: an
                    // unininitialized embedded class (e.g. Exception's
                    // `cause = 0` pattern) would otherwise segfault
                    // in __cajeta_vtable_lookup.
                    auto fieldKlass = dynamic_pointer_cast<CajetaClass>(fieldType);
                    MethodPtr fieldHashMethod = findHashMethod(fieldKlass);
                    if (!fieldHashMethod) {
                        rejectField(parent, prop->getName(),
                            fieldType->getQName()
                                ? fieldType->getQName()->toCanonical()
                                : "<anonymous>",
                            "the field's class has no hash() method visible "
                            "to the synthesizer (either none declared and the "
                            "field's class itself isn't @AutoHash'd, or the "
                            "method-lookup order didn't see it in time)",
                            "add @AutoHash to the field's class, or declare "
                            "hash() manually on it");
                    }

                    llvm::Value* vtableVal = b.CreateLoad(
                        ptrTy, fieldPtr,
                        std::string("hash.v.") + prop->getName());
                    llvm::Value* isNull = b.CreateICmpEQ(
                        vtableVal,
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy)),
                        std::string("hash.vnull.") + prop->getName());

                    llvm::Function* curFn = b.GetInsertBlock()->getParent();
                    llvm::BasicBlock* nullBB =
                        llvm::BasicBlock::Create(ctx,
                            std::string("hash.null.") + prop->getName(),
                            curFn);
                    llvm::BasicBlock* callBB =
                        llvm::BasicBlock::Create(ctx,
                            std::string("hash.call.") + prop->getName(),
                            curFn);
                    llvm::BasicBlock* mergeBB =
                        llvm::BasicBlock::Create(ctx,
                            std::string("hash.mrg.") + prop->getName(),
                            curFn);
                    b.CreateCondBr(isNull, nullBB, callBB);

                    // Null branch — contribute the seed value so a
                    // null embedded field hashes deterministically
                    // (and identically across instances).
                    b.SetInsertPoint(nullBB);
                    llvm::Value* nullContribution = seedAcc;
                    b.CreateBr(mergeBB);

                    // Non-null branch — vtable virtual dispatch.
                    b.SetInsertPoint(callBB);
                    int64_t sigHash = synthSignatureHash(
                        fieldHashMethod->toCanonical(/*labeled=*/false));
                    llvm::Value* fnPtr = b.CreateCall(
                        lookupFn,
                        { vtableVal,
                          llvm::ConstantInt::get(i64Ty,
                              llvm::APInt(64, (uint64_t) sigHash, false)) },
                        std::string("hash.fn.") + prop->getName());
                    llvm::Value* callResult = b.CreateCall(
                        hashCallTy, fnPtr, { fieldPtr },
                        std::string("hash.cr.") + prop->getName());
                    b.CreateBr(mergeBB);

                    b.SetInsertPoint(mergeBB);
                    llvm::PHINode* merged = b.CreatePHI(i64Ty, 2,
                        std::string("hash.h.") + prop->getName());
                    merged->addIncoming(nullContribution, nullBB);
                    merged->addIncoming(callResult, callBB);
                    fieldHash = merged;

                } else {
                    // Primitive path — load the field, optionally
                    // coerce, call the appropriate __cajeta_hash_X.
                    llvm::Type* loadTy = fieldType->getLlvmType();
                    llvm::Value* fieldVal = b.CreateLoad(
                        loadTy, fieldPtr,
                        std::string("hash.v.") + prop->getName());

                    llvm::Value* callArg = fieldVal;
                    llvm::Type*  runtimeArgTy = loadTy;
                    const char*  symbol = nullptr;
                    switch (kind) {
                        case FieldHashKind::PRIM_BOOLEAN:
                            callArg = b.CreateZExt(fieldVal, i8Ty);
                            runtimeArgTy = i8Ty;
                            symbol = "__cajeta_hash_boolean";
                            break;
                        case FieldHashKind::PRIM_INT8:
                            callArg = b.CreateSExt(fieldVal, i32Ty);
                            runtimeArgTy = i32Ty;
                            symbol = "__cajeta_hash_int32";
                            break;
                        case FieldHashKind::PRIM_UINT8:
                            callArg = b.CreateZExt(fieldVal, i32Ty);
                            runtimeArgTy = i32Ty;
                            symbol = "__cajeta_hash_int32";
                            break;
                        case FieldHashKind::PRIM_INT16:
                            callArg = b.CreateSExt(fieldVal, i32Ty);
                            runtimeArgTy = i32Ty;
                            symbol = "__cajeta_hash_int32";
                            break;
                        case FieldHashKind::PRIM_UINT16:
                            callArg = b.CreateZExt(fieldVal, i32Ty);
                            runtimeArgTy = i32Ty;
                            symbol = "__cajeta_hash_int32";
                            break;
                        case FieldHashKind::PRIM_INT32:
                            symbol = "__cajeta_hash_int32";
                            break;
                        case FieldHashKind::PRIM_INT64:
                            symbol = "__cajeta_hash_int64";
                            break;
                        case FieldHashKind::PRIM_FLOAT32:
                            symbol = "__cajeta_hash_float32";
                            break;
                        case FieldHashKind::PRIM_FLOAT64:
                            symbol = "__cajeta_hash_float64";
                            break;
                        default:
                            // classifyOrReject already threw for
                            // SKIP_UNREACHABLE / unsupported kinds;
                            // CLASS_INLINE handled above. Anything
                            // landing here is a coding bug.
                            continue;
                    }
                    llvm::FunctionType* helperTy =
                        llvm::FunctionType::get(i64Ty,
                            { runtimeArgTy }, false);
                    llvm::Function* helperFn = getOrDeclareRuntimeFn(
                        module, symbol, helperTy);
                    fieldHash = b.CreateCall(helperFn, { callArg },
                        std::string("hash.h.") + prop->getName());
                }

                acc = b.CreateCall(combineFn, { acc, fieldHash },
                    "hash.acc");
            }
        };

        walkInheritedThenOwn(parent);

        b.CreateRet(acc);
    }

}
