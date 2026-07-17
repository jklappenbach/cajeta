// Warm in-process lint (lint-server plan Unit 1, spec §3).
//
// runLintDriver is the one-shot `--lint` driver factored out of main.cpp —
// engine lifecycle, exception folding, emit order — so the warm path and the
// one-shot CLI execute the SAME code and parity (spec 1.4.1) holds by
// construction. warmLint is what the `--lint-server` loop (Unit 2) calls per
// request: restore the stdlib baseline, run the driver on a fresh
// shared-context Compiler.
#pragma once

#include <string>

namespace cajeta {
    class Compiler;
}

namespace cajeta::lintservice {

    struct LintRequest {
        std::string file;         // the (possibly staged) buffer to lint
        std::string sourceRoot;   // empty = no --source-root
        std::string shadow;       // empty = no --shadow
        bool jsonDiagnostics = true;
        bool emitXref = false;    // bare --emit-xref: stream on the diagnostic channel
    };

    // The one-shot lint driver: collect-and-continue engine, exception
    // folding, diagnostics then xref stream, all on stderr. Returns the
    // process exit code (0 clean, 1 diagnostics/syntax error). The caller
    // owns flag validation (main.cpp) and Compiler construction.
    int runLintDriver(Compiler& compiler, const std::string& file,
                      const std::string& sourceRoot, const std::string& shadow);

    // One warm lint request: prime the stdlib once per process (front-end
    // only), restore the baseline, and run the driver on a fresh Compiler
    // bound to the shared context. Output and exit code match a fresh
    // one-shot `cajeta --lint` of the same input byte-for-byte. Serial use
    // only, from the priming thread (spec §1.5). The shared context is
    // cleared on exit, so Compilers built after a call stay fully isolated.
    int warmLint(const LintRequest& req);

} // namespace cajeta::lintservice
