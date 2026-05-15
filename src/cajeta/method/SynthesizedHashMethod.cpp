#include "SynthesizedHashMethod.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaStruct.h"
#include "../type/CajetaArray.h"
#include "../compile/CajetaModule.h"

#include <llvm/IR/IRBuilder.h>

using namespace std;

namespace cajeta {

    // Pick which `__cajeta_hash_*` runtime function to use for a given
    // field type. Returns nullptr for types we don't yet hash (struct
    // fields, arrays, sub-byte / extended-precision floats); callers
    // skip those fields rather than emit a broken call. Also reports
    // whether the field value needs a width-coercion (zext/sext to i32
    // for sub-32-bit ints, zext to i8 for boolean) before the call.
    enum class CoercionKind {
        NONE,
        ZEXT_TO_I8,        // boolean -> int8 for __cajeta_hash_boolean
        SEXT_TO_I32,       // signed int8/int16 -> int32
        ZEXT_TO_I32,       // unsigned uint8/uint16 -> int32
        BITCAST_TO_PTR,    // class-reference, pointer-via-identity
    };

    struct HashCall {
        const char*  runtimeSymbol;
        CoercionKind coercion;
    };

    static HashCall pickHashCall(const CajetaTypePtr& type) {
        // Class references, structs, and arrays — defer.
        //
        // Class-typed fields are stored INLINE in the parent struct's
        // layout (CajetaClass::generatePrototype pushes the field's
        // raw llvmType, not a pointer-to). To hash them properly the
        // synthesizer needs to either (a) walk the embedded class's
        // own field layout recursively, or (b) virtually dispatch to
        // the field's `hash()` method using a GEP pointer as `this`.
        // The pointer-identity shortcut doesn't work here — the
        // field's address is positional, not the embedded object's
        // identity, so two equal instances of the containing class
        // would hash differently. Skipping these fields produces a
        // hash that's still deterministic but loses field-level
        // structural info; manual hash() overrides cover the gap
        // until the next synthesizer cut adds the virtual-call form.
        //
        // Struct fields (CajetaStruct) and array fields face the
        // same problem — defer for the same reason.
        bool isClassLike =
            dynamic_pointer_cast<CajetaClass>(type) != nullptr;
        bool isStruct =
            dynamic_pointer_cast<CajetaStruct>(type) != nullptr;
        bool isArr =
            dynamic_pointer_cast<CajetaArray>(type) != nullptr;
        if (isClassLike || isStruct || isArr) {
            return { nullptr, CoercionKind::NONE };
        }
        // Primitives — dispatch by the type's id portion of typeFlags.
        CajetaTypeFlags f = type->getTypeFlags();
        switch (f & TYPE_ID_MASK) {
            case (BOOLEAN_ID):
                return { "__cajeta_hash_boolean", CoercionKind::ZEXT_TO_I8 };
            case (INT8_ID):
                return { "__cajeta_hash_int32", CoercionKind::SEXT_TO_I32 };
            case (UINT8_ID):
                return { "__cajeta_hash_int32", CoercionKind::ZEXT_TO_I32 };
            case (INT16_ID):
                return { "__cajeta_hash_int32", CoercionKind::SEXT_TO_I32 };
            case (UINT16_ID):
                return { "__cajeta_hash_int32", CoercionKind::ZEXT_TO_I32 };
            case (INT32_ID):
            case (UINT32_ID):
                return { "__cajeta_hash_int32", CoercionKind::NONE };
            case (INT64_ID):
            case (UINT64_ID):
                return { "__cajeta_hash_int64", CoercionKind::NONE };
            case (FLOAT32_ID):
                return { "__cajeta_hash_float32", CoercionKind::NONE };
            case (FLOAT64_ID):
                return { "__cajeta_hash_float64", CoercionKind::NONE };
            default:
                // fp4 / fp6 / fp8 / fp16 / fp128 / int128 / uint128 /
                // pointer — no specialized runtime hash today. Skip;
                // these can pick up dedicated mixers in a later cut.
                return { nullptr, CoercionKind::NONE };
        }
    }

    // Declare-or-fetch the named runtime function in the current
    // module. Same pattern Method::emitNativeForwardingBody uses for
    // its forwarding target — the bitcode runtime supplies the body
    // at JIT-link time.
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
        // Block stays null — generateCode emits IR directly without
        // walking a parsed Block AST.
    }

    void SynthesizedHashMethod::generateCode() {
        // Method::generatePrototype already injected `this` as the
        // first parameter and built llvmFunction with type
        // (ptr) -> i64. We emit a single basic block here, then close
        // with a ret.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i8Ty  = llvm::Type::getInt8Ty(ctx);
        llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
        llvm::Type* f64Ty = llvm::Type::getDoubleTy(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        // Seed the accumulator with the per-process random seed.
        // __cajeta_hash_seed() returns i64.
        llvm::FunctionType* seedTy =
            llvm::FunctionType::get(i64Ty, false);
        llvm::Function* seedFn = getOrDeclareRuntimeFn(
            module, "__cajeta_hash_seed", seedTy);
        llvm::Value* acc = b.CreateCall(seedFn, {}, "hash.acc");

        // Combine helper: (i64, i64) -> i64.
        llvm::FunctionType* combineTy =
            llvm::FunctionType::get(i64Ty, { i64Ty, i64Ty }, false);
        llvm::Function* combineFn = getOrDeclareRuntimeFn(
            module, "__cajeta_hash_combine", combineTy);

        // Walk fields in layout order: deepest ancestor's own fields
        // first, then progressively nearer ancestors, finally this
        // class's own fields. Matches CajetaClass::generatePrototype's
        // struct-layout order so the hash includes every field exactly
        // once. The recursive walker captures everything in the
        // parent chain; we then add our own propertyList. Inherited
        // fields use the parent's StructureProperty entry — its
        // getFieldLlvmIndex resolves correctly against our class's
        // struct because the inherited-field LLVM indices are
        // identical between parent and subclass (see CajetaClass.h:258).
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
                if (!fieldType) continue;
                HashCall pick = pickHashCall(fieldType);
                if (pick.runtimeSymbol == nullptr) continue;

                // GEP into `this` at the field's LLVM index. parent
                // here is the class we're synthesizing for — its
                // llvmType is the full inheritance-flattened struct.
                llvm::Value* thisPtr = llvmFunction->getArg(0);
                llvm::Value* fieldPtr = b.CreateStructGEP(
                    parent->getLlvmType(), thisPtr,
                    (unsigned) idx, std::string("hash.f.") + prop->getName());

                // Load the field at its native LLVM type, then coerce
                // for the runtime call where the runtime takes a
                // wider/different type. The coercion rules mirror the
                // C ABI of the corresponding __cajeta_hash_X helper.
                llvm::Type* loadTy = fieldType->getLlvmType();
                llvm::Value* fieldVal = b.CreateLoad(loadTy, fieldPtr,
                    std::string("hash.v.") + prop->getName());

                llvm::Value* callArg = fieldVal;
                llvm::Type* runtimeArgTy = nullptr;
                switch (pick.coercion) {
                    case CoercionKind::NONE:
                        runtimeArgTy = loadTy;
                        break;
                    case CoercionKind::ZEXT_TO_I8:
                        callArg = b.CreateZExt(fieldVal, i8Ty);
                        runtimeArgTy = i8Ty;
                        break;
                    case CoercionKind::SEXT_TO_I32:
                        callArg = b.CreateSExt(fieldVal, i32Ty);
                        runtimeArgTy = i32Ty;
                        break;
                    case CoercionKind::ZEXT_TO_I32:
                        callArg = b.CreateZExt(fieldVal, i32Ty);
                        runtimeArgTy = i32Ty;
                        break;
                    case CoercionKind::BITCAST_TO_PTR:
                        // Class-typed fields are stored as ptr in the
                        // struct, so loadTy is already ptr — no actual
                        // cast needed, just pass through.
                        runtimeArgTy = ptrTy;
                        break;
                }

                llvm::FunctionType* helperTy =
                    llvm::FunctionType::get(i64Ty, { runtimeArgTy }, false);
                llvm::Function* helperFn = getOrDeclareRuntimeFn(
                    module, pick.runtimeSymbol, helperTy);

                llvm::Value* fieldHash = b.CreateCall(
                    helperFn, { callArg },
                    std::string("hash.h.") + prop->getName());

                acc = b.CreateCall(combineFn, { acc, fieldHash }, "hash.acc");
            }
        };

        // Note: parent->getSuperClasses() recursion handles ancestors;
        // we walk THIS class only at the top level (parent->...own).
        // walkInheritedThenOwn(parent) walks ancestors then parent's
        // own fields — exactly what we want.
        walkInheritedThenOwn(parent);

        // Floats need to land back as i64 — the helpers already do
        // that internally, so acc is i64 throughout. Final ret.
        b.CreateRet(acc);
    }

}
