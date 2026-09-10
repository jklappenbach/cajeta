// Stdlib prime-once / restore-many core: the shared LLVMContext, the prime Compiler
// and the captured baseline the warm-lint path reuses. Single-threaded by
// construction — one instance per process, primed and restored from one thread.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>

#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LLVMContext.h>

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"

namespace cajeta {

    class StdlibReuseCore {
    public:
        static StdlibReuseCore& instance();

        // Prime once (idempotent): bind the shared context, build the prime Compiler,
        // front-end the stdlib with xref capture on, and capture the baseline. Leaves
        // Compiler's shared context SET; the caller clears it when its reuse scope ends.
        void ensurePrimed();

        // Ensure the stdlib is primed AND has had `layer` applied, re-capturing the
        // baseline after; restoring to the pristine state first makes the order free.
        void ensureCodegenLayer(const std::function<void(Compiler&)>& layer);

        // Reset every captured global to the post-prime baseline: epoch, registries,
        // stdlib structures, lazy bookkeeping, stdlib method-template instantiations.
        // xref logs restore separately, via xref::resetCapture.
        void restoreBaseline();

        // A SECOND baseline slot holding "stdlib + the sibling sweep", so a warm request
        // can skip the sweep. Capture right after registerLintContext and before the
        // target is parsed, so the snapshot excludes the target; invalidate forces a resweep.
        void captureContextBaseline();
        void restoreContextBaseline();
        void invalidateContextBaseline();
        bool contextBaselineValid() const { return hasContextBaseline; }

        llvm::LLVMContext* context() { return &sharedContext; }
        Compiler* primeCompiler() { return prime.get(); }
        CajetaModulePtr getStdlibModule() { return stdlibModule; }
        bool primed() const { return isPrimed; }
        bool codegenLayered() const { return isCodegenLayered; }

    private:
        StdlibReuseCore() = default;
        void captureBaselines();
        // Baseline of the persistent stdlib llvm::Module's global values. A session's
        // additions (external declarations of what it emitted) must not outlive it, or
        // the next session fails to materialize symbols nothing defines.
        void captureLlvmBaseline();
        void restoreLlvmBaseline();
        void pruneAppendingGlobal(llvm::Module& m, const char* name,
                                  const std::set<llvm::GlobalValue*>& surviving);

        llvm::LLVMContext sharedContext;
        std::unique_ptr<Compiler> prime;   // owns the TargetMachine the stdlib references
        CajetaModulePtr stdlibModule;
        std::map<std::string, CajetaClassPtr> baselineStructures;
        std::set<llvm::GlobalValue*> baselineLlvmValues;
        bool isPrimed = false;
        bool isCodegenLayered = false;

        // Sibling-context slot: stdlib structures + lazy state at capture time; the
        // type/module registries keep their own second slots.
        std::map<std::string, CajetaClassPtr> contextStructures;
        Compiler::LazyStdlibState contextLazyState;
        bool hasContextBaseline = false;
    };

} // namespace cajeta
