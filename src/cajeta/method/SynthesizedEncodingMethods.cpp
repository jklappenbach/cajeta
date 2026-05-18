#include "SynthesizedEncodingMethods.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaArray.h"
#include "../type/FormalParameter.h"
#include "../compile/CajetaModule.h"
#include "../error/Exception.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Intrinsics.h>

using namespace std;

namespace cajeta {

    // Find a static method on `cls` with the given name that takes
    // exactly one parameter, and whose param type's canonical name
    // matches `wantCanon`. Static methods' parameterList has only
    // user params (no implicit `this`). Returns nullptr if no
    // matching method exists.
    static MethodPtr findStaticUnaryMethod(CajetaClassPtr cls,
                                            const std::string& name,
                                            const std::string& wantCanon) {
        if (!cls) return nullptr;
        for (auto& m : cls->getMethodList()) {
            if (!m || m->isConstructor()) continue;
            auto& mods = m->getModifiers();
            if (mods.find(STATIC) == mods.end()) continue;
            if (m->getName() != name) continue;
            auto params = m->getParameterList();
            // Static methods don't have `this` injected — exactly one
            // param means one user-visible param.
            if (params.size() != 1 || !params[0]) continue;
            auto pt = params[0]->getType();
            if (!pt) continue;
            std::string pCanon = pt->getQName()
                ? pt->getQName()->toCanonical() : "";
            // Match by canonical name; accept either bare typeName or
            // fully-qualified form (encoder authors may import the
            // type or refer by short name).
            std::string pShort = pt->getQName()
                ? pt->getQName()->getTypeName() : "";
            if (pCanon == wantCanon || pShort == wantCanon) return m;
        }
        return nullptr;
    }

    // ---- byte[]-taking ctor ---------------------------------------------

    SynthesizedEncodingCtor::SynthesizedEncodingCtor(
            CajetaModulePtr module, CajetaClassPtr parent,
            CajetaClassPtr encoder)
        : Method(module, parent->getQName()->getTypeName(),
                 CajetaType::of("void"), parent),
          encoder(encoder) {
        this->parent = parent;
    }

    void SynthesizedEncodingCtor::initParameter() {
        if (!parameterList.empty()) return;  // idempotent
        // Parameter type is byte[] — CajetaType::of("byte") wrapped
        // in a CajetaArray.
        auto byteType = CajetaType::of("int8");  // byte == int8 in Cajeta
        // Use the canonical byte[] type via CajetaArray. We mirror the
        // ArrayList / ArrayStream pattern of constructing array types
        // through CajetaArray::getOrInsert (or similar).
        // For simplicity, find an existing byte[] type if one is in
        // the canonical map; otherwise construct one.
        auto& canonicalMap = CajetaType::getCanonicalMap();
        CajetaTypePtr arrayType;
        auto it = canonicalMap.find("int8[]");
        if (it != canonicalMap.end()) arrayType = it->second;
        if (!arrayType) {
            // Fallback: search for any cached byte[] CajetaArray.
            for (auto& [name, t] : canonicalMap) {
                if (auto arr = dynamic_pointer_cast<CajetaArray>(t)) {
                    auto et = arr->getElementType();
                    if (et && et->getQName()
                            && et->getQName()->getTypeName() == "int8") {
                        arrayType = arr;
                        break;
                    }
                }
            }
        }
        // Last resort: build one. Most code paths should have a byte[]
        // cached by this point (Optional / String etc.), but be defensive.
        if (!arrayType) {
            arrayType = make_shared<CajetaArray>(module, byteType);
        }

        auto bytesParam = make_shared<FormalParameter>(
            std::string("bytes"), arrayType);
        bytesParam->setParent(shared_from_this());
        parameterList.push_back(bytesParam);
        parameters[bytesParam->getName()] = bytesParam;
    }

    void SynthesizedEncodingCtor::generateCode() {
        // Idempotent — Method-iteration paths can call generateCode
        // multiple times; bail if we've already emitted the body.
        if (llvmBasicBlock != nullptr) return;
        // Body:
        //   tmp = MyEncoder.decode(bytes)
        //   memcpy(this, tmp, sizeof(parent))
        //   ret void
        // The decode() function is a static method on the encoder class
        // returning a fresh `parent`. memcpy copies vtable + fields
        // verbatim; tmp's shell leaks (v1).
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        // Look up decode by canonical-name match — param must be int8[].
        MethodPtr decodeMethod = findStaticUnaryMethod(
            encoder, "decode", std::string("int8[]"));
        if (!decodeMethod) {
            throw Exception(
                "@Encoding synthesizer: encoder class `"
                + encoder->getQName()->toCanonical()
                + "` has no `public static T decode(byte[])` method "
                "with T = `" + parent->getQName()->toCanonical()
                + "`. Add one, or remove @Encoding from `"
                + parent->getQName()->toCanonical() + "`.",
                "CAJETA_ERROR_ENCODING_DECODE_MISSING");
        }
        llvm::Function* decodeFn = decodeMethod->getLlvmFunction();
        if (!decodeFn) {
            throw Exception(
                "@Encoding synthesizer: decode method has no LLVM function",
                "CAJETA_ERROR_ENCODING_DECODE_NOFN");
        }
        decodeFn = CajetaModule::ensureFunctionInModule(lmod, decodeFn);

        llvm::Value* thisPtr = llvmFunction->getArg(0);
        llvm::Value* bytes   = llvmFunction->getArg(1);

        // Call decode(bytes) — returns ptr to fresh T.
        llvm::Value* tmp = b.CreateCall(decodeFn, {bytes}, "enc.decoded");

        // memcpy(this, tmp, sizeof(parent)).
        llvm::Type* parentTy = parent->getLlvmType();
        uint64_t parentSize = dl.getTypeAllocSize(parentTy);
        llvm::Function* memcpyFn = llvm::Intrinsic::getDeclaration(
            lmod, llvm::Intrinsic::memcpy,
            {ptrTy, ptrTy, i64Ty});
        b.CreateCall(memcpyFn, {
            thisPtr,
            tmp,
            llvm::ConstantInt::get(i64Ty, parentSize),
            llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 0)
        });

        // Free the decoded temp's shell — its field POINTERS were
        // copied verbatim into `this`'s slots by the memcpy above, so
        // ownership of the fields transferred to `this`. The shell
        // itself (vtable slot + the inline portions of any value
        // fields) is leaked memory if not freed. __cajeta_free claims
        // tmp from the live-set + raw-frees the shell without
        // walking fields, so the now-aliased field allocations stay
        // alive under `this`'s ownership and get dropped exactly once
        // when `this` does.
        llvm::FunctionType* freeFnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {ptrTy}, false);
        llvm::FunctionCallee freeFn = lmod->getOrInsertFunction(
            "__cajeta_free", freeFnTy);
        b.CreateCall(freeFn, {tmp});

        b.CreateRetVoid();
    }

    // ---- toBytes() ------------------------------------------------------

    SynthesizedEncodingToBytes::SynthesizedEncodingToBytes(
            CajetaModulePtr module, CajetaClassPtr parent,
            CajetaClassPtr encoder)
        // Return type is byte[]. Find/build the byte[] type.
        : Method(module, std::string("toBytes"),
                 [&]() -> CajetaTypePtr {
                     for (auto& [n, t] : CajetaType::getCanonicalMap()) {
                         if (auto arr = dynamic_pointer_cast<CajetaArray>(t)) {
                             auto et = arr->getElementType();
                             if (et && et->getQName()
                                     && et->getQName()->getTypeName() == "int8") {
                                 return t;
                             }
                         }
                     }
                     // Build one if missing
                     return std::static_pointer_cast<CajetaType>(
                         make_shared<CajetaArray>(module, CajetaType::of("int8")));
                 }(),
                 parent),
          encoder(encoder) {
        this->parent = parent;
        // The encoder's encode returns a fresh byte[] — we pass it
        // through. Mark our return as ownership-transferring so the
        // ReturnStatement check accepts it. Without this, the
        // pass-through would still need # because encode itself
        // returns #byte[].
        this->setReturnsOwnership(true);
    }

    void SynthesizedEncodingToBytes::generateCode() {
        if (llvmBasicBlock != nullptr) return;
        // Body: return MyEncoder.encode(this)
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();

        // Look up encode by canonical-name match — param is the
        // parent class (T in the @Encoding contract). Accept either
        // short typeName or full canonical so an encoder author can
        // write `encode(UserMessage)` whether or not they imported
        // the type with its full package qualifier.
        std::string parentCanon = parent->getQName()
            ? parent->getQName()->toCanonical() : "";
        MethodPtr encodeMethod = findStaticUnaryMethod(
            encoder, "encode", parentCanon);
        if (!encodeMethod) {
            throw Exception(
                "@Encoding synthesizer: encoder class `"
                + encoder->getQName()->toCanonical()
                + "` has no `public static byte[] encode(T)` method "
                "with T = `" + parent->getQName()->toCanonical()
                + "`. Add one, or remove @Encoding from `"
                + parent->getQName()->toCanonical() + "`.",
                "CAJETA_ERROR_ENCODING_ENCODE_MISSING");
        }
        llvm::Function* encodeFn = encodeMethod->getLlvmFunction();
        if (!encodeFn) {
            throw Exception(
                "@Encoding synthesizer: encode method has no LLVM function",
                "CAJETA_ERROR_ENCODING_ENCODE_NOFN");
        }
        encodeFn = CajetaModule::ensureFunctionInModule(lmod, encodeFn);

        llvm::Value* thisPtr = llvmFunction->getArg(0);
        llvm::Value* result = b.CreateCall(encodeFn, {thisPtr}, "enc.bytes");
        b.CreateRet(result);
    }
}
