//
// JIT helper for Cajeta integration tests. Compiles a Cajeta source string,
// links the embedded runtime, and JITs the resulting module so tests can call
// generated functions directly and assert against the returned value.
//

#pragma once

#include <map>
#include "cajeta/error/DiagnosticEngine.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

#include "cajeta/compile/CompilerMode.h"
#include "cajeta/xpu/XpuTarget.h"

namespace cajeta {
    class Compiler;
    class CajetaModule;
    class SessionState;
}

namespace cajeta_test {

class CajetaJit {
public:
    // title-tracking 5.1.11 / 5.2.8 — warning capture. The compiler reports
    // recoverable diagnostics to the active DiagnosticEngine; the CLI installs
    // one, but the JIT harness never did, so every WARNING was silently
    // dropped and no test could assert on one. compile() now installs an
    // engine and parks the diagnostics it collected here (cleared at the start
    // of each compile), so a test can assert an advisory fired without the
    // build going red.
    static const std::vector<cajeta::CollectedDiagnostic>& lastDiagnostics();
    // Convenience: was a diagnostic with this code reported by the last
    // compile? (Any severity — the code is the identity.)
    static bool sawDiagnostic(const std::string& code);

    struct Options {
        bool boundsCheckEnabled = true;
        // Signed-overflow trap (--overflow-checks=on). On by default
        // — Debug mode's strict-correctness stance. Stdlib hash code
        // that needs wrapping uses uint* types (FNV-1a in String.hash
        // accumulates in uint64, then reinterprets to int64). Tests
        // exercising wrapping with explicit signed types disable it.
        bool overflowChecksEnabled = true;
        // Bounds-check mode (--bounds=on|off|trap). Overrides the
        // boolean above when set; left empty defaults to on/off based
        // on boundsCheckEnabled.
        std::optional<cajeta::BoundsCheck> boundsCheckMode;
        // Live-set tracking (--live-set=strict|bounded|off). Defaults
        // to Strict (the Debug default); tests verifying the off path
        // skip the runtime registration emit.
        std::optional<cajeta::LiveSet> liveSetMode;
        // Poison-on-free (--poison-free=on). When true, the JIT
        // initializer calls __cajeta_set_poison_free(1) so freed
        // chunks get memset with 0xDB before free(). Default off for
        // tests so the existing suite stays deterministic; tests that
        // exercise UAF surfaces explicitly opt in. Compiler default
        // for binary builds (CompilerFlags::poisonFree) is true and
        // wires through a global ctor in --emit=exe mode.
        bool poisonFreeEnabled = false;
        // Drop-chain validation (--drop-chain-validate=on). When true,
        // every drop-chain push / pop / mark_inactive checks invariants
        // and aborts with a diagnostic on corruption (LIFO discipline,
        // active-bit sanity). Default off; tests that deliberately
        // induce corruption opt in.
        bool dropChainValidateEnabled = false;
        // Stack-trace capture on throw (--stack-trace-capture=on).
        // Defaults true to preserve the runtime's default (every
        // throw records its native stack via backtrace(3)); tests
        // verifying the off path explicitly opt out.
        bool stackTraceCaptureEnabled = true;
        // Line-info shadow-stack emission (--line-info). Default true (the
        // compiler default), so getStackTrace() yields semantic frames. Tests
        // exercising the address-only fallback set this false.
        bool lineInfoEnabled = true;
        // --debug-info=full: statement safepoints, local records, and the
        // embedded location table (external-debug §2/§3). Off by default — it
        // changes codegen and only matters under a debugger.
        bool debugInfoEnabled = false;
        // --profiler=instrument (cajeta-profiler §3). Off by default, which
        // is what makes 10.1.b assertable: the whole existing suite runs with
        // the flag absent, so any IR change it caused would surface as a
        // failure somewhere else first.
        cajeta::Profiler profiler = cajeta::Profiler::Off;
        // --profiler-select CONTENTS (§3.8-§3.10). The test supplies the text
        // directly rather than a file, exactly as the flags carry it — the
        // path is never part of what the compiler decides on.
        std::string profilerSelect;
        // XPU device backend(s) to register @Kernels for and bundle in the
        // runtime manifest. Empty defaults to {Nvptx} (the legacy NVIDIA
        // host-launch path). The CPU dispatcher tests set {Cpu} to exercise
        // the GPU-free fall-to-CPU launch through __cajeta_xpu_launch.
        std::vector<cajeta::xpu::Backend> xpuBackends;
        // Override the per-backend default device arch. May be a comma-separated
        // list ("gfx1100,gfx1151") to build a multi-arch bundle. Empty = default.
        std::string xpuArch;
        // script-units U4 — session compile. When set, script units compile
        // INTO this host-owned session table (ownership facts seed/write
        // back across compiles) and located diagnostics carry
        // sessionHostName as the source name. Null for ordinary compiles.
        cajeta::SessionState* session = nullptr;
        std::string sessionHostName;
        // Capture the post-codegen, post-AlwaysInline (O0) host LLVM IR into the
        // returned CajetaJit, readable via getModuleIr(). Captured after the O0
        // optimizeModule pass so a test can see whether a @ValueType operator
        // folded to its caller (S4) as well as the flat <N x T> Vector shape
        // (S0). Off by default so the normal test path pays nothing.
        bool captureIr = false;
    };

    // Compile `source` (a Cajeta compilation unit) into a JIT instance. The class
    // must match the file path mapping the compiler expects (we infer the package
    // from `fqClassName`). The two overloads exist because default-aggregate-init
    // for a separately-declared struct trips older C++17 dialects.
    static std::unique_ptr<CajetaJit> compile(const std::string& source,
                                              const std::string& fqClassName);
    static std::unique_ptr<CajetaJit> compile(const std::string& source,
                                              const std::string& fqClassName,
                                              const Options& opts);

    // Multi-source overload: each (fqClassName → source) pair lands
    // at the matching path under a shared temp source-root, so the
    // file's path-derived package matches its `package X;`
    // declaration. The compiler then parses each file as its own
    // module, runs the same A3/A8/Phase 1/Phase 2 passes the
    // single-source path runs, and merges the resulting IR modules
    // into one before handing to LLJIT. Cross-module DI, advice
    // pointcuts, and method calls span the merged module.
    static std::unique_ptr<CajetaJit> compile(
        const std::map<std::string, std::string>& sources,
        const std::string& fqEntryClass);
    static std::unique_ptr<CajetaJit> compile(
        const std::map<std::string, std::string>& sources,
        const std::string& fqEntryClass,
        const Options& opts);

    ~CajetaJit();

    // Look up a method by short name (e.g. "add"). Returns a callable pointer cast
    // to T, or nullptr if not found.
    template <typename T>
    T lookup(const std::string& shortName) {
        return reinterpret_cast<T>(lookupAddress(shortName));
    }

    // Resolve an exact (unmangled IR) symbol name to its JIT address —
    // e.g. a data global like "cajeta.lang.String#VTable". LLJIT applies
    // the platform mangling internally, so the IR name works on every
    // target. Returns nullptr if unresolved. Used by tests that must
    // distinguish a real cajeta.lang.String return from a legacy
    // malloc'd char* without sniffing pointer bytes.
    void* lookupRawSymbol(const std::string& exactName);

    // Post-codegen host LLVM IR captured when Options::captureIr is set
    // (empty otherwise). The regression oracle for the Vector retrofit
    // (value-type-overloading-plan S0): tests assert the flat `<N x T>` /
    // intrinsic shape is unchanged across the operator-mechanism work.
    const std::string& getModuleIr() const { return moduleIr; }

private:
    CajetaJit();

    void* lookupAddress(const std::string& shortName);

    std::unique_ptr<llvm::orc::LLJIT> jit;
    // Shared-dylib mode (U7b/c, CAJETA_JIT_SHARED_DYLIB): `jit` is null and the
    // LLJIT is BORROWED from the process-wide StdlibReuseCache. This test's code
    // lives in `userDylib` (links the shared CajetaStdlib JITDylib); it's removed
    // on destruction so dylibs don't accumulate. Lookups target `userDylib`.
    llvm::orc::LLJIT* sharedJit = nullptr;
    llvm::orc::JITDylib* userDylib = nullptr;
    std::string moduleIr;
    // Mapping short method name -> full mangled name in the JIT'd module. Built
    // once at compile time so per-test lookups don't have to rescan.
    std::map<std::string, std::string> nameMap;
    // Temp source/archive roots the multi-source compile path created for this
    // test, removed on destruction. The lifetime is the JIT's rather than the
    // compile call's so nothing that reads back through a recorded path (debug
    // info, the archive root) can find the tree gone while the module is live.
    // Set CAJETA_KEEP_TEMP to leave them for inspection.
    std::vector<std::string> tempRoots;
};

// Remove any temp source/archive roots still outstanding. The test binary
// terminates with _Exit to dodge a two-LLVM teardown crash (see test/main.cpp),
// which skips atexit and every static destructor — so a CajetaJit held in a
// function-local static never runs its own cleanup, and this has to be called
// explicitly at each exit point, exactly as the gcov dump already is.
void sweepTempRoots();

} // namespace cajeta_test
