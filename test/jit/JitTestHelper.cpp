//
// Implementation of the JIT test helper.
//

#include "JitTestHelper.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/compile/Optimizer.h"   // optimizeModule — runs AlwaysInlinerPass at O0
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/xpu/XpuTarget.h"

#include "JitWinSymbols.h"
#include "JitErrorShim.h"

#include <list>
#include <set>

#include "cajeta/type/CajetaClass.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Transforms/Utils/Cloning.h"   // CloneModule — stdlib-reuse merge
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

// Runtime hooks defined in runtime/native/cajeta_runtime.c. The runtime
// is linked into the test binary as a native object (see src/CMakeLists.txt
// — "Also compile the runtime as a native object so tests can observe
// runtime state directly from C++"), so these resolve at link time.
extern "C" {
    void __cajeta_set_poison_free(int enabled);
    void __cajeta_set_drop_chain_validate(int enabled);
    void __cajeta_set_stack_trace_capture(int enabled);

    // cajeta.net TLS engine (runtime/native/cajeta_tls.c). Unlike the other
    // __cajeta_* runtime symbols, these are NOT in the embedded JIT bitcode
    // (that would drag OpenSSL's headers + unresolvable static-libssl SSL_*
    // externs into every JIT module). They're native-only in libcajeta_lib, so
    // the JIT's process-symbol generator can't see them (exe-local, not
    // PE-exported on MinGW) — registerTlsEngineSymbols() binds them explicitly.
    void* __cajeta_tls_ctx_new(int isServer);
    int   __cajeta_tls_ctx_use_cert_key_pem(void* ctx, const void* cert, int cl,
                                            const void* key, int kl);
    void  __cajeta_tls_ctx_free(void* ctx);
    int   __cajeta_tls_ctx_set_verify(void* ctx, int mode);
    int   __cajeta_tls_ctx_add_trust_pem(void* ctx, const void* pem, int len);
    int   __cajeta_tls_ctx_use_system_trust(void* ctx);
    void* __cajeta_tls_conn_new(void* ctx, int isServer);
    int   __cajeta_tls_set_verify_host(void* conn, const void* host, int len);
    int   __cajeta_tls_verify_result(void* conn);
    int   __cajeta_tls_set_sni(void* conn, const void* host, int hostLen);
    int   __cajeta_tls_set_alpn(void* conn, const void* protos, int len);
    int   __cajeta_tls_get_alpn(void* conn, void* out, int max);
    int   __cajeta_tls_ctx_set_alpn_select(void* ctx, const void* protos, int len);
    int   __cajeta_tls_feed_ciphertext(void* conn, const void* buf, int len);
    int   __cajeta_tls_pull_ciphertext(void* conn, void* out, int max);
    int   __cajeta_tls_pending_ciphertext(void* conn);
    int   __cajeta_tls_handshake_step(void* conn);
    int   __cajeta_tls_write_plaintext(void* conn, const void* buf, int len);
    int   __cajeta_tls_read_plaintext(void* conn, void* out, int max);
    int   __cajeta_tls_shutdown(void* conn);
    void  __cajeta_tls_free(void* conn);
}

namespace cajeta_test {

using namespace cajeta;

namespace {

// Initialize LLVM JIT machinery once per process. ORC needs these — InitializeAll*
// from the Compiler ctor only set up codegen targets; the JIT also wants the
// native asm parser.
void ensureJitInitialized() {
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        initialized = true;
    }
}

// Convert a fully-qualified class name like "test.foo.Bar" into a relative file
// path "test/foo/Bar.cajeta" (the Compiler infers the package from this layout).
std::filesystem::path classNameToRelativePath(const std::string& fqClassName) {
    std::filesystem::path p;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            p /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    p += ".cajeta";
    return p;
}

// Write `source` to a fresh path under a tmp dir whose layout matches `fqClassName`.
// Returns {sourceRoot, sourcePath}.
std::pair<std::filesystem::path, std::filesystem::path>
writeSourceToTemp(const std::string& source, const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_test_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel = classNameToRelativePath(fqClassName);
    std::filesystem::path full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full);
    out << source;
    out.close();
    return {base, full};
}

// Cross-module codegen sweeps + the phase-1/2 fixpoint + per-class static
// initializers, run over the given module list. Factored so the stdlib prime
// and each per-test compile run the exact same passes. In reuse mode the caller
// MUST include the persistent stdlib module here even though it is not in the
// test Compiler's own list: user code can instantiate a stdlib template (e.g.
// `Stream<Pair<...>>`), whose new CajetaClass is owned by the stdlib module, so
// its method bodies are emitted only if the stdlib module is in this loop.
// Re-visiting already-built stdlib methods is a cheap idempotent no-op.
void runCodegenPasses(const std::list<cajeta::CajetaModulePtr>& modules) {
    cajeta::CajetaModule::validatePlaceholders();
    cajeta::CajetaModule::resolveAdviceMatches();
    cajeta::CajetaModule::setActiveProfile("test");
    cajeta::CajetaModule::resolveDependencyGraph();

    // REFL-1.7: force-build the canonical Class<?> instantiation before method
    // codegen (cajeta.reflect.Class is a template Class<T>) so its bodies are
    // emitted and every #ClassObject can embed its shared vtable. Mirrors
    // Compiler::compile. Under stdlib reuse this runs at prime (before
    // captureBaseline), so Class<?> becomes a resident baseline instantiation
    // and per-test calls are idempotent no-ops — never a novel-instantiation
    // reuse hazard.
    cajeta::CajetaClass::ensureClassWildcardInstantiated();

    size_t prevMethodCount = 0;
    while (true) {
        size_t methodCount = 0;
        for (auto& m : modules)
            methodCount += m->getAllMethods().size();
        for (auto& m : modules)
            for (auto& method : m->getAllMethods())
                method->getLlvmFunctionType();
        for (auto& m : modules) {
            for (auto& method : m->getAllMethods()) {
                try {
                    method->generateCode();
                } catch (cajeta::Exception& e) {
                    std::cerr << "[CajetaException @ codegen] "
                        << e.getErrorId() << ": " << e.getMessage()
                        << "  (method=" << method->getName() << ")\n";
                    throw;
                }
            }
        }
        size_t after = 0;
        for (auto& m : modules)
            after += m->getAllMethods().size();
        if (after == methodCount && after == prevMethodCount) break;
        prevMethodCount = after;
    }
    for (auto& m : modules)
        for (auto& [name, klass] : m->getStructures())
            if (klass) klass->generateStaticInitializers();

    // REFL-2 — fill the reflective invoke-adapter bodies + finalize/register
    // #ClassObjects now that every method's LLVM function exists (mirrors
    // Compiler::compile's AOT pass). Runs here, at the tail of the codegen
    // pass, so under stdlib reuse it executes at PRIME for stdlib classes
    // (captured byte-pristine into the baseline) and only for new user classes
    // per-test — emitReflect*Body / finalizeClassObject are all idempotent.
    // Each adapter lands in its class's own llvm module, before any merge.
    for (auto& [key, type] : cajeta::CajetaType::getCanonicalMap()) {
        if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(type)) {
            klass->emitReflectInvokeBody();
            klass->emitReflectNewBody();
            klass->finalizeClassObject();
        }
    }
}

// True when a test's stdlib-affecting flags match the defaults the stdlib was
// primed with. Only then is the cached stdlib IR valid for the test; otherwise
// the caller falls back to a fresh, fully-isolated Compiler (the original path).
// poison-free / drop-chain / stack-trace are runtime toggles set dynamically and
// do NOT change stdlib codegen, so they don't gate reuse.
bool stdlibReusable(const CajetaJit::Options& o) {
    return o.boundsCheckEnabled
        && o.overflowChecksEnabled
        && !o.boundsCheckMode.has_value()
        && !o.liveSetMode.has_value();
}

// Process-global cache: the stdlib parsed + codegen'd ONCE into a shared
// LLVMContext, with a captured post-stdlib baseline restored before each reusing
// test. Collapses the ~14s/test stdlib parse+codegen to a one-time cost.
struct StdlibReuseCache {
    llvm::LLVMContext sharedContext;
    std::unique_ptr<cajeta::Compiler> primeCompiler;   // owns the TargetMachine the stdlib references
    cajeta::CajetaModulePtr stdlibModule;
    std::map<std::string, cajeta::CajetaClassPtr> baselineStructures;
    std::set<std::string> baselineStructNames;   // stdlib llvm StructType names at prime
    // Per-baseline-function signature (instruction count; SIZE_MAX = declaration)
    // captured at prime. A reusing test that MUTATES or REMOVES a baseline
    // function corrupts the next test through the shared module — this names it.
    // New functions (template instantiations) are accumulation, not corruption.
    std::map<std::string, size_t> baselineFnSig;
    std::set<std::string> baselineGlobalNames;   // stdlib llvm GlobalVariable names
    bool primed = false;

    static StdlibReuseCache& instance() {
        static StdlibReuseCache c;
        return c;
    }

    void ensurePrimed() {
        if (primed) return;
        cajeta::Compiler::setSharedContext(&sharedContext);
        // First Compiler under the shared context primes the global type
        // tables (resetGlobals + init) in that context.
        primeCompiler = std::make_unique<cajeta::Compiler>();
        primeCompiler->ensureStdlibModule();            // parse stdlib once
        runCodegenPasses(primeCompiler->getModules());   // codegen stdlib bodies + static inits
        stdlibModule = cajeta::CajetaModule::getStdlibModule();
        // Snapshot the pristine state AFTER the stdlib is fully built.
        cajeta::CajetaType::captureBaseline();
        cajeta::CajetaModule::captureBaseline();
        baselineStructures = stdlibModule->getStructures();
        // Snapshot each baseline stdlib class's module-bound llvm bindings
        // (drop/vtable/RTTI/static-field globals + method functions) so
        // restoreBaseline can reset any that a reusing test lazily generated
        // into its own per-test module — the cross-module-reference leak.
        for (auto& [canon, klass] : baselineStructures)
            if (klass) klass->captureReuseBaseline();
        // Record the stdlib's llvm StructType names so per-test (user) struct
        // names can be stripped afterward — see clearTransientStructNames.
        for (auto* st : stdlibModule->getLlvmModule()->getIdentifiedStructTypes())
            if (st->hasName()) baselineStructNames.insert(st->getName().str());
        // Pristineness signature: per-function instruction count + global names.
        for (auto& F : *stdlibModule->getLlvmModule())
            baselineFnSig[F.getName().str()] =
                F.isDeclaration() ? SIZE_MAX : F.getInstructionCount();
        for (auto& G : stdlibModule->getLlvmModule()->globals())
            if (G.hasName()) baselineGlobalNames.insert(G.getName().str());
        primed = true;
    }

    // Diagnostic (CAJETA_STDLIB_VERIFY=1): after a reusing test's codegen, check
    // that no BASELINE stdlib function was mutated/removed in the shared module.
    // Such a change is the cross-test corruption that breaks the *next* test.
    // Reports MUTATED/REMOVED functions and the running count of accumulated
    // (new) functions + globals. Returns true if pristine. O(#stdlib functions)
    // — gated, only runs when explicitly verifying.
    bool verifyPristine(const std::string& label) {
        if (!primed) return true;
        auto* m = stdlibModule->getLlvmModule();
        std::vector<std::string> mutated, removed;
        size_t newFns = 0;
        std::set<std::string> seen;
        for (auto& F : *m) {
            std::string name = F.getName().str();
            seen.insert(name);
            auto it = baselineFnSig.find(name);
            if (it == baselineFnSig.end()) { ++newFns; continue; }
            size_t sig = F.isDeclaration() ? SIZE_MAX : F.getInstructionCount();
            if (sig != it->second)
                mutated.push_back(name + " (" + std::to_string(it->second)
                                  + "->" + std::to_string(sig) + ")");
        }
        for (auto& [name, sig] : baselineFnSig)
            if (!seen.count(name)) removed.push_back(name);
        size_t newGlobals = 0;
        std::vector<std::string> newFnNames, newGlobalNames;
        for (auto& F : *m)
            if (!baselineFnSig.count(F.getName().str()))
                newFnNames.push_back(F.getName().str()
                    + (F.isDeclaration() ? " (decl)" : " (def)"));
        for (auto& G : m->globals())
            if (G.hasName() && !baselineGlobalNames.count(G.getName().str())) {
                ++newGlobals;
                newGlobalNames.push_back(G.getName().str()
                    + (G.isDeclaration() ? " (decl)" : " (def)"));
            }
        if (std::getenv("CAJETA_STDLIB_VERIFY_NAMES")) {
            for (auto& s : newFnNames)
                std::fprintf(stderr, "[stdlib-verify]   +FN %s\n", s.c_str());
            for (auto& s : newGlobalNames)
                std::fprintf(stderr, "[stdlib-verify]   +GLOBAL %s\n", s.c_str());
        }
        bool pristine = mutated.empty() && removed.empty();
        if (!pristine || newFns || newGlobals) {
            std::fprintf(stderr,
                "[stdlib-verify] %s  %s  +%zu fns +%zu globals%s\n",
                label.c_str(), pristine ? "PRISTINE" : "*** LEAK ***",
                newFns, newGlobals,
                pristine ? "" : "  <-- baseline mutation");
        }
        for (auto& s : mutated)
            std::fprintf(stderr, "[stdlib-verify]   MUTATED %s\n", s.c_str());
        for (auto& s : removed)
            std::fprintf(stderr, "[stdlib-verify]   REMOVED %s\n", s.c_str());
        return pristine;
    }

    // Strip the NAMES of this test's user-defined struct types from the shared
    // context so the next test's `getOrCreateLlvmType` (CajetaType.cpp:1072,
    // StructType::getTypeByName) does NOT find a stale, already-bodied struct of
    // the same name (e.g. two tests both declaring `test.S`) — which would
    // double-set the body / impose a stale layout and crash. Baseline (stdlib)
    // struct names are preserved. Safe: the JIT module is a separate
    // bitcode-roundtripped context, so the live test is unaffected; only the
    // throwaway compile-context type identity is released.
    void clearTransientStructNames(llvm::Module* m) {
        if (!primed) return;
        for (auto* st : m->getIdentifiedStructTypes())
            if (st->hasName() && !baselineStructNames.count(st->getName().str()))
                st->setName("");
    }

    // Reset per-test global state to the captured post-stdlib baseline: drop
    // every user module's types/methods/structures AND any user-triggered
    // stdlib template instantiation's CAJETA-side objects. The stdlib
    // llvm::Module itself is left as-is — codegen must target it (the cached
    // baseline objects hold llvm::Function*/Global* pointers INTO it, so a new
    // instantiation that references a baseline vtable must be emitted there too;
    // emitting into a clone yields cross-module references). The merge takes a
    // self-contained CLONE, so the (growing) cached module is never consumed.
    // Instantiations accumulate in it across the shard — bounded and harmless
    // for built-in type args; see the known-limitation note at the call site.
    void restoreBaseline() {
        if (!primed) return;
        // Advance the reuse generation so per-template method-instantiation
        // caches (held on persistent stdlib Methods) invalidate stale entries
        // bound to the previous test's now-freed user emit module.
        cajeta::CajetaModule::bumpReuseEpoch();
        cajeta::CajetaType::restoreBaseline();
        cajeta::CajetaModule::restoreBaseline();   // re-pins the stdlib singleton
        stdlibModule->getStructures() = baselineStructures;
        // Lazy stdlib: the baseline was captured before any on-demand package
        // (cajeta.math) was parsed, and the restore above drops those types.
        // Clear the lazy bookkeeping too, so a later test importing the package
        // re-parses it instead of skipping it as "already parsed".
        cajeta::Compiler::resetLazyStdlibState();
        // Drop every method-template instantiation a PRIOR test registered on a
        // persistent stdlib class (e.g. ParallelDriver::forEachWorker<test.M>,
        // forEachParallelChain<test.M>). Such a class outlives the per-test user
        // module its instantiations were codegen'd into; left registered, the
        // next test's resolveMethod finds the stale entry and skips re-emitting
        // the body into ITS module — the call site then references an undefined
        // symbol at JIT link. The per-template methodInstantiationCache is already
        // epoch-invalidated; this clears the parallel registration on the host
        // class so the next test re-instantiates + re-codegens cleanly. (Class-
        // template instantiations like Stream<test.M> ride their own per-test
        // class object, which is dropped with the user module — no cleanup
        // needed.) Collect-then-remove to avoid mutating the maps mid-iteration.
        for (auto& [canonical, klass] : stdlibModule->getStructures()) {
            if (!klass) continue;
            std::vector<cajeta::MethodPtr> stale;
            for (auto& m : klass->getMethodList()) {
                if (m && m->isMethodTemplateInstantiation()) stale.push_back(m);
            }
            for (auto& m : stale) klass->removeMethod(m);
            // Reset this class's module-bound llvm bindings (drop/vtable/RTTI/
            // static-field globals + remaining method functions) that a reusing
            // test generated into its own per-test module, so the next test
            // regenerates into its module rather than referencing a freed one.
            klass->restoreReuseBaseline();
        }
    }
};

} // namespace

CajetaJit::CajetaJit() = default;

CajetaJit::~CajetaJit() {
    // Each JIT module's runtime statics include the carrier-thread flag and
    // pthread handle. A fresh module starts a fresh carrier on its FIRST
    // spawn — so test 1's carrier lives in test 1's data, and unloading the
    // module at the end of the test leaves an orphaned pthread blocked on a
    // condvar whose backing memory the JIT can recycle. The next test
    // spawns, its own carrier signals (or wakes the same VA), and the
    // orphan races back into now-defunct JIT code. Calling shutdown via the
    // module's own symbol (looked up here, while the module is still live)
    // joins the carrier cleanly before the LLJIT destructor frees the IR.
    if (jit) {
        auto sym = jit->lookup("__cajeta_task_shutdown");
        if (sym) {
            auto fn = reinterpret_cast<void(*)()>(sym->getValue());
            if (fn) fn();
        } else {
            cajeta::jittest::consumeError(sym.takeError());
        }
    }
}

std::unique_ptr<CajetaJit> CajetaJit::compile(const std::string& source,
                                              const std::string& fqClassName) {
    return compile(source, fqClassName, Options{});
}

std::unique_ptr<CajetaJit> CajetaJit::compile(const std::string& source,
                                              const std::string& fqClassName,
                                              const Options& opts) {
    // Single-source path is the multi-source one with a one-entry map.
    std::map<std::string, std::string> sources;
    sources.emplace(fqClassName, source);
    return compile(sources, fqClassName, opts);
}

std::unique_ptr<CajetaJit> CajetaJit::compile(
        const std::map<std::string, std::string>& sources,
        const std::string& fqEntryClass) {
    return compile(sources, fqEntryClass, Options{});
}

std::unique_ptr<CajetaJit> CajetaJit::compile(
        const std::map<std::string, std::string>& sources,
        const std::string& fqEntryClass,
        const Options& opts) {
    // Multi-source path. Lay every (fqClass, source) pair under a
    // shared temp source-root that matches its declared package
    // (so onPackageDeclaration's path/package check passes), then
    // delegate to the single-source path's same parse/codegen/JIT
    // pipeline by parsing each module into the same Compiler.
    ensureJitInitialized();

    // Coarse per-phase wall-clock timing, gated on CAJETA_JIT_TIMING, to locate
    // the per-test fixed cost (parse vs cajeta codegen vs LLJIT backend compile).
    const bool jitTiming = std::getenv("CAJETA_JIT_TIMING") != nullptr;
    auto timingStart = std::chrono::steady_clock::now();
    auto timingPrev = timingStart;
    auto mark = [&](const char* label) {
        if (!jitTiming) return;
        auto now = std::chrono::steady_clock::now();
        double step = std::chrono::duration<double, std::milli>(now - timingPrev).count();
        double total = std::chrono::duration<double, std::milli>(now - timingStart).count();
        std::fprintf(stderr, "[jit-timing] %-26s +%8.1f ms  (total %8.1f ms)\n",
                     label, step, total);
        timingPrev = now;
    };

    // Stdlib-reuse decision. When the test's stdlib-affecting flags match the
    // primed defaults, bind the next Compiler to the shared context + cached
    // stdlib (skips the ~14s re-parse/re-codegen). Otherwise force a fresh,
    // fully-isolated Compiler — the original path — so flag-sensitive stdlib
    // codegen stays correct.
    // Gated OFF by default. The reuse path collapses the ~14s/test stdlib
    // parse+codegen to a one-time prime and is correct for tests that don't
    // instantiate stdlib templates over USER types. Cross-test memory safety
    // for that case (a stdlib-template instantiation referencing a freed user
    // type, accumulated in the shared module) is NOT yet solved — it needs
    // either redirecting stdlib-template instantiation ownership to the user
    // module (a production codegen change) or per-test module reset with
    // cajeta-object rebinding. Until then the default path is the original
    // fully-isolated compile. Opt in with CAJETA_STDLIB_REUSE=1.
    static const bool kReuseEnabled = std::getenv("CAJETA_STDLIB_REUSE") != nullptr;
    bool reuseStdlib = kReuseEnabled && stdlibReusable(opts);
    auto& stdlibCache = StdlibReuseCache::instance();

    // The reuse path binds the process-global shared context (line ~499) per
    // test but only clears it on the fallback / non-reuse branches — so after a
    // SUCCESSFUL reuse test it would stay set, and the NEXT test to construct a
    // raw Compiler (e.g. parser/unit tests that don't use this harness) would
    // skip resetGlobals() (Compiler ctor's shared-context branch) and inherit
    // this test's stale process-global registries (aspectClasses, etc.) ->
    // cross-test contamination (AspectRegistrationTests.freshCompilerStartsEmpty
    // failed mid-shard for exactly this reason). Guarantee the context (and the
    // hazard arm) are cleared on EVERY exit from this helper — normal return and
    // exception alike. The next reuse test re-binds at line ~499; s_sharedInitialized
    // is left intact so the one-time prime is preserved.
    struct SharedContextGuard {
        ~SharedContextGuard() {
            cajeta::Compiler::setSharedContext(nullptr);
            cajeta::Compiler::setReuseHazardArmed(false);
        }
    } sharedContextGuard;

    static std::mt19937_64 rng(std::random_device{}());
    auto sourceRoot = std::filesystem::temp_directory_path()
                    / ("cajeta_multi_" + std::to_string(rng()));
    std::filesystem::create_directories(sourceRoot);

    std::vector<std::filesystem::path> sourcePaths;
    sourcePaths.reserve(sources.size());
    for (auto& [fqClass, source] : sources) {
        std::filesystem::path rel = classNameToRelativePath(fqClass);
        std::filesystem::path full = sourceRoot / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full);
        out << source;
        out.close();
        sourcePaths.push_back(full);
    }

    // Designate the ENTRY class's module as `primary` (the merge target) by
    // compiling it first — `primary` is set to whichever module is built first
    // (see the !primary check below). The default map order is alphabetical by
    // fqClass, which can make a NON-entry file primary and leave the entry's
    // body (and any template instantiation it triggers, e.g.
    // ArrayList<test.DemoClass>) in a DONOR module. Linking a donor that owns a
    // monomorphized instantiation into primary mis-resolves the instantiation's
    // struct type against a structurally-identical stdlib type, producing a
    // bad-vtable SIGSEGV at run() time. Keeping the entry (and the instantiation
    // it owns) IN primary matches the working single-entry-module path.
    {
        std::filesystem::path entryRel = classNameToRelativePath(fqEntryClass);
        std::filesystem::path entryFull = sourceRoot / entryRel;
        auto it = std::find(sourcePaths.begin(), sourcePaths.end(), entryFull);
        if (it != sourcePaths.end()) {
            std::rotate(sourcePaths.begin(), it, it + 1);
        }
    }

    auto archiveRoot = std::filesystem::temp_directory_path()
                     / ("cajeta_archive_" + sourceRoot.filename().string());
    std::filesystem::create_directories(archiveRoot);

    // (The just-written sources are pre-scanned into the archive INSIDE the
    // retry loop below — after the Compiler ctor's resetGlobals() / the reuse
    // path's restoreBaseline(), both of which clear g_archive. Scanning before
    // the loop would have its registrations wiped by that reset, leaving
    // forward-referenced user types (e.g. a `#M decode()` return whose `M` is
    // declared later) resolving to null. See prescanSourceRoot placement below.)

    // Speculative reuse with a fresh, fully-isolated fallback. When the test's
    // stdlib-affecting flags match the primed defaults, the first attempt binds
    // the next Compiler to the shared context + cached stdlib (skips the ~14s
    // re-parse/re-codegen) AND arms hazard detection. If the test triggers a
    // NOVEL stdlib-template instantiation (Stream<test.M>, HashMap<...>, ...) —
    // the one operation that would emit module-bound IR into a per-test user
    // module and contaminate the shared cache with cross-module references on
    // the next test — the instantiator throws ReuseHazardAbort BEFORE emitting,
    // and we transparently re-run the WHOLE parse on a fresh Compiler. Net: the
    // safe majority reuses (~4s), the hazardous minority degrades to today's
    // isolated cost (~16s), and reuse can never produce corruption — so it is
    // safe to leave on for the entire suite. Gated OFF unless
    // CAJETA_STDLIB_REUSE=1; production --emit never arms the gate.
    std::unique_ptr<CajetaJit> jitState;
    std::unique_ptr<Compiler> compiler;
    cajeta::CajetaModulePtr primary;
    for (;;) {
        if (reuseStdlib) {
            stdlibCache.ensurePrimed();
            stdlibCache.restoreBaseline();
            cajeta::Compiler::setSharedContext(&stdlibCache.sharedContext);
            cajeta::Compiler::setReuseHazardArmed(true);
        } else {
            cajeta::Compiler::setSharedContext(nullptr);
            cajeta::Compiler::setReuseHazardArmed(false);
        }

        jitState.reset(new CajetaJit);
        compiler = std::make_unique<Compiler>();
        compiler->setBoundsCheckEnabled(opts.boundsCheckEnabled);
        compiler->getMutableFlags().overflowChecks =
            opts.overflowChecksEnabled
                ? cajeta::OverflowChecks::On
                : cajeta::OverflowChecks::Wrapping;
        if (opts.boundsCheckMode.has_value()) {
            compiler->getMutableFlags().bounds = *opts.boundsCheckMode;
        }
        if (opts.liveSetMode.has_value()) {
            compiler->getMutableFlags().liveSet = *opts.liveSetMode;
        }
        // CAJETA_LAZY_SCOPE=1 runs the whole suite under --lazy-scope so the
        // safe lazy-frame path (ensure_at at spawn sites) gets full coverage.
        if (const char* lz = std::getenv("CAJETA_LAZY_SCOPE")) {
            if (lz[0] == '1' || lz[0] == 'o' || lz[0] == 'O' || lz[0] == 't') {
                compiler->getMutableFlags().lazyScope = true;
            }
        }

        // Pre-scan the just-written sources into the archive so forward
        // references across files can create placeholders (rather than throw or
        // silently null) when their type's declaration arrives later in the
        // parse order. MUST run here — after the Compiler ctor (fresh path:
        // resetGlobals() clears g_archive) and after restoreBaseline() (reuse
        // path: resets g_archive to the stdlib baseline). Both reset points
        // precede this in the loop body, so scanning here is the first point at
        // which the registrations survive to the body walk. Re-scanned each
        // retry iteration because the Compiler is reconstructed (and the archive
        // re-cleared) per attempt; registerArchive is first-write-wins so this
        // is idempotent.
        cajeta::prescanSourceRoot(sourceRoot.string());

        try {
            primary = nullptr;
            for (auto& sourcePath : sourcePaths) {
                auto m = compiler->createModule(sourcePath.string(),
                                                sourceRoot.string(),
                                                archiveRoot.string());
                // Reuse mode: designate the first user module as the per-test
                // fallback emit target BEFORE compiling it, so a stdlib-template
                // instantiation triggered during type resolution (no user-type
                // arg, no codegen frame) emits into this disposable user module
                // instead of the cached stdlib. TemplateInstantiator consults
                // this when nothing better is available.
                if (!primary) {
                    primary = m;
                    if (reuseStdlib) cajeta::CajetaModule::setReuseEmitModule(primary);
                }
                // CajetaException carries the diagnostic; the gtest "Unknown C++
                // exception" wrapper otherwise eats the message. Rethrow after
                // surfacing so the test harness still sees the failure.
                try {
                    compiler->compile(m);
                } catch (cajeta::Exception& e) {
                    std::cerr << "[CajetaException] " << e.getErrorId()
                        << ": " << e.getMessage() << "\n";
                    throw;
                }
            }
            mark("parse+compile(modules)");

            // Deferred body codegen runs HERE — inside the retry scope, with the
            // reuse hazard STILL ARMED. A novel cross-module instantiation that
            // surfaces only at call-site codegen (not during the parse above) is
            // the METHOD-template case: Json.parse<test.Outer> & friends are
            // monomorphized when MethodCallExpression lowers the call, deep in
            // runCodegenPasses. Arming the gate across this window lets the
            // instantiator throw ReuseHazardAbort BEFORE it emits corrupt
            // cross-module IR (previously a null-operand SIGSEGV in
            // JsonSynthesizerTests.parseNestedClass) so the test falls back to a
            // fresh, isolated compile like any other novel stdlib-template
            // instantiation. (Class-template instantiations triggered during
            // type resolution already aborted in the compile() loop above.)
            //
            // Reuse-mode codegen semantics: user code that instantiates a stdlib
            // template EMITS that instantiation's IR into a USER module
            // (resolution/emit separation — Method/CajetaClass swap to their emit
            // module while lowering), so the cached stdlib is never in the
            // per-test codegen list and stays byte-pristine (CAJETA_STDLIB_VERIFY
            // confirms +0/+0). In fresh mode the Compiler's own list already
            // includes the stdlib.
            runCodegenPasses(compiler->getModules());
            if (reuseStdlib) {
                // Diagnostic: assert the cached stdlib stayed byte-pristine this
                // test (CAJETA_STDLIB_VERIFY=1). See verifyPristine.
                static const bool kVerify = std::getenv("CAJETA_STDLIB_VERIFY") != nullptr;
                if (kVerify) stdlibCache.verifyPristine(fqEntryClass);
                // The per-test sink has done its job (all instantiation emitted);
                // drop the reference so it doesn't retain this test's primary module.
                cajeta::CajetaModule::setReuseEmitModule(nullptr);
            }
            mark("phase1/2 codegen loop");
            break;  // compiled + codegen'd cleanly under the current mode
        } catch (const cajeta::ReuseHazardAbort&) {
            // A novel stdlib-template instantiation — this test can't reuse
            // safely. Scrub the partial shared-context state (the abort fired
            // before any hazardous IR emitted) and retry fully isolated. Only
            // reachable when reuseStdlib was true.
            cajeta::CajetaModule::setReuseEmitModule(nullptr);
            cajeta::Compiler::setReuseHazardArmed(false);
            stdlibCache.restoreBaseline();
            cajeta::Compiler::setSharedContext(nullptr);
            reuseStdlib = false;
            if (std::getenv("CAJETA_STDLIB_REUSE_TRACE")) {
                std::fprintf(stderr,
                    "[stdlib-reuse] %s -> fresh fallback "
                    "(novel stdlib-template instantiation)\n",
                    fqEntryClass.c_str());
            }
            // loop: re-run the parse on a fresh, isolated Compiler
        } catch (...) {
            // A reusing test whose compile THROWS (e.g. an expected-error test
            // such as TemplateBasicTests.diamondWithoutInferableConstructorThrows)
            // never reaches the end-of-compile struct-name release below. The
            // user structs it created keep their names in the shared LLVMContext
            // and poison a later same-named test (TemplatedInterfaceV2Tests'
            // `Holder<int32>` GEPs into the stale layout). Free those names now —
            // while this test's modules and type tables are still intact — then
            // rethrow so the test still observes its expected failure. Only on
            // the reuse path; the ReuseHazardAbort retry above is handled
            // separately and never reaches here.
            if (reuseStdlib) {
                cajeta::CajetaType::releaseThrownTransientStructNames();
                cajeta::CajetaModule::setReuseEmitModule(nullptr);
                cajeta::Compiler::setReuseHazardArmed(false);
            }
            throw;
        }
    }
    cajeta::Compiler::setReuseHazardArmed(false);
    (void) fqEntryClass;

    // The compiler's stdlib module holds the parsed cajeta.error.*
    // classes + the runtime bitcode (parsed once per Compiler, see
    // Compiler::ensureStdlibModule). The merge below pulls it into
    // `primary` alongside the user modules — that's where extern
    // decls in user IR get resolved to real definitions.
    // (Codegen — the cross-module sweeps, REFL-1.7 Class<?> build, the
    // phase1/2 method loop, and static initializers — all run inside
    // runCodegenPasses, called within the retry loop above while the reuse
    // hazard was still armed. main ran them here post-parse; under reuse they
    // must live inside the hazard-armed window, so they were hoisted into
    // runCodegenPasses, which is also the prime path.)

    // (REFL-2 reflect-adapter bodies + #ClassObject finalize also run inside
    // runCodegenPasses now, at the end of the quiescence loop. Under reuse this
    // means stdlib classes get their adapters at PRIME — baseline-resident and
    // byte-pristine across tests — instead of being mutated into the cached
    // stdlib on first per-test use. The emit*/finalize calls are idempotent
    // (guarded by llvmReflect*BodyEmitted / non-null #ClassObject slot 0), so
    // per-test they only fire for the new user classes.)

    // Merge every secondary module into `primary`'s llvm::Module.
    // After the parse-stdlib-once refactor, each module owns only
    // its own classes' definitions + extern decls for everything
    // else, so a plain linkModules call works (no need for the old
    // OverrideFromSrc workaround). The Linker resolves each extern
    // decl to the single definition it finds across all donors.
    auto modulesList = compiler->getModules();
    for (auto& m : modulesList) {
        if (m == primary) continue;
        std::unique_ptr<llvm::Module> donor(m->getLlvmModule());
        if (llvm::Linker::linkModules(*primary->getLlvmModule(),
                                       std::move(donor))) {
            throw std::runtime_error("JIT module-merge failed");
        }
    }
    // Reuse mode: the persistent stdlib isn't in this Compiler's module list
    // (it's process-global, built once), so CLONE its IR — including any
    // template instantiations this test triggered into it — and link the
    // self-contained clone into `primary`. Cloning (not moving) leaves the
    // cached module available for the next test.
    if (reuseStdlib) {
        auto stdlibClone = llvm::CloneModule(*stdlibCache.stdlibModule->getLlvmModule());
        if (llvm::Linker::linkModules(*primary->getLlvmModule(),
                                       std::move(stdlibClone))) {
            throw std::runtime_error("JIT stdlib-clone merge failed");
        }
    }
    mark("linkModules merge");

    // CajetaXPU host launch: build each @Kernel's cubin and emit a global
    // constructor that registers it with the runtime, so a JIT'd
    // `kernel.launch(...)` resolves the device function by name at runtime.
    // The ctors run in jit->initialize() below (after the JIT is built). No-op
    // when there are no kernels or ptxas is unavailable.
    {
        std::vector<cajeta::MethodPtr> kernels;
        for (auto& m : compiler->getModules()) {
            for (auto& method : m->getAllMethods()) {
                if (method && cajeta::xpu::isKernel(*method)) {
                    kernels.push_back(method);
                }
            }
        }
        if (!kernels.empty()) {
            // Default to NVIDIA (the legacy JIT host-launch path); opts can
            // select CPU to drive the GPU-free fall-to-CPU dispatcher, or any
            // bundle. The manifest tells the runtime dispatcher which backends
            // were bundled (cajeta-cpu.md Increment 4).
            std::vector<cajeta::xpu::Backend> backends = opts.xpuBackends;
            if (backends.empty()) {
                backends.push_back(cajeta::xpu::Backend::Nvptx);
            }
            cajeta::xpu::emitBackendManifest(backends, *primary->getLlvmModule());
            for (cajeta::xpu::Backend be : backends) {
                std::string arch =
                    !opts.xpuArch.empty()              ? opts.xpuArch
                  : be == cajeta::xpu::Backend::Nvptx  ? "sm_89"
                  : be == cajeta::xpu::Backend::Amdgpu ? "gfx1151"
                  : be == cajeta::xpu::Backend::Spirv  ? "vulkan1.3"
                  :                                      "";
                cajeta::xpu::emitKernelRegistration(
                    be, kernels, *primary->getLlvmModule(), arch);
            }
        }
    }

    llvm::Module* llvmModule = primary->getLlvmModule();
    for (auto& fn : *llvmModule) {
        if (fn.isDeclaration()) continue;
        std::string fullName = fn.getName().str();
        // Cajeta-mangled names look like "package.Class::method(...)". Extract the
        // short method name between "::" and "(".
        size_t colon = fullName.find("::");
        if (colon == std::string::npos) continue;
        size_t lparen = fullName.find('(', colon);
        std::string shortName = (lparen == std::string::npos)
            ? fullName.substr(colon + 2)
            : fullName.substr(colon + 2, lparen - colon - 2);
        // Only the first match wins for a given short name (avoids ambiguity from
        // overloaded methods; tests should use unique names within a file).
        jitState->nameMap.emplace(shortName, fullName);
    }

    // Print before verify so a failed verify still shows the IR.
    if (std::getenv("CAJETA_DUMP_IR")) {
        llvmModule->print(llvm::errs(), nullptr);
    }
    // Verify before handing to JIT — gives clearer errors when codegen produced
    // malformed IR.
    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*llvmModule, &verifyStream)) {
        throw std::runtime_error("JIT verify failed: " + verifyErr);
    }
    mark("nameMap + verify");

    // Build the JIT. We need to extract the module out of the compiler's context so
    // we can wrap it in a ThreadSafeModule with its own context. Simplest: clone
    // bitcode-roundtrip would work, but here we just transfer the unique_ptr.
    auto tsCtx = std::make_unique<llvm::LLVMContext>();
    // To keep things simple and avoid context-transfer machinery, write the module
    // to bitcode and re-parse into a fresh context owned by ThreadSafeModule. This
    // is the documented LLJIT integration path when the source context isn't owned
    // by the caller.
    llvm::SmallVector<char, 0> bitcodeBuf;
    {
        llvm::raw_svector_ostream os(bitcodeBuf);
        llvm::WriteBitcodeToFile(*llvmModule, os);
    }
    auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(bitcodeBuf.data(), bitcodeBuf.size()), "cajeta_test");
    llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
    // LLVM 21 removed ThreadSafeContext::getContext() in favor of
    // withContextDo(...), which runs the callable under the context's mutex.
    // 18 / 20 still have getContext(). The parseBitcodeFile call needs the raw
    // LLVMContext, so we preprocess-branch on LLVM_VERSION_MAJOR.
#if LLVM_VERSION_MAJOR >= 21
    auto parsed = tsContext.withContextDo(
        [&](llvm::LLVMContext* ctx) {
            return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), *ctx);
        });
#else
    auto parsed = llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                          *tsContext.getContext());
#endif
    if (!parsed) {
        std::string err = cajeta::jittest::toString(parsed.takeError());
        throw std::runtime_error("JIT bitcode reparse failed: " + err);
    }
    // Honor `alwaysinline` before handing the module to LLJIT. A bare LLJIT runs
    // no inliner, so @ValueType operators / @Device helpers marked alwaysinline
    // would otherwise stay real calls in JIT execution. optimizeModule at O0 runs
    // exactly the AlwaysInlinerPass (see Optimizer.cpp / value-type plan S1b).
    cajeta::optimizeModule(**parsed, nullptr, cajeta::OptLevel::O0);
    mark("bitcode roundtrip + O0");
    // IR capture (S0/S4): stash the post-codegen, post-AlwaysInline (O0) host IR
    // so a test can assert the lowered shape — the flat <N x T> Vector intrinsics
    // (S0, unaffected by an inline-only O0 pass) AND whether a @ValueType operator
    // folded to its caller (S4). Captured AFTER optimizeModule so the fold is
    // visible. Opt-in via Options::captureIr so the normal JIT path pays nothing.
    if (opts.captureIr) {
        llvm::raw_string_ostream irStream(jitState->moduleIr);
        (*parsed)->print(irStream, nullptr);
        irStream.flush();
    }
    llvm::orc::ThreadSafeModule tsModule(std::move(*parsed), std::move(tsContext));

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        throw std::runtime_error("LLJIT create failed");
    }
    jitState->jit = std::move(*jitOrErr);

    // Opt-in GDB JIT symbolization (CAJETA_JIT_GDB=1): registers each JIT'd
    // object with the GDB JIT interface so a debugger can name JIT frames
    // instead of showing bare addresses. Requires the JITLink ObjectLinkingLayer
    // (the LLVM 22 default on x86-64 ELF); on RTDyld it returns an Error which
    // we surface and continue. Unset (the normal case) → behavior unchanged.
    if (std::getenv("CAJETA_JIT_GDB")) {
        if (auto err = llvm::orc::enableDebuggerSupport(*jitState->jit)) {
            llvm::errs() << "[jit-gdb] enableDebuggerSupport failed: "
                         << cajeta::jittest::toString(std::move(err)) << "\n";
        } else {
            llvm::errs() << "[jit-gdb] GDB JIT symbolization enabled\n";
        }
    }

    if (auto err = jitState->jit->addIRModule(std::move(tsModule))) {
        throw std::runtime_error("LLJIT addIRModule failed");
    }
    mark("LLJIT create + addIRModule");

    // Make host process symbols (printf, malloc, etc.) resolvable from JIT'd code.
    auto& mainDylib = jitState->jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jitState->jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        throw std::runtime_error("LLJIT process-symbol generator failed");
    }
    mainDylib.addGenerator(std::move(*generator));

#ifdef _WIN32
    // The generator above dlsym's the current process, but MinGW's CRT /
    // libgcc functions are statically linked into this test binary and not
    // in its PE export table, so it can't find them — the embedded runtime
    // bitcode's calls to write/open/__mingw_fprintf/stat64i32/... then fail
    // to materialize, cascading to every runtime-dependent symbol. Bind them
    // explicitly to the addresses JitWinSymbols.c took (in a TU compiled with
    // the same MinGW headers as the runtime, so each address is the exact
    // entry point the bitcode references).
    {
        size_t winSymCount = 0;
        const CajetaJitWinSym* winSyms = cajeta_jit_win_symbols(&winSymCount);
        auto& execSession = jitState->jit->getExecutionSession();
        llvm::orc::SymbolMap winSymMap;
        for (size_t i = 0; i < winSymCount; ++i) {
            winSymMap[execSession.intern(winSyms[i].name)] =
                llvm::orc::ExecutorSymbolDef(
                    llvm::orc::ExecutorAddr::fromPtr(winSyms[i].addr),
                    llvm::JITSymbolFlags::Exported);
        }
        cajeta::jittest::cantFail(
            mainDylib.define(llvm::orc::absoluteSymbols(std::move(winSymMap))));
    }
#endif

    // Bind the cajeta.net TLS engine symbols (native-only, see the extern block
    // at the top). Done on every platform: on MinGW the process generator can't
    // find exe-local symbols; elsewhere absoluteSymbols simply wins over the
    // (redundant) generator entry. The engine functions are self-contained — the
    // SSL_*/BIO_* calls inside them are already resolved by native linking, so
    // the JIT only needs these entry points.
    {
        auto& execSession = jitState->jit->getExecutionSession();
        llvm::orc::SymbolMap tlsSyms;
        auto bind = [&](const char* name, void* addr) {
            tlsSyms[execSession.intern(name)] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(addr),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
        };
        bind("__cajeta_tls_ctx_new", (void*) &__cajeta_tls_ctx_new);
        bind("__cajeta_tls_ctx_use_cert_key_pem", (void*) &__cajeta_tls_ctx_use_cert_key_pem);
        bind("__cajeta_tls_ctx_free", (void*) &__cajeta_tls_ctx_free);
        bind("__cajeta_tls_ctx_set_verify", (void*) &__cajeta_tls_ctx_set_verify);
        bind("__cajeta_tls_ctx_add_trust_pem", (void*) &__cajeta_tls_ctx_add_trust_pem);
        bind("__cajeta_tls_ctx_use_system_trust", (void*) &__cajeta_tls_ctx_use_system_trust);
        bind("__cajeta_tls_conn_new", (void*) &__cajeta_tls_conn_new);
        bind("__cajeta_tls_set_verify_host", (void*) &__cajeta_tls_set_verify_host);
        bind("__cajeta_tls_verify_result", (void*) &__cajeta_tls_verify_result);
        bind("__cajeta_tls_set_sni", (void*) &__cajeta_tls_set_sni);
        bind("__cajeta_tls_set_alpn", (void*) &__cajeta_tls_set_alpn);
        bind("__cajeta_tls_get_alpn", (void*) &__cajeta_tls_get_alpn);
        bind("__cajeta_tls_ctx_set_alpn_select", (void*) &__cajeta_tls_ctx_set_alpn_select);
        bind("__cajeta_tls_feed_ciphertext", (void*) &__cajeta_tls_feed_ciphertext);
        bind("__cajeta_tls_pull_ciphertext", (void*) &__cajeta_tls_pull_ciphertext);
        bind("__cajeta_tls_pending_ciphertext", (void*) &__cajeta_tls_pending_ciphertext);
        bind("__cajeta_tls_handshake_step", (void*) &__cajeta_tls_handshake_step);
        bind("__cajeta_tls_write_plaintext", (void*) &__cajeta_tls_write_plaintext);
        bind("__cajeta_tls_read_plaintext", (void*) &__cajeta_tls_read_plaintext);
        bind("__cajeta_tls_shutdown", (void*) &__cajeta_tls_shutdown);
        bind("__cajeta_tls_free", (void*) &__cajeta_tls_free);
        cajeta::jittest::cantFail(
            mainDylib.define(llvm::orc::absoluteSymbols(std::move(tlsSyms))));
    }

    // Run any global ctors / static initializers (P6.2 clinit, etc.)
    // before handing control to test code. LLJIT does NOT run
    // llvm.global_ctors automatically — the host runtime's
    // `__attribute__((constructor))` functions (__cajeta_runtime_init,
    // __cajeta_hash_seed_init) only fire because the runtime is also
    // statically linked into the test binary. Anything that lives ONLY
    // in the JIT module (per-class clinit emitted by
    // CajetaClass::generateStaticInitializers) needs this initialize
    // call to execute its initializer.
    if (auto err = jitState->jit->initialize(mainDylib)) {
        throw std::runtime_error("LLJIT initialize failed: "
            + cajeta::jittest::toString(std::move(err)));
    }
    mark("LLJIT initialize (ctors)");

    // Apply per-test runtime-flag overrides. The runtime ships in two
    // places — the embedded bitcode merged into the JIT module, AND
    // the native object linked into the test binary. Each has its own
    // copy of `__cajeta_poison_free_enabled` (the static is module-
    // local). The JIT'd user code calls the JIT's free path → JIT's
    // poison helper → JIT's flag. The test's `extern "C"` direct
    // calls hit the host's flag. Both copies must stay in sync, so
    // we set both here on every compile() — gives each test a
    // deterministic starting point regardless of what the previous
    // test left behind.
    int desiredPoison = opts.poisonFreeEnabled ? 1 : 0;
    if (auto sym = jitState->jit->lookup("__cajeta_set_poison_free")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getValue());
        if (setFn) setFn(desiredPoison);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_poison_free(desiredPoison);

    int desiredValidate = opts.dropChainValidateEnabled ? 1 : 0;
    if (auto sym = jitState->jit->lookup("__cajeta_set_drop_chain_validate")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getValue());
        if (setFn) setFn(desiredValidate);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_drop_chain_validate(desiredValidate);

    int desiredTrace = opts.stackTraceCaptureEnabled ? 1 : 0;
    if (auto sym = jitState->jit->lookup("__cajeta_set_stack_trace_capture")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getValue());
        if (setFn) setFn(desiredTrace);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_stack_trace_capture(desiredTrace);

    // Reuse mode: release this test's user struct-type names from the shared
    // context so the next test can re-declare same-named classes (`test.S`)
    // without colliding with this test's now-stale, already-bodied structs.
    // The JIT module is a separate bitcode-roundtripped context, so the live
    // test is unaffected. (llvmModule is primary's compile-context module.)
    if (reuseStdlib) {
        stdlibCache.clearTransientStructNames(llvmModule);
    }

    return jitState;
}

void* CajetaJit::lookupAddress(const std::string& shortName) {
    auto it = nameMap.find(shortName);
    if (it == nameMap.end()) return nullptr;
    auto sym = jit->lookup(it->second);
    if (!sym) {
        cajeta::jittest::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getValue());
}

void* CajetaJit::lookupRawSymbol(const std::string& exactName) {
    auto sym = jit->lookup(exactName);
    if (!sym) {
        cajeta::jittest::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getValue());
}

} // namespace cajeta_test
