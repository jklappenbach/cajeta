// In-process JIT host for the `cajeta` binary: compiles a project to in-memory
// IR, merges the embedded runtime + stdlib, builds an LLJIT and runs a static
// no-arg entry in this process. `cajeta dap` drives the same host.

#pragma once

#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cajeta/dbg/DebugController.h"

namespace llvm { class DataLayout; }

#include "cajeta/dbg/DebugTypeTable.h"

namespace cajeta::jit {

    struct JitRunOptions {
        std::string sourceRoot;
        // Static, parameter-less entry, dotted `package.Class.method`.
        std::string entryMethod;
        std::vector<std::string> programArgs;   // reserved; not yet forwarded
        // Reserved for CP2+: emit __cajeta_dbg_safepoint polls + debug frames.
        bool debugInfo = false;
        // XPU backends to bundle; empty = host-only, so a @Kernel launch no-ops.
        std::vector<cajeta::xpu::Backend> xpuBackends;
        std::string xpuArch;   // device arch override for those backends
        // Reuse this process's primed stdlib front-end instead of isolating.
        bool resident = false;
        // Dependency `.cja` archives ingested first: the JIT's --classpath.
        std::vector<std::string> classpath;
        // DI profile for @Profile providers; empty = the AOT default, "prod".
        std::string profile;
        // Whole-program JIT cache root: a slot keyed on compiler version, flags,
        // entry and source digests loads without constructing a Compiler.
        std::string cacheDir;
        // `cajeta run`: compile this ONE file instead of walking sourceRoot and
        // look the entry up by the script-entry suffix. Empty = normal mode.
        std::string scriptFile;
        // Build progress on the calling thread: phase boundaries carry 0/0,
        // per-source "parse" events 1-based counts.
        std::function<void(const std::string& phase, const std::string& detail,
                           int current, int total)> onProgress;
    };

    // Wall-clock breakdown of one buildJit run. The named phases are disjoint
    // segments of one interval, so their sum never exceeds totalSeconds.
    struct JitBuildPhases {
        double collectSeconds = 0;        // fs walk + compiler setup + prescan
        double parseSeconds = 0;          // per-source parse (incl. lazy stdlib)
        double codegenStdlibSeconds = 0;  // method codegen, stdlib module
        double codegenUserSeconds = 0;    // method codegen, user modules
        double finalizeSeconds = 0;       // REFL-2 + drop backfill/pin
        double mergeSeconds = 0;          // linkModules donor merge
        double jitSeconds = 0;            // verify + LLJIT build/initialize
        double totalSeconds = 0;          // whole buildJit wall time

        // Each a SUBSET of a segment above, measured inside it.
        double parseStdlibSeconds = 0;    // ensureStdlibModule + lazy pkg parses
        double jitSerializeSeconds = 0;   // WriteBitcodeToFile (cold only)
        double jitReparseSeconds = 0;     // parseBitcodeFile round-trip/load
        double jitMaterializeSeconds = 0; // LLJIT initialize (incl. codegen)
    };

    // Optional diagnostics filled by runJit when a non-null result is passed.
    struct JitRunResult {
        // Safepoint call sites emitted inside the entry: a static IR count.
        int entrySafepointsEmitted = 0;
        // Safepoints EXECUTED by the entry; global ctors are not counted.
        long safepointsExecuted = 0;
        JitBuildPhases phases;
        // Served from the whole-program cache slot; no Compiler was built.
        bool cacheHit = false;
        bool objectCacheHit = false;
        // Objects served from the pool vs compiled here; both 0 without cacheDir.
        int moduleObjectsServed = 0;
        int moduleObjectsCompiled = 0;
        // Kernel manifests embedded into the lowered module; empty for a
        // host-only run, and on a cache hit the runtime serves the cached copy.
        std::vector<cajeta::xpu::KernelManifest> kernelManifests;
    };

    // Compile + JIT + run, returning a process exit code (non-zero on a
    // compile/JIT/lookup failure); fills `result` when non-null.
    int runJit(const JitRunOptions& opts, JitRunResult* result = nullptr);

    // Convert a dotted `package.Class.method` entry into the mangled prefix
    // `package.Class::method`; "" when there is no trailing `.method`.
    std::string entryTargetFromDotted(const std::string& dotted);

    // CLI entry: `cajeta jit-run <sourceRoot> <entryMethod> [args...]`.
    int dispatchJitRun(int argc, const char* argv[]);

    // CLI entry: `cajeta run [flags] <file>.cajeta [args...]`, one script unit
    // as a session; an ancestor `cajeta.json` supplies the classpath.
    int dispatchRun(int argc, const char* argv[]);

    // --- Debug sessions (CP3) ---------------------------------------------
    // A source-line breakpoint, matched to safepoints by file BASENAME + line.
    struct Breakpoint {
        std::string file;   // e.g. "Calc.cajeta"
        int line = 0;
    };

    // The safepoint locations startDebugSession would arm for `bp`; empty means
    // it cannot bind. Reads the global loc table, populated by a compile.
    std::vector<int32_t> matchingLocIds(const Breakpoint& bp);

    // A running debug session: the entry runs on a BACKGROUND thread, so the
    // caller drives controller().waitForStop()/resume() from another thread and
    // join()s for the exit code. Keeps the Compiler + LLJIT alive meanwhile.
    class JitDebugSession {
    public:
        struct Impl;
        explicit JitDebugSession(std::unique_ptr<Impl> impl);
        ~JitDebugSession();
        JitDebugSession(const JitDebugSession&) = delete;
        JitDebugSession& operator=(const JitDebugSession&) = delete;

        // The stop/resume controller; its address is stable for the run.
        cajeta::dbg::DebugController& controller();

        // The layout the running program's memory uses, for decoding values.
        const llvm::DataLayout& dataLayout() const;

        // The debug type table's symbols, resolved to this run's addresses.
        const cajeta::dbg::ResolvedTypeSymbols& resolvedTypeSymbols() const;

        bool isFinished() const;

        // Join the program thread for its exit code, 0 for a void entry.
        int join();

        // One live fiber, read while the program is parked at a breakpoint.
        struct FiberSnapshot {
            int id;          // stable per-fiber dbg id (1, 2, 3, ...)
            void* frameTop;  // head of the fiber's debug frame chain
            int state;       // cajeta_fiber_state enum value
        };

        // Live fibers from the JIT module's registry: only the JIT'd program
        // populates it, so the accessors resolve through jit->lookup.
        std::vector<FiberSnapshot> liveFibers();

    private:
        std::unique_ptr<Impl> impl_;
    };

    // Compile with debug info, arm `breakpoints` (plus break-on-throw and
    // `stopOnEntry`) and run the entry on a background thread; null on failure,
    // with *error set. Arming precedes the thread; `beforeRun` is its last gap.
    std::unique_ptr<JitDebugSession> startDebugSession(
        const JitRunOptions& opts,
        const std::vector<Breakpoint>& breakpoints,
        std::string* error = nullptr,
        bool armExceptions = false,
        bool stopOnEntry = false,
        const std::function<void()>& beforeRun = {});

} // namespace cajeta::jit
