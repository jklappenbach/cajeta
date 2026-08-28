//
// jupyter-kernel U1 (spec 2.1, 2.4) — the accumulating-dylib kernel session.
//
// One session == one LLJIT == one notebook kernel. Structure:
//
//   BootstrapJD   runtime + resident stdlib, materialized and initialize()d
//                 ONCE for the session's lifetime (its global ctors run once,
//                 not once per cell).
//   Cell_0 JD     one JITDylib per successfully compiled cell, each carrying
//   Cell_1 JD     only that cell's module. Link order is set EXPLICITLY to
//   ...           {self, prior cells (newest first), BootstrapJD, process},
//                 so cell N sees every earlier definition, later definitions
//                 shadow earlier ones (script-units 5.2 last-write-wins), and
//                 runtime symbols always resolve to the JIT copy.
//
// Cells are never merged. There is no `Linker::linkModules` across cells —
// resolution is by name at materialization, the model
// SharedStdlibDylibSpikeTests proved and the per-module delivery path in
// CajetaJitHost already relies on.
//
// LINK ORDER IS LOAD-BEARING, NOT COSMETIC. `createJITDylib` seeds a new
// dylib's link order with the process-symbol main dylib FIRST. Leaving that
// in place (i.e. calling only `addToLinkOrder(bootstrap)`) makes user code
// resolve `__cajeta_exc_push` and friends to the process's NATIVE runtime
// while stdlib code inside the JIT uses the JIT's copy — two distinct
// `__cajeta_main_exc_top` TLS slots, so a throw crossing that seam is never
// caught. JitTestHelper hit exactly this (its per-test dylib path documents
// the same fix). Always setLinkOrder explicitly.
//
// THREADING: single-threaded by contract, like the runtime session registry
// and StdlibReuseCore (whose baselines are thread_local — an off-owner thread
// silently gets no baseline and a cold path). The session's construction,
// every cell compile, and every cell run happen on ONE thread: the kernel's
// execution thread.
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cajeta::kernel {

    // One compiler diagnostic for a cell, in the compiler's own shape
    // (compiler-jsonl 3.1.1). These arrive parsed from the compiler's NDJSON
    // stream rather than scraped from its text, so a frontend dispatches on
    // fields instead of sniffing prose — and a WARNING reaches the notebook
    // too, which the pass/fail of `execute` alone can never carry.
    struct CellDiagnostic {
        std::string severity;   // "error" | "warning" | "note"
        std::string code;       // CAJETA_ERROR_*; empty when the record had none
        std::string message;
        // The CELL's name (In[N]) and the USER's line: script-units U5 maps
        // wrapper coordinates back before the diagnostic is ever emitted, so
        // nothing here re-translates.
        std::string file;
        int line = 0;
        int column = 0;
    };

    // One frame of a cell's traceback (spec 4.4). `text` is the rendered
    // form: a frame in the cell's own entry renders as `In[3], line 2` —
    // never as the synthesized class the cell compiles into, which is an
    // implementation detail the notebook user never asked about.
    struct CellFrame {
        std::string type;     // declaring type, canonical
        std::string method;
        std::string file;     // `In[N]` for a cell's own frame
        int line = 0;
        std::string text;
    };

    // The outcome of one `execute`. A compile failure is DATA, not an
    // exception: the kernel turns it into an `error` reply and carries on,
    // and the session is unchanged (script-units 5.5).
    struct CellResult {
        bool ok = false;
        // Compiler error id (`CAJETA_ERROR_*`, or "syntax") when !ok.
        std::string errorId;
        std::string message;
        // Host source name for the diagnostic — the cell id this session
        // handed the compiler, so line numbers are the USER's (script-units
        // U5 does the mapping; nothing extra needed here).
        std::string file;
        int line = 0;
        // The cell entry's return value (script-units: `return <int32>`),
        // 0 when the cell had no explicit return. Distinct from the unit
        // RESULT below: `return 5;` sets this and displays nothing.
        int32_t value = 0;
        // The unit result — `Out[N]` (spec 4.2). Set when the cell's last
        // statement was an expression that produced a value; `result` is that
        // value rendered as text. A cell ending in a declaration, a loop, a
        // `return`, or a void call has none, which is why presence is its own
        // flag: a result CAN legitimately render as the empty string.
        bool hasResult = false;
        std::string result;
        // 1-based, advancing on every execute INCLUDING a failed one (spec
        // 2.2) so `Out[N]` never reuses a number.
        int executionCount = 0;
        // Everything the compiler said about this cell, structured (spec 4.4).
        // A failing cell's error appears here as well as in the errorId /
        // message fields above; warnings appear ONLY here, and a cell can
        // succeed with a non-empty list.
        std::vector<CellDiagnostic> diagnostics;
        // The cell RAN and threw (spec 4.4) — as opposed to failing to
        // compile, which is what an empty `exceptionType` with `!ok` means.
        // The session survives either way.
        bool threw = false;
        std::string exceptionType;   // canonical class of the thrown value
        std::vector<CellFrame> traceback;   // innermost first
    };

    // Observability for the tests and, later, the kernel's own diagnostics.
    // These are the invariants of the accumulating world, made checkable.
    struct SessionStats {
        int cellsCompiled = 0;
        int cellDylibsCreated = 0;
        // MUST stay 0: cells are never merged into one another.
        int crossCellModuleMerges = 0;
        // Template-instantiation symbols demoted to weak_odr so a
        // specialization used by several cells has one winning definition
        // instead of a hard ORC duplicate-definition failure.
        int weakDemotedInstantiations = 0;
        // Vtable slots repointed by a BODY-ONLY class redefinition
        // (script-units 5.4). Observable because "the edit took effect" and
        // "the edit was a no-op" are otherwise indistinguishable from the
        // outside until a value happens to be called.
        int vtableSlotsRepointed = 0;
        // `__cajeta_task_shutdown` invocations — exactly one per session,
        // at the end. Calling it per cell would tear the shared carrier pool
        // out from under later cells.
        int taskShutdownCalls = 0;
        // `__cajeta_session_drop_all` invocations — also exactly one, and
        // BEFORE the task shutdown: a drop that runs after the carrier pool
        // is gone runs user code on a torn-down runtime.
        int sessionDropAllCalls = 0;
        // Session bindings registered when shutdown began, and the count
        // after the drop pass — which must be 0. The before-count is the one
        // that answers "was this name ever bound at all?", the question a
        // silently-empty cross-cell read turns on.
        int sessionBindingsAtShutdown = -1;
        int liveSessionBindings = -1;
        // generateCode invocations made by the eager codegen loop, summed
        // over the session's cells. ~23k per cell on the eager path; under
        // lazy codegen only the cell's own module goes through the loop, so
        // this collapsing is THE observable for lazy-codegen 4.2.1.
        long long eagerBodiesGenerated = 0;
        // Bodies the DefinitionGenerator delivered on demand. With
        // init-extract delivery (4.2.4) a trivial cell's total stays a
        // fraction of the world's ~3,205 — before it, delivered
        // vtable/RTTI/#ClassObject definitions pulled 2,906 of them.
        long long lazyBodiesDelivered = 0;
    };

    // How a session is built (spec 6). Everything here is optional; the
    // no-argument `create` is a stdlib-only session, which is what every
    // test that does not care about dependencies wants.
    struct SessionOptions {
        // A directory whose nearest ancestor `cajeta.json` governs the
        // classpath — the project the notebook belongs to. Its resolved
        // manifest dependencies are exactly the set `cajeta build` would
        // pass. Empty means no project resolution at all.
        //
        // `cajeta kernel` defaults this to the process's working directory,
        // because Jupyter launches a kernel in the notebook's own directory
        // and that is the project the user means. It is NOT defaulted here:
        // a test that happens to run inside a project would otherwise start
        // resolving that project's dependencies without asking.
        std::string projectDir;
        // Archive paths added directly, after anything `projectDir`
        // resolved. For a caller that knows exactly which `.cja` it wants.
        std::vector<std::string> classpath;
        // Called at each build phase boundary (7.2.8) so a host can narrate
        // the wait — the whole session build runs inside the first
        // execute_request, which otherwise shows a silent running cell.
        std::function<void(const std::string& phase)> progress;
    };

    class KernelSession {
    public:
        // Build a session: LLJIT + bootstrap dylib with the runtime and the
        // resident stdlib initialized once. Returns null on failure, with the
        // reason in `error` when non-null. Must be called on the thread that
        // will own the session.
        static std::unique_ptr<KernelSession> create(std::string* error = nullptr);

        // As above, with a classpath (spec 6.1): cells can then import and
        // drive project dependencies exactly as a compiled program would.
        // Resolution happens ONCE, here — dependency definitions have to be
        // in the module list before the first cell is delivered to the JIT.
        static std::unique_ptr<KernelSession> create(const SessionOptions& options,
                                                     std::string* error = nullptr);

        ~KernelSession();
        KernelSession(const KernelSession&) = delete;
        KernelSession& operator=(const KernelSession&) = delete;

        // Compile `source` as a script unit into this session and run its
        // entry. `cellName` is the host source name diagnostics will carry
        // (e.g. "In[3]"); the no-name overload derives "In[N]" from the
        // execution count. The unit's implicit class is named after it —
        // "In[3]" compiles to `cajeta.script.cell_3` — so that is the name
        // that appears in mangled symbols and JIT errors.
        // A failed compile leaves the session untouched.
        CellResult execute(const std::string& source);
        CellResult execute(const std::string& source, const std::string& cellName);

        // Cell output (spec 4.1). Installed once and used for every later
        // cell; chunks arrive on a PUMP thread while the cell is still
        // running, in write order, so a loop printing progress shows each
        // line as it is written rather than a burst at the end. Passing an
        // empty handler turns capture off, which is the default: without one,
        // a cell's output goes wherever the process's stdout already goes.
        using StreamHandler = std::function<void(const std::string&)>;
        void setStreamHandler(StreamHandler handler);

        // Stop the running cell at its next safepoint (spec 5.1). The cell
        // ends as a `KeyboardInterrupt` cell error with the session and every
        // binding intact; the next cell runs normally.
        //
        // THE ONE METHOD ON THIS CLASS SAFE TO CALL FROM ANOTHER THREAD, and
        // it has to be: the kernel answers `interrupt_request` on its control
        // channel while the execution thread is inside the cell being
        // interrupted. It sets an atomic through a pointer resolved at
        // construction and touches nothing else.
        //
        // Best-effort at safepoint granularity (spec 5.1): a cell blocked in
        // a native call reaches no safepoint and does not stop until it
        // returns to one. Requesting an interrupt with nothing running is a
        // no-op — the request is cleared at the start of every cell, so it
        // can never land on a cell the user did not aim at.
        void requestInterrupt();

        // Resolve a symbol across the session's dylibs, newest cell first, so
        // a redefined name yields the newest definition. `lookup` takes a
        // short cajeta name (e.g. "bar"); `lookupSymbol` takes an exact IR
        // symbol (e.g. "__cajeta_task_shutdown").
        void* lookupSymbol(const std::string& exactName);

        template <typename T>
        T lookup(const std::string& shortName) {
            return reinterpret_cast<T>(lookupShort(shortName));
        }

        // End the session: run `__cajeta_task_shutdown` exactly once (joining
        // carriers while the code they may re-enter is still live — the same
        // ordering constraint CajetaJit's destructor documents), then release
        // the dylibs. Idempotent; the destructor calls it.
        void shutdown();

        // notebook-olla-install U1 (spec 4.1-4.4): splice a local .cja into
        // the LIVE session — collision-checked ingest + link against the
        // accumulating world. Its packages become importable by cells
        // compiled afterwards. Safe to call from the session thread while a
        // cell executes (the host-hook shape). False + *error on rejection.
        bool installArchive(const std::string& cjaPath,
                            std::string* error = nullptr);

        // notebook-olla-install U2 (spec 2.1, 2.4-2.6): the host end of
        // `Packages.install`, called from JIT'd cell code through the
        // runtime bridge. Writes the resolved version into `out` when it
        // returns true, and the located failure message when it returns
        // false. Resolution is stubbed for U2 — `request` is a local .cja
        // path until Unit 3 wires in the resolver.
        bool installFromHook(const std::string& request,
                             const std::string& constraint,
                             char* out, int32_t outCap);

        // notebook-olla-install U3 (spec 3.1, 3.2, 3.4, 2.6): resolve a
        // library NAME + constraint to a verified local archive through the
        // buildtool's repositories, artifact cache, and published
        // checksums. `phase` narrates resolve/fetch/verify to the cell's
        // stream (6.1). False with `errorOut` set on any rejection —
        // nothing is spliced, so a failure leaves no half-installed state.
        bool resolveForInstall(
            const std::string& name, const std::string& constraint,
            const std::function<void(const std::string&)>& phase,
            std::string* pathOut, std::string* versionOut,
            std::string* errorOut);

        const SessionStats& stats() const;

    private:
        KernelSession();
        void* lookupShort(const std::string& shortName);
        // Turn a thrown Throwable into the cell's structured error (spec
        // 4.4): type, message, and a traceback whose frames name cells.
        void describeThrow(void* thrown, const std::string& cellName,
                           CellResult* result);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
