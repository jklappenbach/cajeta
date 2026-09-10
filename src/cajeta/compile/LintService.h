// Warm in-process lint. runLintDriver is the one-shot `--lint` driver factored out
// of main.cpp, so the warm path and the CLI execute the SAME code and parity holds
// by construction; warmLint is what the `--lint-server` loop calls per request.
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
        // Dependency archives, the warm `--classpath=a.cja,b.cja`. Server-level in
        // practice, but per-request so warmLint drives standalone; dropping them
        // streams a dependency-free xref that clobbers the IDE's use-site shard.
        std::vector<std::string> classpath;
    };

    // The one-shot lint driver: collect-and-continue, then diagnostics and the xref
    // stream on stderr. Returns the process exit code; the caller owns flag
    // validation. The two trailing arguments serve the warm path only.
    int runLintDriver(Compiler& compiler, const std::string& file,
                      const std::string& sourceRoot, const std::string& shadow,
                      bool skipContextRegistration = false,
                      const std::function<void()>& afterContextRegistration = {});

    // Warm sibling context for one project root, persistent across requests.
    // `excluded` is the one under-root path the sweep omits; `stamps` is every
    // sibling's (mtime, size). Any mismatch resweeps the root and refreshes this.
    struct SiblingContext {
        std::string root;
        std::set<std::string> excluded;
        std::map<std::string, std::pair<std::int64_t, std::uintmax_t>> stamps;
        // Compared, so a context built for different dependencies is never served.
        std::vector<std::string> classpath;
        bool valid = false;
    };

    struct LintOutcome {
        int rc = 0;
        int siblingsReparsed = 0;   // 0 on a warm hit; the swept count on a resweep
    };

    // One warm request: prime the stdlib once per process, restore the baseline,
    // run the driver on a fresh shared-context Compiler. Output, stderr payload and
    // exit code match a one-shot run byte-for-byte. Serial, from the priming thread.
    int warmLint(const LintRequest& req);

    // warmLint reusing `ctx` across requests; a miss resweeps and re-captures it.
    LintOutcome warmLintServed(const LintRequest& req, SiblingContext& ctx);

    struct ServerOptions {
        std::string sourceRoot;   // --source-root: shared across requests
        // Shared across requests: one server is keyed per (compiler, root, classpath).
        std::vector<std::string> classpath;
        bool jsonDiagnostics = true;
    };

    // The `cajeta --lint-server` loop: a ready record on stdout, then NDJSON
    // requests from stdin answered with the one-shot payload lines VERBATIM and a
    // done marker. Errors keep serving; EOF or a shutdown record exits 0.
    int runLintServer(const ServerOptions& opts);

} // namespace cajeta::lintservice
