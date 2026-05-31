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

#include <string>
#include <vector>

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

} // namespace cajeta::jit
