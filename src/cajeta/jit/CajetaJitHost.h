//
// In-process JIT host for the `cajeta` binary. Compiles a Cajeta project
// (every .cajeta under a source root) to in-memory LLVM IR, merges the
// embedded runtime + stdlib, builds an LLJIT, and runs a chosen static
// no-arg entry method inside this process.
//
// This productizes the proven pipeline from test/jit/JitTestHelper.cpp into a
// reusable component. The debugger (`cajeta dap`, future) drives the same host
// so breakpoints can park the executing fiber in-process. CP1 of the debugger
// plan: prove the host JIT-runs a program to completion.
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cajeta/dbg/DebugController.h"

namespace cajeta::jit {

    struct JitRunOptions {
        // Directory whose .cajeta files form the compilation unit. Each file's
        // path-derived package must match its `package` declaration (the normal
        // compiler convention).
        std::string sourceRoot;
        // Entry method in dotted `package.Class.method` form. Must be a static,
        // parameter-less method.
        std::string entryMethod;
        // Program arguments (reserved; not yet forwarded to the entry, which is
        // parameter-less for now).
        std::vector<std::string> programArgs;
        // Reserved for CP2+: emit __cajeta_dbg_safepoint polls + debug frames.
        bool debugInfo = false;
        // Whole-program JIT cache root (fast-debug-launch 4.2.1). Non-empty
        // enables the cache: a slot keyed on compiler version+flags+entry+
        // source digests holds the MERGED program bitcode + sidecars, and a
        // launch whose key matches loads it without constructing a Compiler.
        // Empty = today's full compile, unconditionally.
        std::string cacheDir;
        // Optional build-progress callback (fast-debug-launch 1.2.2). Invoked
        // from the calling thread at phase boundaries — phase is one of
        // "collect", "parse", "codegen", "finalize", "merge", "jit" — and once
        // per source during "parse" with detail = the source's root-relative
        // path and current/total = 1-based source counts. Boundary events
        // carry current = total = 0. Unset = no calls, no behavior change.
        std::function<void(const std::string& phase, const std::string& detail,
                           int current, int total)> onProgress;
    };

    // Wall-clock breakdown of one buildJit run (fast-debug-launch 1.2.1).
    // The named phases are disjoint segments of the same interval, so their
    // sum never exceeds totalSeconds (loop bookkeeping between segments is
    // deliberately unattributed). Codegen is split by module owner: the one
    // process-wide stdlib module vs everything else — the split that decides
    // whether stdlib cache slots (plan Unit 7) are worth building.
    struct JitBuildPhases {
        double collectSeconds = 0;        // fs walk + compiler setup + prescan
        double parseSeconds = 0;          // per-source parse (incl. lazy stdlib)
        double codegenStdlibSeconds = 0;  // method codegen, stdlib module
        double codegenUserSeconds = 0;    // method codegen, user modules
        double finalizeSeconds = 0;       // REFL-2 + drop backfill/pin
        double mergeSeconds = 0;          // linkModules donor merge
        double jitSeconds = 0;            // verify + LLJIT build/initialize
        double totalSeconds = 0;          // whole buildJit wall time

        // Sub-phase splits (fast-debug-launch 7.2.1) — each a SUBSET of the
        // parent segment above, measured inside it. They locate the
        // edit-relaunch cost the Stage-B rescope targets.
        double parseStdlibSeconds = 0;    // ensureStdlibModule + lazy pkg parses
        double jitSerializeSeconds = 0;   // WriteBitcodeToFile (cold only)
        double jitReparseSeconds = 0;     // parseBitcodeFile round-trip/load
        double jitMaterializeSeconds = 0; // LLJIT initialize (incl. codegen)
    };

    // Optional diagnostics filled by runJit when a non-null result is passed.
    // Used by the debugger TDD harness to verify safepoint emission/execution.
    struct JitRunResult {
        // __cajeta_dbg_safepoint call sites emitted INSIDE the entry function
        // (static count from the IR — deterministic, one per statement).
        int entrySafepointsEmitted = 0;
        // __cajeta_dbg_safepoint calls EXECUTED during the entry's run (the JIT
        // module's counter, reset immediately before invoking the entry so it
        // measures only the entry's execution, not global ctors).
        long safepointsExecuted = 0;
        // Build-phase wall-clock breakdown (fast-debug-launch 1.2.1).
        JitBuildPhases phases;
        // True when this launch was served from the whole-program cache slot
        // (fast-debug-launch 4.1.1) — no Compiler was constructed.
        bool cacheHit = false;
        // True when EVERY module materialized from the content-addressed
        // object pool (fast-debug-launch 6.1.1) — no instruction selection.
        bool objectCacheHit = false;
        // Per-module delivery counters (resident-debug-server 2.1.3):
        // objects served from the pool vs compiled this launch. Zero when
        // cacheDir is unset (no pools).
        int moduleObjectsServed = 0;
        int moduleObjectsCompiled = 0;
    };

    // Compile + JIT + run. Returns the process-style exit code (0 on success,
    // non-zero on a compile/JIT/lookup failure). Diagnostics go to stderr.
    // When `result` is non-null it is populated (see JitRunResult).
    int runJit(const JitRunOptions& opts, JitRunResult* result = nullptr);

    // Convert a dotted `package.Class.method` entry into the cajeta-mangled
    // function-name prefix `package.Class::method` (the IR appends the
    // parameter list in parentheses). Returns "" when `dotted` has no trailing
    // `.method` segment. Pure; exposed for unit testing.
    std::string entryTargetFromDotted(const std::string& dotted);

    // CLI entry: `cajeta jit-run <sourceRoot> <entryMethod> [programArgs...]`.
    // A developer/diagnostic verb that exercises the JIT host headlessly; the
    // full `cajeta dap` server (later) reuses runJit().
    int dispatchJitRun(int argc, const char* argv[]);

    // --- Debug sessions (CP3) ---------------------------------------------
    // A source-line breakpoint, matched against emitted safepoints by file
    // BASENAME + line (so an absolute compiled path matches a bare file name).
    struct Breakpoint {
        std::string file;   // e.g. "Calc.cajeta"
        int line = 0;
    };

    // A running debug session: the program is compiled with --debug-info,
    // breakpoints are armed, and the entry runs on a BACKGROUND thread wired to
    // an in-process DebugController. The caller (the DAP server, or a test)
    // drives controller().waitForStop()/resume() from another thread, then
    // join()s for the exit code. The session keeps the Compiler + LLJIT alive
    // for the program's lifetime. CP4's `cajeta dap` server is built on this.
    class JitDebugSession {
    public:
        struct Impl;
        explicit JitDebugSession(std::unique_ptr<Impl> impl);
        ~JitDebugSession();
        JitDebugSession(const JitDebugSession&) = delete;
        JitDebugSession& operator=(const JitDebugSession&) = delete;

        // The controller driving stop/resume. Stable address for the program's
        // lifetime. Arm/disarm + waitForStop/resume go through here.
        cajeta::dbg::DebugController& controller();

        // True once the program thread has finished.
        bool isFinished() const;

        // Join the program thread and return its exit code (an int32 entry's
        // return value, or 0 for a void entry). Idempotent.
        int join();

        // Debugger CP6f-2b: a snapshot of one live fiber from the runtime's
        // fiber registry (registered at spawn, removed when the carrier frees a
        // finished fiber). Read while the program is parked at a breakpoint.
        struct FiberSnapshot {
            int id;          // stable per-fiber dbg id (1, 2, 3, ...)
            void* frameTop;  // head of this fiber's debug frame chain
                             // (feed to DebugVars::walkFrames)
            int state;       // cajeta_fiber_state enum value
        };

        // Enumerate live fibers from the JIT module's registry. The registry is
        // populated by the JIT'd program (the embedded-bitcode runtime copy),
        // so this resolves the __cajeta_dbg_fiber_* accessors via jit->lookup
        // rather than the host's native runtime copy (whose registry is empty).
        // Returns empty if the symbols aren't found. Safe to call while parked.
        std::vector<FiberSnapshot> liveFibers();

    private:
        std::unique_ptr<Impl> impl_;
    };

    // Start a debug session. Compiles `opts.sourceRoot` (debug-info forced on),
    // arms `breakpoints` (and break-on-throw when `armExceptions`), installs the
    // handlers, and launches the entry on a background thread. Arming happens
    // BEFORE the program thread starts, so a program that throws/hits a bp
    // immediately can't race past the arm. Returns null on a compile/JIT failure
    // (with a message in *error when non-null).
    // `stopOnEntry` parks at the first safepoint (the entry method's first
    // executable statement) — armed before the thread starts, like exceptions.
    // `beforeRun` runs after the program is built and armed but before the
    // program thread starts. That window is where process-global state the
    // DEBUGGEE should see — but the COMPILER should not — belongs: the DAP
    // server's launch environment overlay is applied here, so a configuration
    // that suppresses inherited variables cannot strip the environment the
    // in-process build itself depends on (PATH, HOME, TMPDIR, ROCM_PATH).
    std::unique_ptr<JitDebugSession> startDebugSession(
        const JitRunOptions& opts,
        const std::vector<Breakpoint>& breakpoints,
        std::string* error = nullptr,
        bool armExceptions = false,
        bool stopOnEntry = false,
        const std::function<void()>& beforeRun = {});

} // namespace cajeta::jit
