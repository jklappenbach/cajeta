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
#include <memory>
#include <string>

namespace cajeta::kernel {

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
        // 0 when the cell had no explicit return. The trailing-expression
        // unit RESULT (`Out[N]`) is U3's concern, not this.
        int32_t value = 0;
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
        // `__cajeta_task_shutdown` invocations — exactly one per session,
        // at the end. Calling it per cell would tear the shared carrier pool
        // out from under later cells.
        int taskShutdownCalls = 0;
    };

    class KernelSession {
    public:
        // Build a session: LLJIT + bootstrap dylib with the runtime and the
        // resident stdlib initialized once. Returns null on failure, with the
        // reason in `error` when non-null. Must be called on the thread that
        // will own the session.
        static std::unique_ptr<KernelSession> create(std::string* error = nullptr);

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

        const SessionStats& stats() const;

    private:
        KernelSession();
        void* lookupShort(const std::string& shortName);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
