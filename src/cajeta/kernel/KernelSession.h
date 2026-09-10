// The accumulating-dylib kernel session: one session == one LLJIT == one
// notebook kernel, one JITDylib per cell, never merged, link order always set
// EXPLICITLY. Single-threaded by contract, except requestInterrupt().
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declared so this header does not pull the build tool in.
namespace cajeta::buildtool {
    class Repository;
    struct RepositoryDelegation;
}

namespace cajeta::kernel {

    // One compiler diagnostic for a cell, parsed from the compiler's NDJSON so a
    // frontend dispatches on fields — and so warnings reach the notebook at all.
    struct CellDiagnostic {
        std::string severity;   // "error" | "warning" | "note"
        std::string code;       // CAJETA_ERROR_*; empty when the record had none
        std::string message;
        // The CELL's name (In[N]) and the USER's line; nothing here re-translates.
        std::string file;
        int line = 0;
        int column = 0;
    };

    // One frame of a cell's traceback; `text` renders as `In[3], line 2`.
    struct CellFrame {
        std::string type;     // declaring type, canonical
        std::string method;
        std::string file;     // `In[N]` for a cell's own frame
        int line = 0;
        std::string text;
    };

    // The outcome of one `execute`. A compile failure is DATA, not an exception.
    struct CellResult {
        bool ok = false;
        // Compiler error id (`CAJETA_ERROR_*`, or "syntax") when !ok.
        std::string errorId;
        std::string message;
        // Host source name for the diagnostic — the cell id given to the compiler.
        std::string file;
        int line = 0;
        // The cell entry's `return <int32>`, 0 when it had none; `return 5;` sets
        // this and displays nothing, which the unit RESULT below does not.
        int32_t value = 0;
        // The unit result, `Out[N]`: the cell's trailing expression rendered as
        // text. Presence is its own flag because a result can render as empty.
        bool hasResult = false;
        std::string result;
        // 1-based, advancing on every execute INCLUDING a failed one.
        int executionCount = 0;
        // Everything the compiler said, structured; warnings appear ONLY here.
        std::vector<CellDiagnostic> diagnostics;
        // The cell RAN and threw, as against failing to compile. Session survives.
        bool threw = false;
        std::string exceptionType;   // canonical class of the thrown value
        std::vector<CellFrame> traceback;   // innermost first
    };

    // The invariants of the accumulating world, made checkable.
    struct SessionStats {
        int cellsCompiled = 0;
        int cellDylibsCreated = 0;
        // MUST stay 0: cells are never merged into one another.
        int crossCellModuleMerges = 0;
        // Instantiations demoted to weak_odr so several cells can share one.
        int weakDemotedInstantiations = 0;
        // Vtable slots repointed by a BODY-ONLY class redefinition.
        int vtableSlotsRepointed = 0;
        // `__cajeta_task_shutdown` calls — one per session; per cell would tear
        // the shared carrier pool out from under later cells.
        int taskShutdownCalls = 0;
        // `__cajeta_session_drop_all` calls — one, and BEFORE the task shutdown.
        int sessionDropAllCalls = 0;
        // Bindings when shutdown began, and after the drop pass — which must be 0.
        int sessionBindingsAtShutdown = -1;
        int liveSessionBindings = -1;
        // generateCode calls from the eager codegen loop, over the session's cells.
        long long eagerBodiesGenerated = 0;
        // Bodies the DefinitionGenerator delivered on demand.
        long long lazyBodiesDelivered = 0;
    };

    // How a session is built. Everything here is optional; the no-argument
    // `create` is a stdlib-only session.
    struct SessionOptions {
        // A directory whose nearest ancestor `cajeta.json` governs the classpath;
        // empty means no project resolution. Deliberately NOT defaulted here.
        std::string projectDir;
        // Archive paths added directly, after anything `projectDir` resolved.
        std::vector<std::string> classpath;
        // Called at each build phase boundary so a host can narrate the wait.
        std::function<void(const std::string& phase)> progress;
    };

    // The project a kernel launched in `cwd` belongs to: the nearest ANCESTOR
    // (inclusive) carrying a cajeta.json, or `cwd` when there is none. Jupyter
    // starts a kernel in the NOTEBOOK's directory, often one level down.
    std::string projectDirForLaunch(const std::string& cwd);

    class KernelSession {
    public:
        // Build a session: LLJIT + bootstrap dylib, runtime and resident stdlib
        // initialized once. Null on failure. Call on the thread that will own it.
        static std::unique_ptr<KernelSession> create(std::string* error = nullptr);

        // As above, with a classpath. Resolution happens ONCE, here: dependency
        // definitions must be in the module list before the first cell is run.
        static std::unique_ptr<KernelSession> create(const SessionOptions& options,
                                                     std::string* error = nullptr);

        ~KernelSession();
        KernelSession(const KernelSession&) = delete;
        KernelSession& operator=(const KernelSession&) = delete;

        // Compile `source` as a script unit into this session and run its entry.
        // `cellName` is the source name diagnostics carry ("In[3]", which compiles
        // to `cajeta.script.cell_3`). A failed compile leaves the session alone.
        CellResult execute(const std::string& source);
        CellResult execute(const std::string& source, const std::string& cellName);

        // Cell output: installed once, used for every later cell. Chunks arrive on
        // a PUMP thread while the cell still runs, in write order. An empty handler
        // turns capture off, which is the default.
        using StreamHandler = std::function<void(const std::string&)>;
        void setStreamHandler(StreamHandler handler);

        // Stop the running cell at its next safepoint; it ends as a
        // KeyboardInterrupt with the session intact. THE ONE METHOD HERE SAFE TO
        // CALL FROM ANOTHER THREAD. A cell in a native call reaches no safepoint.
        void requestInterrupt();

        // Resolve across the session's dylibs, newest cell first, so a redefined
        // name yields the newest definition. `lookup` takes a short cajeta name.
        void* lookupSymbol(const std::string& exactName);

        template <typename T>
        T lookup(const std::string& shortName) {
            return reinterpret_cast<T>(lookupShort(shortName));
        }

        // End the session: `__cajeta_task_shutdown` exactly once, joining carriers
        // while the code they re-enter is live, then the dylibs. Idempotent.
        void shutdown();

        // Splice a local .cja into the LIVE session: collision-checked ingest plus
        // link, importable by later cells. False + *error on rejection.
        bool installArchive(const std::string& cjaPath,
                            std::string* error = nullptr);

        // The host end of `Packages.install`, called from JIT'd cell code. Writes
        // the resolved version into `out` on true, the failure message on false.
        bool installFromHook(const std::string& request,
                             const std::string& constraint,
                             bool save,
                             char* out, int32_t outCap);

        // True when the archive declares a canonical name the session already
        // holds. Safe to call MID-CELL: archive I/O and lookups, no compiler pass.
        bool collidesWithSession(const std::string& archivePath,
                                 std::string* error);

        // Graduate an installed dependency into the project's cajeta.json through
        // the format-preserving editor `cajeta add` uses. False + `errorOut` when
        // there is no project, or the rewrite would not parse.
        bool saveToManifest(
            const std::string& name, const std::string& constraint,
            const std::function<void(const std::string&)>& phase,
            std::string* errorOut);

        // True when `signature` is empty (policy is the caller's) or verifies as
        // from the organization owning the name. A key document served for
        // `owningOrganization` DECIDES, the trust store being only its fallback.
        bool verifySignatureOrFail(
            const std::string& archivePath, const std::string& name,
            const std::string& version,
            const cajeta::buildtool::Repository& repo,
            const std::string& owningOrganization,
            const std::string& signature,
            // The repository's verified delegation, or nullptr.
            const cajeta::buildtool::RepositoryDelegation* delegation,
            const std::function<void(const std::string&)>& phase,
            std::string* errorOut);

        // Resolve a library NAME + constraint to a verified local archive through
        // the buildtool's repositories, cache and checksums; `phase` narrates to
        // the cell's stream. False + `errorOut` leaves no half-installed state.
        bool resolveForInstall(
            const std::string& name, const std::string& constraint,
            const std::function<void(const std::string&)>& phase,
            std::string* pathOut, std::string* versionOut,
            std::string* errorOut);

        const SessionStats& stats() const;

    private:
        KernelSession();
        void* lookupShort(const std::string& shortName);
        // Turn a thrown Throwable into the cell's structured error: type, message,
        // and a traceback whose frames name cells.
        void describeThrow(void* thrown, const std::string& cellName,
                           CellResult* result);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
