//
// JIT helper for Cajeta integration tests. Compiles a Cajeta source string,
// links the embedded runtime, and JITs the resulting module so tests can call
// generated functions directly and assert against the returned value.
//

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

#include "cajeta/compile/CompilerMode.h"

namespace cajeta {
    class Compiler;
    class CajetaModule;
}

namespace cajeta_test {

class CajetaJit {
public:
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

private:
    CajetaJit();

    void* lookupAddress(const std::string& shortName);

    std::unique_ptr<llvm::orc::LLJIT> jit;
    // Mapping short method name -> full mangled name in the JIT'd module. Built
    // once at compile time so per-test lookups don't have to rescan.
    std::map<std::string, std::string> nameMap;
};

} // namespace cajeta_test
