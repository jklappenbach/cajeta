// See LintService.h.

#include "cajeta/compile/LintService.h"

#include <exception>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CompilerMode.h"
#include "cajeta/compile/StdlibReuseCore.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "cajeta/error/Exception.h"
#include "cajeta/type/CajetaType.h"

namespace cajeta::lintservice {

    int runLintDriver(Compiler& compiler, const std::string& file,
                      const std::string& sourceRoot, const std::string& shadow) {
        const bool jsonDiag =
            compiler.getFlags().diagFormat == DiagFormat::Json;
        // Collect-and-continue: recoverable semantic errors report to the
        // engine (multiple, located) instead of aborting; un-migrated throw
        // sites are caught and folded in. All emitted at the end
        // (diagnostic-engine-spec).
        DiagnosticEngine engine;
        DiagnosticEngine::setActive(&engine);
        try {
            compiler.lint(file, sourceRoot, shadow);
        } catch (SyntaxErrorException&) {
            DiagnosticEngine::setActive(nullptr);
            // The buffer did not parse, so nothing was captured for it — the
            // stream is a version line and records for nothing, which is the
            // signal for the plugin to KEEP its previous index for this file
            // (a stale answer beats a wrong one; ide-symbol-index §7).
            compiler.emitLintXrefStream(file, sourceRoot, shadow);
            return 1;  // syntax diagnostics already emitted during parsing
        } catch (Exception& e) {
            engine.report("error", e.getErrorId(), e.getMessage(),
                          e.getFile(), e.getLine(), e.getColumn());
        } catch (const std::exception& e) {
            engine.report("error", "", e.what());
        }
        DiagnosticEngine::setActive(nullptr);
        engine.emit(jsonDiag);
        compiler.emitLintXrefStream(file, sourceRoot, shadow);
        return engine.hasErrors() ? 1 : 0;
    }

    int warmLint(const LintRequest& req) {
        auto& core = StdlibReuseCore::instance();
        // Clear the shared context on EVERY exit — normal return and
        // exception alike — so a Compiler constructed after this call takes
        // the fresh, fully-isolated path (the JIT harness discipline; a
        // stale shared context makes an unrelated Compiler skip
        // resetGlobals and inherit this request's registries).
        struct SharedContextGuard {
            ~SharedContextGuard() { Compiler::setSharedContext(nullptr); }
        } guard;

        core.ensurePrimed();
        core.restoreBaseline();
        Compiler::setSharedContext(core.context());

        // Fresh Compiler per request: per-instance state (`modules`,
        // `externalModules`, target machine) starts empty, exactly as a
        // fresh process's would — the reset seam for everything the global
        // baselines don't cover (plan 1.2.2/1.2.3).
        Compiler compiler;
        compiler.getMutableFlags().diagFormat =
            req.jsonDiagnostics ? DiagFormat::Json : DiagFormat::Text;
        compiler.getMutableFlags().emitXref = req.emitXref ? "-" : "";

        int rc = runLintDriver(compiler, req.file, req.sourceRoot, req.shadow);

        // Release this request's USER struct-type names from the shared
        // context while canonicalMap still records them (the next restore
        // drops the record). LLVM struct types are context-owned and outlive
        // the request's modules; a later request re-declaring the same class
        // name would otherwise find the stale, already-bodied struct and
        // double-set / mis-layout it. Stdlib-resident names are preserved.
        CajetaType::releaseThrownTransientStructNames();
        return rc;
    }

} // namespace cajeta::lintservice
