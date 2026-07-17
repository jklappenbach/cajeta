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
    //
    // The diagnostics + xref that a one-shot run writes to stderr go to
    // stderr here too (fd 2). The server loop redirects fd 2 per request to
    // capture that payload; a direct caller sees it on its own stderr.
    int warmLint(const LintRequest& req);

    struct ServerOptions {
        std::string sourceRoot;   // --source-root: shared across requests
        bool jsonDiagnostics = true;
    };

    // The `cajeta --lint-server` loop (spec §2). Prime the stdlib once,
    // print the ready record (proto version) on stdout, then read NDJSON
    // requests on stdin and answer each with the one-shot lint's payload
    // lines VERBATIM (captured from the per-request stderr) bracketed by a
    // `{"kind":"done","id":<n>}` marker. Malformed lines, unknown kinds,
    // and unlintable files get a `{"kind":"error",...}` record and the
    // server keeps serving. stdin EOF or a `{"kind":"shutdown"}` record
    // exits 0. Requests are handled serially in arrival order (§1.5).
    // Returns the process exit code (0 on clean shutdown/EOF).
    int runLintServer(const ServerOptions& opts);

} // namespace cajeta::lintservice
