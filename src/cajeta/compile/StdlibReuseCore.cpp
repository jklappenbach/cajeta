// See StdlibReuseCore.h. The prime/restore sequences here are the ones the
// JIT test harness proved under the full suite (StdlibReuseCache); comments
// on the non-obvious steps live with the machinery they guard.

#include "cajeta/compile/StdlibReuseCore.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xref/XrefIndex.h"

namespace cajeta {

    StdlibReuseCore& StdlibReuseCore::instance() {
        static StdlibReuseCore core;
        return core;
    }

    void StdlibReuseCore::ensurePrimed() {
        if (isPrimed) return;
        Compiler::setSharedContext(&sharedContext);
        // Arm xref capture for the stdlib parse: template members are only
        // recordable AS the stdlib parses, and a warm lint that asks for the
        // xref stream needs them in its restored logs exactly as a fresh
        // process (which parses the stdlib with capture armed) would have
        // them. Costs a few vectors of records when no one ever asks.
        xref::resetCapture();
        xref::setCaptureEnabled(true);
        // First Compiler under the shared context primes the global type
        // tables (resetGlobals + init) in that context.
        prime = std::make_unique<Compiler>();
        prime->ensureStdlibModule();   // front-end: parse + prototype layout
        xref::setCaptureEnabled(false);
        stdlibModule = CajetaModule::getStdlibModule();
        captureBaselines();
        isPrimed = true;
    }

    void StdlibReuseCore::ensureCodegenLayer(
            const std::function<void(Compiler&)>& layer) {
        ensurePrimed();
        if (isCodegenLayered) return;
        // Front-end-only users (warm lint) may have run requests already;
        // return to the pristine post-front-end state before layering.
        restoreBaseline();
        Compiler::setSharedContext(&sharedContext);
        layer(*prime);
        captureBaselines();
        isCodegenLayered = true;
    }

    void StdlibReuseCore::captureBaselines() {
        CajetaType::captureBaseline();
        CajetaModule::captureBaseline();
        baselineStructures = stdlibModule->getStructures();
        // Snapshot each baseline stdlib class's module-bound llvm bindings
        // (drop/vtable/RTTI/static-field globals + method functions) so
        // restoreBaseline can reset any that a reusing run lazily generated
        // into its own per-run module — the cross-module-reference leak.
        for (auto& [canon, klass] : baselineStructures)
            if (klass) klass->captureReuseBaseline();
        xref::captureBaseline();
    }

    void StdlibReuseCore::restoreBaseline() {
        if (!isPrimed) return;
        // Advance the reuse generation so per-template method-instantiation
        // caches (held on persistent stdlib Methods) invalidate stale entries
        // bound to the previous run's now-freed user emit module.
        CajetaModule::bumpReuseEpoch();
        CajetaType::restoreBaseline();
        CajetaModule::restoreBaseline();   // re-pins the stdlib singleton
        stdlibModule->getStructures() = baselineStructures;
        // Lazy stdlib: the baseline was captured before any on-demand package
        // (cajeta.math) was parsed, and the restore above drops those types.
        // Clear the lazy bookkeeping too, so a later run importing the package
        // re-parses it instead of skipping it as "already parsed".
        Compiler::resetLazyStdlibState();
        // Drop every method-template instantiation a PRIOR run registered on
        // a persistent stdlib class. Such a class outlives the per-run user
        // module its instantiations were codegen'd into; left registered, the
        // next run's resolveMethod finds the stale entry and skips re-emitting
        // the body into ITS module. Collect-then-remove to avoid mutating the
        // maps mid-iteration.
        for (auto& [canonical, klass] : stdlibModule->getStructures()) {
            if (!klass) continue;
            std::vector<MethodPtr> stale;
            for (auto& m : klass->getMethodList()) {
                if (m && m->isMethodTemplateInstantiation()) stale.push_back(m);
            }
            for (auto& m : stale) klass->removeMethod(m);
            // Reset this class's module-bound llvm bindings that a reusing
            // run generated into its own per-run module, so the next run
            // regenerates into its module rather than referencing a freed one.
            klass->restoreReuseBaseline();
        }
    }

} // namespace cajeta
