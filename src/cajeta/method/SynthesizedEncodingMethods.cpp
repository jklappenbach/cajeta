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

    // The static one-parameter `name` on `cls` whose parameter type matches
    // `wantCanon`, or nullptr. A static's parameterList carries no implicit `this`.
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
            if (params.size() != 1 || !params[0]) continue;
            auto pt = params[0]->getType();
            if (!pt) continue;
            std::string pCanon = pt->getQName()
                ? pt->getQName()->toCanonical() : "";
            // Short or canonical: the encoder author may or may not have imported it.
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
        // The parameter is byte[]: reuse the canonical int8[] when one exists, so
        // the synthesized signature matches every other byte[] in the program.
        auto byteType = CajetaType::of("int8");  // byte == int8 in Cajeta
        auto& canonicalMap = CajetaType::getCanonicalMap();
        CajetaTypePtr arrayType;
        auto it = canonicalMap.find("int8[]");
        if (it != canonicalMap.end()) arrayType = it->second;
        if (!arrayType) {
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
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        // Idempotent: Method iteration can reach generateCode more than once.
        if (llvmBasicBlock != nullptr) return;
        // Emits `memcpy(this, MyEncoder.decode(bytes), sizeof(parent))`: the static
        // decode returns a fresh `parent`, whose vtable and fields copy verbatim.
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();
        const llvm::DataLayout& dl = lmod->getDataLayout();

        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

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

        // A trailing 0 transfer word when the target's ABI carries one, or the
        // verifier rejects the argument count.
        std::vector<llvm::Value*> decodeArgs{bytes};
        if (decodeMethod->needsTransferWord()) {
            decodeArgs.push_back(llvm::ConstantInt::get(i64Ty, 0));
        }
        llvm::Value* tmp = b.CreateCall(decodeFn, decodeArgs, "enc.decoded");

        llvm::Type* parentTy = parent->getLlvmType();
        uint64_t parentSize = dl.getTypeAllocSize(parentTy);
        llvm::Function* memcpyFn = llvm::Intrinsic::getOrInsertDeclaration(
            lmod, llvm::Intrinsic::memcpy,
            {ptrTy, ptrTy, i64Ty});
        b.CreateCall(memcpyFn, {
            thisPtr,
            tmp,
            llvm::ConstantInt::get(i64Ty, parentSize),
            llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 0)
        });

        // The memcpy moved the temp's field pointers into `this`, so only the shell
        // is left to reclaim. __cajeta_free raw-frees it WITHOUT walking fields, which
        // is what leaves the now-aliased allocations alive under `this`.
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
                     return std::static_pointer_cast<CajetaType>(
                         make_shared<CajetaArray>(module, CajetaType::of("int8")));
                 }(),
                 parent),
          encoder(encoder) {
        this->parent = parent;
        // encode() returns an owned byte[] this passes straight through, so the
        // return must transfer too or the ReturnStatement check rejects it.
        this->setReturnsOwnership(true);
    }

    void SynthesizedEncodingToBytes::generateCode() {
        auto& llvmFunction = llvmFunctionRef();  // U6.3b: frozen-aware
        if (llvmBasicBlock != nullptr) return;
        llvm::LLVMContext& ctx = *module->getLlvmContext();
        llvmBasicBlock = llvm::BasicBlock::Create(ctx, "entry", llvmFunction);
        llvm::IRBuilder<> b(llvmBasicBlock);
        llvm::Module* lmod = module->getLlvmModule();

        // encode's parameter is the parent class, matched short or canonical.
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
        // The same trailing transfer word as the decode call above.
        std::vector<llvm::Value*> encodeArgs{thisPtr};
        if (encodeMethod->needsTransferWord()) {
            encodeArgs.push_back(llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx), 0));
        }
        llvm::Value* result = b.CreateCall(encodeFn, encodeArgs, "enc.bytes");
        b.CreateRet(result);
    }
}
