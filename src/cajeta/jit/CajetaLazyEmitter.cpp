#include "cajeta/jit/CajetaLazyEmitter.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/jit/JitModulePrep.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace cajeta {

    namespace {

        // The shared snapshot tail: one definition cloned out of the live
        // module, prepared exactly as the eager delivery path prepares whole
        // modules (spec 4.4 — the passes hold over the lazy set because they
        // run on every snapshot), then bitcode-round-tripped into a context
        // ORC owns so the live module stays the front-end's to mutate.
        llvm::Expected<llvm::orc::ThreadSafeModule>
        snapshotOne(llvm::Module* live, const llvm::GlobalValue* keep,
                    const std::string& sym) {
            llvm::ValueToValueMapTy vmap;
            auto clone = llvm::CloneModule(
                *live, vmap, [&](const llvm::GlobalValue* gv) {
                    // Local-linkage globals come along whole — a private
                    // constant cannot be declared across modules, and
                    // duplicating private data per snapshot is harmless.
                    return gv == keep || gv->hasLocalLinkage();
                });
            clone->setModuleIdentifier("lazy:" + sym);

            // Instantiation emission can leave operands pointing at globals
            // homed in ANOTHER live module; the bitcode writer cannot express
            // that, so rewrite them to local declarations first (3.2.2).
            cajeta::jit::legalizeCrossModuleRefs(clone.get());
            // ODR: an eagerly delivered module or another snapshot may carry
            // the same instantiation symbol; weak_odr lets ORC pick one.
            cajeta::jit::demoteInstantiationsToWeakODR(clone.get());

            // Every declaration must lose dso_local: CloneModule copies it,
            // and a dso_local external is addressed with a direct 32-bit
            // PC-relative fixup — valid inside the eager world's single large
            // module, but a snapshot's externals live in OTHER snapshots that
            // JITLink may place >2 GB away ("out of range of Delta32 fixup",
            // seen on __cajeta_session_guard_frame at delivery #558).
            // Clearing it routes external addressing through the GOT, which
            // JITLink synthesizes with no range limit.
            for (llvm::GlobalValue& gv : clone->global_values()) {
                if (gv.isDeclaration()) gv.setDSOLocal(false);
            }

            // Dylib-init work is the EAGER remainder (spec 2.2): a snapshot
            // that carried llvm.global_ctors would re-run class registration
            // on every delivered body.
            if (auto* ctors = clone->getNamedGlobal("llvm.global_ctors"))
                ctors->eraseFromParent();
            if (auto* dtors = clone->getNamedGlobal("llvm.global_dtors"))
                dtors->eraseFromParent();

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
                return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                              *ctx);
            });
            if (!parsed) return parsed.takeError();
            return llvm::orc::ThreadSafeModule(std::move(*parsed),
                                               std::move(tsContext));
        }

    } // namespace

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
        return snapshotOne(live, fn, sym);
    }

    llvm::Expected<llvm::orc::ThreadSafeModule>
    snapshotLiveDefinition(llvm::GlobalValue* gv) {
        if (!gv || gv->isDeclaration()) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "lazy emit: no live definition");
        }
        return snapshotOne(gv->getParent(), gv, gv->getName().str());
    }

} // namespace cajeta
