// Stdlib prime-once / restore-many core (lint-server plan 1.2.1).
//
// The in-process stdlib-reuse machinery the JIT test harness proved
// (test/jit/JitTestHelper.cpp, StdlibReuseCache), factored into src/ so the
// production warm-lint path (`cajeta --lint-server`) can use it. This core
// owns the shared LLVMContext, the prime Compiler, and the captured
// baseline; the test cache LAYERS its JIT-specific needs (codegen passes,
// llvm-level pristineness snapshots, the shared stdlib dylib) on top of it
// rather than duplicating any of this.
//
// Single-threaded by construction: one instance per process, primed and
// restored from one thread (the lint server is serial — spec §1.5; the JIT
// harness confines reuse to the priming thread). The underlying baselines
// (CajetaType / CajetaModule / xref) are thread_local, so an off-owner
// thread simply sees no baseline and takes the fresh, isolated path.
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

        // Prime once (idempotent): bind the shared context, build the prime
        // Compiler, front-end the stdlib (parse + prototype layout — NO
        // codegen passes; lint stops before codegen and never needs them),
        // and capture the post-prime baseline. The stdlib parse runs with
        // xref capture enabled so the capture baseline holds what a fresh
        // one-shot `--lint --emit-xref` run's stdlib parse would log.
        //
        // Leaves Compiler's shared context SET (pointing at context()); the
        // caller owns clearing it (Compiler::setSharedContext(nullptr)) when
        // its reuse scope ends, so unrelated Compilers stay fully isolated.
        void ensurePrimed();

        // JIT layering (the test cache): ensure the stdlib is primed AND has
        // had `layer` applied (e.g. the codegen passes), re-capturing the
        // baseline afterwards so restores return to the post-layer state.
        // Safe whichever ran first: if front-end-only lint requests already
        // used the core, the state is restored to the pristine post-front-end
        // baseline before the layer runs.
        void ensureCodegenLayer(const std::function<void(Compiler&)>& layer);

        // Reset every captured global to the post-prime baseline: reuse
        // epoch, type/module registries (incl. the prescan archive and
        // placeholder records), stdlib structures, lazy-stdlib bookkeeping,
        // and method-template instantiations registered on stdlib classes.
        // xref logs restore separately via xref::resetCapture (which the
        // lint path already calls per run).
        void restoreBaseline();

        // lint-server sibling-context reuse (spec §4). captureContextBaseline
        // snapshots "stdlib + the sibling sweep" (type + module registries,
        // the stdlib module's structures incl. any lazy package a sibling
        // pulled in, and the lazy bookkeeping) into a SECOND slot, independent
        // of the pristine stdlib baseline. restoreContextBaseline reinstates it
        // on a warm request — skipping the sweep — and drops the previous
        // request's target (its entries aren't in the snapshot). invalidate
        // forces the next request to resweep. Call capture right after
        // registerLintContext and before the target is parsed, so the snapshot
        // excludes the target.
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
        // The persistent stdlib llvm::Module's own baseline (spec §4.1): the
        // set of global values present after priming. A session adds to that
        // module — notably `external global` declarations of instantiations it
        // emitted into ITS user module — and those additions must not outlive
        // it, or the next session fails to materialize symbols nothing
        // defines. See the definitions for the full account.
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

        // Sibling-context slot (spec §4): the stdlib module structures + lazy
        // state at capture time; the type/module registries live in their own
        // second slots (CajetaType/CajetaModule::*ContextBaseline).
        std::map<std::string, CajetaClassPtr> contextStructures;
        Compiler::LazyStdlibState contextLazyState;
        bool hasContextBaseline = false;
    };

} // namespace cajeta
