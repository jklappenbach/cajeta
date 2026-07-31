// Warm in-process lint (lint-server plan Unit 1, spec §3).
//
// runLintDriver is the one-shot `--lint` driver factored out of main.cpp —
// engine lifecycle, exception folding, emit order — so the warm path and the
// one-shot CLI execute the SAME code and parity (spec 1.4.1) holds by
// construction. warmLint is what the `--lint-server` loop (Unit 2) calls per
// request: restore the stdlib baseline, run the driver on a fresh
// shared-context Compiler.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
        // Dependency archives, the warm equivalent of `--classpath=a.cja,b.cja`.
        // Server-level in practice (the loop copies ServerOptions::classpath
        // into every request), but carried per-request so warmLint is drivable
        // standalone. Empty = no dependencies.
        std::vector<std::string> classpath;
    };

    // The one-shot lint driver: collect-and-continue engine, exception
    // folding, diagnostics then xref stream, all on stderr. Returns the
    // process exit code (0 clean, 1 diagnostics/syntax error). The caller
    // owns flag validation (main.cpp) and Compiler construction.
    //
    // skipContextRegistration / afterContextRegistration forward to
    // Compiler::lint for the warm sibling-context path (spec §4); one-shot
    // callers leave them defaulted.
    int runLintDriver(Compiler& compiler, const std::string& file,
                      const std::string& sourceRoot, const std::string& shadow,
                      bool skipContextRegistration = false,
                      const std::function<void()>& afterContextRegistration = {});

    // Warm sibling context for one project root (spec §4). Persists across
    // requests in the server. `excluded` is the single under-root path the
    // sweep omits (the target, or its --shadow original); `stamps` is every
    // swept sibling's (mtime, size). A request is served warm iff the root,
    // that exclusion, and every stamp match — otherwise the root is reswept
    // and this is refreshed.
    struct SiblingContext {
        std::string root;
        std::set<std::string> excluded;
        std::map<std::string, std::pair<std::int64_t, std::uintmax_t>> stamps;
        // The classpath the context baseline was captured under. A server
        // fixes it at startup, so this normally never changes; comparing it
        // anyway keeps a direct caller from serving a context whose dependency
        // declarations belong to a different classpath.
        std::vector<std::string> classpath;
        bool valid = false;
    };

    struct LintOutcome {
        int rc = 0;
        int siblingsReparsed = 0;   // 0 on a warm hit; the swept count on a resweep
    };

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

    // The server's per-request entry: like warmLint, but reuses the sibling
    // context across requests (spec §4). On a warm hit (root + exclusion +
    // every sibling stamp unchanged) it restores the context baseline and
    // skips the sweep (siblingsReparsed = 0); otherwise it resweeps the root,
    // re-captures the context baseline, and reports the swept count. `ctx` is
    // persistent server state, updated in place.
    LintOutcome warmLintServed(const LintRequest& req, SiblingContext& ctx);

    struct ServerOptions {
        std::string sourceRoot;   // --source-root: shared across requests
        // --classpath dependency archives: shared across requests, like the
        // source root. The plugin keys one server per (compiler, root,
        // classpath), so this is fixed for the process's life.
        std::vector<std::string> classpath;
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
