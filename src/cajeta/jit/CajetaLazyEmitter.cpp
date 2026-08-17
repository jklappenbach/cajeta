#include "cajeta/jit/CajetaLazyEmitter.h"

#include "cajeta/compile/CajetaModule.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace cajeta {

    llvm::Expected<llvm::orc::ThreadSafeModule>
    emitMethodModule(const MethodPtr& method) {
        if (!method) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "lazy emit: null method");
        }
        const std::string sym = method->getLlvmSymbolName();

        // The same two calls the eager fixpoint makes, for one method.
        method->getLlvmFunctionType();
        method->generateCode();

        CajetaModulePtr parent = method->getModule();
        llvm::Module* live = parent ? parent->getLlvmModule() : nullptr;
        if (!live) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "lazy emit: '%s' has no parent llvm::Module", sym.c_str());
        }
        llvm::Function* fn = live->getFunction(sym);
        if (!fn || fn->isDeclaration()) {
            // spec 5.3 — distinguishable from an ordinary missing symbol: the
            // method was indexed but generateCode() left no definition.
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "lazy emit: generateCode() left no body for '%s'", sym.c_str());
        }

        // One definition, everything else declarations. Local-linkage globals
        // must come along whole — a private constant cannot be declared across
        // modules — and duplicating private data per snapshot is harmless.
        llvm::ValueToValueMapTy vmap;
        auto clone = llvm::CloneModule(
            *live, vmap, [&](const llvm::GlobalValue* gv) {
                return gv == fn || gv->hasLocalLinkage();
            });
        clone->setModuleIdentifier("lazy:" + sym);

        // Dylib-init work is the EAGER remainder (spec 2.2): a snapshot that
        // carried llvm.global_ctors would re-run class registration on every
        // delivered body.
        if (auto* ctors = clone->getNamedGlobal("llvm.global_ctors"))
            ctors->eraseFromParent();
        if (auto* dtors = clone->getNamedGlobal("llvm.global_dtors"))
            dtors->eraseFromParent();

        // Snapshot: bitcode round-trip into a context ORC owns, so the live
        // module stays the front-end's to mutate.
        llvm::SmallVector<char, 0> buf;
        {
            llvm::raw_svector_ostream os(buf);
            llvm::WriteBitcodeToFile(*clone, os);
        }
        auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef(buf.data(), buf.size()),
            clone->getModuleIdentifier());
        auto tsCtx = std::make_unique<llvm::LLVMContext>();
        llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
        auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
            return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), *ctx);
        });
        if (!parsed) return parsed.takeError();
        return llvm::orc::ThreadSafeModule(std::move(*parsed),
                                           std::move(tsContext));
    }

} // namespace cajeta
