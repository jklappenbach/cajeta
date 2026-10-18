//
// Implementation of the JIT test helper.
//

#include "JitTestHelper.h"
#include "jit/CoffSafeJit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include "cajeta/ownership/FieldStoreCensus.h"
#include "cajeta/buildtool/IrCache.h"
#include "cajeta/buildtool/Lockfile.h"   // sha256Hex — the shared stdlib object key
#include "cajeta/buildtool/PrimeCache.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/core/XpuKernelGate.h"
#include "cajeta/compile/StdlibReuseCore.h"
#include "cajeta/compile/DropBackfill.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/compile/Optimizer.h"   // optimizeModule — runs AlwaysInlinerPass at O0
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/LaunchBlocks.h"

#include "JitWinSymbols.h"
#include "JitErrorShim.h"

#include <list>
#include <set>

#include "cajeta/type/CajetaClass.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Transforms/Utils/Cloning.h"   // CloneModule — the shared stdlib dylib
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <functional>
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
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
}

namespace cajeta_test {

using namespace cajeta;

namespace {

// Initialize LLVM JIT machinery once per process. ORC needs these — InitializeAll*
// from the Compiler ctor only set up codegen targets; the JIT also wants the
// native asm parser.
void ensureJitInitialized() {
    // Magic-static: C++ guarantees thread-safe once-init, so concurrent compiles
    // don't race on the JIT one-time init (threadsafe U5 concurrency-first).
    static const bool initialized = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        return true;
    }();
    (void) initialized;
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
    static thread_local std::mt19937_64 rng(std::random_device{}());  // per-thread (U5)
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
    if (cajeta::ownership::FieldStoreCensus::enabled()) {
        cajeta::ownership::FieldStoreCensus::run(modules);
    }
    cajeta::ownership::FieldStoreCensus::check(modules);

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
        // Vtables for classes whose implemented interface was a
        // lazy-package placeholder at prototype time (drain order).
        for (auto& m : modules)
            m->completePendingInterfaceVTables();
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
        && !o.liveSetMode.has_value()
        // A CPU/feature override changes stdlib codegen too, so the baseline
        // primed at the default target is not valid for it.
        && o.cpu.empty()
        && o.features.empty()
        // cajeta-profiler U10: an instrumented compile adds a #ProfMethod
        // global and a registration ctor to whatever module the builder is
        // in — including the SHARED stdlib module. That is exactly the
        // baseline mutation verifyPristine exists to catch, and it would
        // land on the next test rather than this one. Instrumented compiles
        // take the fresh path.
        && o.profiler == cajeta::Profiler::Off
        // ...and so must a --line-info=off compile, for the same reason with
        // the sign flipped. Reuse shares ONE runtime copy, and line-info
        // presence is published into it by a global ctor
        // (__cajeta.lineinfo.register). A reusing lineInfo=off test therefore
        // borrows a runtime where an EARLIER lineInfo=on test already set the
        // flag, and __cajeta_line_info_is_present answers 1 for a build that
        // emitted no probes at all.
        //
        // That is not cosmetic: it is the exact state §2.5's refusal exists to
        // detect, so the test that pins the refusal was the one it broke —
        // and only under the sweep, which exports CAJETA_STDLIB_REUSE=1 while
        // a bare run does not. Found by the full suite; a targeted run and
        // even 24-way contention both pass.
        && o.lineInfoEnabled;
}

// Process-global cache: the stdlib parsed + codegen'd ONCE into a shared
// LLVMContext, with a captured post-stdlib baseline restored before each reusing
// test. Collapses the ~14s/test stdlib parse+codegen to a one-time cost.
//
// The prime/restore CORE (shared context, prime Compiler, type/module/xref
// baselines, structures snapshot, lazy-stdlib + instantiation rollback)
// lives in src/cajeta/compile/StdlibReuseCore (factored for the lint
// server, plan 1.2.1). This cache LAYERS the JIT-specific pieces on top:
// the codegen passes at prime, llvm-level pristineness snapshots
// (verifyPristine), transient struct-name release, the prime-cache lookup
// seam, and the shared stdlib dylib fork.
// The prime cache's root directory: CAJETA_PRIME_CACHE_DIR, else
// <CAJETA_SOURCE_ROOT>/.cajeta/cache/prime; empty when CAJETA_PRIME_CACHE=0
// or neither variable is set.
std::string primeCacheRoot() {
    const char* toggle = std::getenv("CAJETA_PRIME_CACHE");
    if (toggle && std::string(toggle) == "0") return {};
    if (const char* dir = std::getenv("CAJETA_PRIME_CACHE_DIR")) return dir;
    if (const char* src = std::getenv("CAJETA_SOURCE_ROOT"))
        return std::string(src) + "/.cajeta/cache/prime";
    return {};
}

// Machine code for the shared stdlib module, kept across processes under
// <root>/obj/<target>/<bitcode digest>.o (compile-cache plan Unit 4). Only the
// module named `kPrefix + digest` is stored or served; a stored object that no
// longer parses is deleted and recompiled.
class StdlibObjectCache : public llvm::ObjectCache {
public:
    static constexpr const char* kPrefix = "cajeta-prime:";
    static constexpr size_t kKeep = 4;

    explicit StdlibObjectCache(std::filesystem::path root) : root_(std::move(root)) {}

    // Pins the pool to one target: triple, cpu and feature string.
    void setTarget(const std::string& targetKey) {
        pool_ = root_ / "obj" / cajeta::buildtool::sha256Hex(targetKey).substr(0, 16);
    }

    void notifyObjectCompiled(const llvm::Module* m, llvm::MemoryBufferRef obj) override {
        std::filesystem::path target = pathFor(m);
        if (target.empty()) return;
        ++compiled_;
        std::error_code ec;
        std::filesystem::create_directories(pool_, ec);
        if (ec) return;
        std::filesystem::path tmp = target;
        tmp += "." + std::to_string(llvm::sys::Process::getProcessId()) + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(obj.getBufferStart(), (std::streamsize) obj.getBufferSize());
            if (!out.good()) return;
        }
        std::filesystem::rename(tmp, target, ec);
        if (ec) std::filesystem::remove(tmp, ec);
        evict(target);
    }

    std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* m) override {
        std::filesystem::path target = pathFor(m);
        if (target.empty()) return nullptr;
        auto buf = llvm::MemoryBuffer::getFile(target.string());
        if (!buf) return nullptr;
        if (!wellFormed((*buf)->getMemBufferRef())) {
            std::error_code ec;
            std::filesystem::remove(target, ec);
            return nullptr;
        }
        ++served_;
        return std::move(*buf);
    }

    size_t served() const { return served_; }
    size_t compiled() const { return compiled_; }

private:
    std::filesystem::path pathFor(const llvm::Module* m) const {
        llvm::StringRef id = m->getModuleIdentifier();
        if (pool_.empty() || !id.starts_with(kPrefix)) return {};
        return pool_ / (id.drop_front(std::strlen(kPrefix)).str() + ".o");
    }

    // Every section's contents must lie within the file.
    static bool wellFormed(llvm::MemoryBufferRef buf) {
        auto obj = llvm::object::ObjectFile::createObjectFile(buf);
        if (!obj) {
            cajeta::jittest::consumeError(obj.takeError());
            return false;
        }
        for (const llvm::object::SectionRef& sec : (*obj)->sections()) {
            auto contents = sec.getContents();
            if (!contents) {
                cajeta::jittest::consumeError(contents.takeError());
                return false;
            }
        }
        return true;
    }

    // Keeps the newest kKeep objects in the pool, `keep` among them.
    void evict(const std::filesystem::path& keep) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<std::pair<fs::file_time_type, fs::path>> objects;
        for (auto& e : fs::directory_iterator(pool_, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".o" || e.path() == keep) continue;
            objects.emplace_back(e.last_write_time(ec), e.path());
        }
        std::sort(objects.begin(), objects.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = kKeep - 1; i < objects.size(); ++i) fs::remove(objects[i].second, ec);
    }

    std::filesystem::path root_;
    std::filesystem::path pool_;
    size_t served_ = 0;
    size_t compiled_ = 0;
};

struct StdlibReuseCache {
    cajeta::CajetaModulePtr stdlibModule;   // = StdlibReuseCore's, cached for the llvm-level probes
    std::set<std::string> baselineStructNames;   // stdlib llvm StructType names at prime
    // Per-baseline-function signature (instruction count; SIZE_MAX = declaration)
    // captured at prime. A reusing test that MUTATES or REMOVES a baseline
    // function corrupts the next test through the shared module — this names it.
    // New functions (template instantiations) are accumulation, not corruption.
    std::map<std::string, size_t> baselineFnSig;
    std::set<std::string> baselineGlobalNames;   // stdlib llvm GlobalVariable names
    // [compile-cache Unit 2] the computed prime key + any validated hit from
    // the lookup seam. The hit path is recorded but not consumed until the
    // Unit 3 loader lands; Unit 3's store-on-miss also hangs off these.
    cajeta::Compiler::PrimeCacheKey primeCacheKey;
    std::optional<std::string> primeCacheHitPath;
    bool primed = false;

    static StdlibReuseCache& instance() {
        static StdlibReuseCache c;
        return c;
    }

    void ensurePrimed() {
        if (primed) return;
        // [compile-cache plan Unit 1] phase-split instrumentation, env-gated.
        const bool kPrimeTiming = std::getenv("CAJETA_PRIME_TIMING") != nullptr;
        auto _ptStart = std::chrono::steady_clock::now();
        // [compile-cache plan Unit 2] persistent-prime lookup seam (spec §3).
        // Key computed + lookup issued here; nothing is STORED yet (the
        // artifact writer is Unit 3), so today every lookup is a miss and
        // the compile below runs exactly as before — zero behaviour change.
        // Disable outright with CAJETA_PRIME_CACHE=0; relocate with
        // CAJETA_PRIME_CACHE_DIR.
        primeCacheHitPath.reset();
        {
            std::string root = primeCacheRoot();
            if (!root.empty()) {
                primeCacheKey = cajeta::Compiler::stdlibPrimeCacheKey();
                cajeta::buildtool::IrCache cache(root);
                if (auto hit = cajeta::buildtool::primeValidatedLookup(
                        cache, primeCacheKey.discriminator,
                        primeCacheKey.digest)) {
                    primeCacheHitPath = *hit;
                }
                if (kPrimeTiming) {
                    std::fprintf(stderr,
                        "[CAJETA_PRIME_CACHE] %s  disc=%s digest=%.12s...\n",
                        primeCacheHitPath ? "HIT (unused: loader lands in Unit 3)"
                                          : "MISS",
                        primeCacheKey.discriminator.c_str(),
                        primeCacheKey.digest.c_str());
                }
            }
        }
        // (a) front-end + (b) IR-gen, via the shared core: the core primes
        // the front-end (parse + prototypes) and captures the global
        // baselines; the layer callback adds the JIT-only codegen passes,
        // after which the core RE-captures so restores return to the
        // post-codegen state — the same net sequence the pre-factoring
        // cache ran inline.
        auto& core = cajeta::StdlibReuseCore::instance();
        core.ensurePrimed();                             // (a) front-end: parse + typecheck stdlib
        auto _ptAfterFrontEnd = std::chrono::steady_clock::now();
        core.ensureCodegenLayer([](cajeta::Compiler& prime) {
            runCodegenPasses(prime.getModules());        // (b) IR-gen: codegen bodies + static inits
            // Every concrete class's vtable gets its drop slot now: the shared
            // stdlib dylib materializes these vtables once, and a slot a later
            // program would have patched in the IR stays null in the JIT.
            for (auto& [canon, type] : cajeta::CajetaType::getCanonicalMap()) {
                auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(type);
                if (klass && !klass->isTemplate() && !klass->isPlaceholder())
                    klass->patchVirtualTableDropFn();
            }
        });
        auto _ptAfterIrGen = std::chrono::steady_clock::now();
        stdlibModule = core.getStdlibModule();
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
        if (kPrimeTiming) {
            auto _ptEnd = std::chrono::steady_clock::now();
            auto ms = [](auto a, auto b) {
                return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
            };
            // (c) JIT native-compile is lazy (ORC materializes on first call at test
            // time) — measure it as (first-test wall-clock − test logic), not here.
            std::fprintf(stderr,
                "[CAJETA_PRIME_TIMING] front_end_a=%lldms ir_gen_b=%lldms baseline=%lldms total_prime=%lldms\n",
                ms(_ptStart, _ptAfterFrontEnd), ms(_ptAfterFrontEnd, _ptAfterIrGen),
                ms(_ptAfterIrGen, _ptEnd), ms(_ptStart, _ptEnd));
        }
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

    // Reset per-test global state to the captured post-stdlib baseline. The
    // mechanics (reuse-epoch bump, type/module registries, structures, lazy
    // stdlib rollback, method-template-instantiation removal, per-class llvm
    // binding reset) live in StdlibReuseCore::restoreBaseline — the factored
    // core this cache layers on. The stdlib llvm::Module itself is left
    // as-is — codegen must target it (the cached baseline objects hold
    // llvm::Function*/Global* pointers INTO it, so a new instantiation that
    // references a baseline vtable must be emitted there too; emitting into
    // a clone yields cross-module references). The merge takes a
    // self-contained CLONE, so the (growing) cached module is never
    // consumed. Instantiations accumulate in it across the shard — bounded
    // and harmless for built-in type args; see the known-limitation note at
    // the call site.
    void restoreBaseline() {
        if (!primed) return;
        cajeta::StdlibReuseCore::instance().restoreBaseline();
    }

    // ===================== U7 shared stdlib JITDylib =======================
    // Persistent LLJIT hosting the materialized stdlib in its OWN JITDylib, so
    // every reusing compile's user dylib links ONE stdlib (addToLinkOrder)
    // instead of cloning and re-JITing it. Built on the first reusing compile.
    // See SharedStdlibDylibSpikeTests for the proven model.
    std::unique_ptr<StdlibObjectCache> objectCache;   // outlives sharedJit
    std::unique_ptr<llvm::orc::LLJIT> sharedJit;
    llvm::orc::JITDylib* sharedStdlibJD = nullptr;
    bool sharedJitReady = false;
    size_t sharedMaterializations = 0;
    // The stdlib's static initializers, in ctor order, and the runtime's
    // execution-epoch entry points, all resolved in the shared stdlib dylib.
    std::vector<void (*)()> stdlibClinits;
    int32_t (*epochPush)() = nullptr;
    void (*epochPopTo)(int32_t) = nullptr;
    // Every stdlib static field's address and its bytes right after the
    // stdlib's own initialization, restored before each execution.
    std::vector<std::pair<void*, std::string>> stdlibStatics;

    // Starts one program's execution against the shared stdlib: pushes a
    // runtime epoch, restores every stdlib static field and re-runs the
    // static initializers, so the program sees the statics a fresh process
    // would. Returns the epoch depth.
    int32_t beginExecution() {
        int32_t depth = epochPush ? epochPush() : 0;
        for (auto& [addr, bytes] : stdlibStatics) std::memcpy(addr, bytes.data(), bytes.size());
        for (auto fn : stdlibClinits) fn();
        return depth;
    }

    // The stdlib's static field globals: `<class>.<field>`, external, mutable.
    static std::vector<std::pair<std::string, uint64_t>> collectStatics(
            llvm::Module& m, const llvm::DataLayout& dl) {
        std::vector<std::pair<std::string, uint64_t>> out;
        for (auto& g : m.globals()) {
            if (!g.hasInitializer() || !g.hasExternalLinkage() || g.isConstant()
                    || g.isThreadLocal()) continue;
            llvm::StringRef name = g.getName();
            if (name.starts_with("llvm.") || name.starts_with("__cajeta")
                    || !name.contains('.') || name.contains('#')) continue;
            out.emplace_back(name.str(), dl.getTypeAllocSize(g.getValueType()));
        }
        return out;
    }

    // Ends that execution: every registration its constructors made is dropped.
    void endExecution(int32_t depth) {
        if (epochPopTo && depth > 0) epochPopTo(depth);
    }

    // Names of every definition the shared stdlib dylib holds, including its ctors.
    std::set<std::string> materializedDefs;

    static bool definesValue(const llvm::GlobalValue* gv) {
        if (auto* f = llvm::dyn_cast<llvm::Function>(gv)) return !f->isDeclaration();
        if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(gv)) return g->hasInitializer();
        return false;
    }

    static std::vector<std::pair<llvm::Function*, unsigned>> ctorsOf(llvm::Module& m) {
        std::vector<std::pair<llvm::Function*, unsigned>> out;
        auto* ctors = m.getGlobalVariable("llvm.global_ctors");
        if (!ctors || !ctors->hasInitializer()) return out;
        auto* list = llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
        if (!list) return out;
        for (unsigned i = 0; i < list->getNumOperands(); ++i) {
            auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(list->getOperand(i));
            if (!entry || entry->getNumOperands() < 2) continue;
            auto* fn = llvm::dyn_cast<llvm::Function>(entry->getOperand(1)->stripPointerCasts());
            auto* prio = llvm::dyn_cast<llvm::ConstantInt>(entry->getOperand(0));
            if (fn) out.emplace_back(fn, prio ? (unsigned) prio->getZExtValue() : 65535u);
        }
        return out;
    }

    void noteDefinitions(llvm::Module& m) {
        for (auto& f : m) if (!f.isDeclaration()) materializedDefs.insert(f.getName().str());
        for (auto& g : m.globals()) if (g.hasInitializer()) materializedDefs.insert(g.getName().str());
    }

    // Adds a module holding the stdlib definitions created since the last
    // materialization (lazy classes, instantiations, drop thunks, statics a
    // program was first to reference) to the shared stdlib dylib, with the
    // local values they reach copied alongside, and runs its constructors.
    void materializeStdlibDelta() {
        if (!sharedJitReady) return;
        llvm::Module& src = *stdlibModule->getLlvmModule();
        std::set<const llvm::GlobalValue*> keep;
        std::vector<const llvm::GlobalValue*> work;
        auto consider = [&](const llvm::GlobalValue* gv) {
            if (!gv || !definesValue(gv) || keep.count(gv)) return;
            keep.insert(gv);
            work.push_back(gv);
        };
        for (auto& f : src)
            if (!f.hasLocalLinkage() && !materializedDefs.count(f.getName().str())) consider(&f);
        for (auto& g : src.globals())
            if (!g.hasLocalLinkage() && !g.getName().starts_with("llvm.")
                    && !materializedDefs.count(g.getName().str())) consider(&g);
        std::vector<std::pair<llvm::Function*, unsigned>> newCtors;
        for (auto& [fn, prio] : ctorsOf(src))
            if (!materializedDefs.count(fn->getName().str())) {
                consider(fn);
                newCtors.emplace_back(fn, prio);
            }
        if (work.empty()) return;
        std::function<void(const llvm::Value*)> scan = [&](const llvm::Value* v) {
            if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(v)) {
                if (gv->hasLocalLinkage()) consider(gv);
                return;
            }
            if (auto* c = llvm::dyn_cast<llvm::Constant>(v))
                for (unsigned i = 0; i < c->getNumOperands(); ++i) scan(c->getOperand(i));
        };
        while (!work.empty()) {
            const llvm::GlobalValue* gv = work.back();
            work.pop_back();
            if (auto* f = llvm::dyn_cast<llvm::Function>(gv)) {
                for (auto& bb : *f)
                    for (auto& inst : bb)
                        for (unsigned i = 0; i < inst.getNumOperands(); ++i)
                            scan(inst.getOperand(i));
            } else if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(gv)) {
                scan(g->getInitializer());
            }
        }
        llvm::ValueToValueMapTy vm;
        auto delta = llvm::CloneModule(src, vm,
            [&](const llvm::GlobalValue* gv) { return keep.count(gv) > 0; });
        for (const char* name : {"llvm.global_ctors", "llvm.global_dtors", "llvm.used",
                                 "llvm.compiler.used"})
            if (auto* g = delta->getGlobalVariable(name)) g->eraseFromParent();
        for (auto& [fn, prio] : newCtors)
            if (auto* cloned = llvm::dyn_cast_or_null<llvm::Function>(vm.lookup(fn)))
                llvm::appendToGlobalCtors(*delta, cloned, (int) prio);
        std::vector<std::string> clinitNames = exportClinits(*delta);
        auto staticNames = collectStatics(*delta, sharedJit->getDataLayout());
        size_t defs = keep.size();
        for (const llvm::GlobalValue* gv : keep) materializedDefs.insert(gv->getName().str());

        llvm::SmallVector<char, 0> bc;
        { llvm::raw_svector_ostream os(bc); llvm::WriteBitcodeToFile(*delta, os); }
        auto memBuf = llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef(bc.data(), bc.size()), "cajeta_shared_stdlib_delta");
        llvm::orc::ThreadSafeContext tsContext(std::make_unique<llvm::LLVMContext>());
#if LLVM_VERSION_MAJOR >= 21
        auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
            return llvm::parseBitcodeFile(memBuf->getMemBufferRef(), *ctx); });
#else
        auto parsed = llvm::parseBitcodeFile(memBuf->getMemBufferRef(),
                                             *tsContext.getContext());
#endif
        if (!parsed)
            throw std::runtime_error("shared stdlib delta reparse failed: "
                + cajeta::jittest::toString(parsed.takeError()));
        cajeta::optimizeModule(**parsed, nullptr, cajeta::OptLevel::O0);
        llvm::orc::ThreadSafeModule tsm(std::move(*parsed), std::move(tsContext));
        if (auto err = sharedJit->addIRModule(*sharedStdlibJD, std::move(tsm)))
            throw std::runtime_error("shared stdlib delta addIRModule failed: "
                + cajeta::jittest::toString(std::move(err)));
        if (auto err = sharedJit->initialize(*sharedStdlibJD))
            throw std::runtime_error("shared stdlib delta initialize failed: "
                + cajeta::jittest::toString(std::move(err)));
        for (const std::string& name : clinitNames)
            if (auto fn = resolveStdlib<void (*)()>(name.c_str())) stdlibClinits.push_back(fn);
        for (auto& [name, size] : staticNames)
            if (auto* addr = resolveStdlib<void*>(name.c_str()))
                stdlibStatics.emplace_back(addr, std::string((const char*) addr, size));
        if (std::getenv("CAJETA_PRIME_TIMING"))
            std::fprintf(stderr, "[shared-stdlib] delta: %zu definitions, %zu ctors, %zu static fields\n",
                         defs, newCtors.size(), staticNames.size());
    }

    template <typename Fn>
    Fn resolveStdlib(const char* name) {
        auto sym = sharedJit->lookup(*sharedStdlibJD, name);
        if (!sym) {
            cajeta::jittest::consumeError(sym.takeError());
            return nullptr;
        }
        return reinterpret_cast<Fn>(sym->getValue());
    }

    // Exposes the clone's `__cajeta_clinit_*` ctors to lookup and returns their
    // names in ctor order; they are private in the compiler's module.
    static std::vector<std::string> exportClinits(llvm::Module& m) {
        std::vector<std::string> names;
        auto* ctors = m.getGlobalVariable("llvm.global_ctors");
        if (!ctors || !ctors->hasInitializer()) return names;
        auto* list = llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
        if (!list) return names;
        for (unsigned i = 0; i < list->getNumOperands(); ++i) {
            auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(list->getOperand(i));
            if (!entry || entry->getNumOperands() < 2) continue;
            auto* fn = llvm::dyn_cast<llvm::Function>(
                entry->getOperand(1)->stripPointerCasts());
            if (!fn || !fn->getName().starts_with("__cajeta_clinit_")) continue;
            fn->setLinkage(llvm::GlobalValue::ExternalLinkage);
            names.push_back(fn->getName().str());
        }
        return names;
    }

    // Bind host process symbols and the MinGW CRT entry points onto `jd` so
    // JIT'd stdlib and user code resolves libc and the statically-linked
    // runtime. (The TLS engine left with cajeta-crypto spec 6.3.)
    void installHostSymbols(llvm::orc::LLJIT& jit, llvm::orc::JITDylib& jd) {
        auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit.getDataLayout().getGlobalPrefix());
        if (!gen) throw std::runtime_error("LLJIT process-symbol generator failed");
        jd.addGenerator(std::move(*gen));
        auto& ES = jit.getExecutionSession();
#ifdef _WIN32
        {
            size_t winSymCount = 0;
            const CajetaJitWinSym* winSyms = cajeta_jit_win_symbols(&winSymCount);
            llvm::orc::SymbolMap winSymMap;
            for (size_t i = 0; i < winSymCount; ++i) {
                winSymMap[ES.intern(winSyms[i].name)] =
                    llvm::orc::ExecutorSymbolDef(
                        llvm::orc::ExecutorAddr::fromPtr(winSyms[i].addr),
                        llvm::JITSymbolFlags::Exported);
            }
            cajeta::jittest::cantFail(
                jd.define(llvm::orc::absoluteSymbols(std::move(winSymMap))));
        }
#endif
    }

    void ensureSharedStdlibDylib() {
        if (sharedJitReady) return;
        ensurePrimed();
        auto materializeStart = std::chrono::steady_clock::now();
        auto builder = cajeta::test::coffSafeJitBuilder();
        std::string cacheRoot = primeCacheRoot();
        if (!cacheRoot.empty()) {
            objectCache = std::make_unique<StdlibObjectCache>(cacheRoot);
            StdlibObjectCache* cache = objectCache.get();
            builder.setCompileFunctionCreator(
                [cache](llvm::orc::JITTargetMachineBuilder jtmb)
                    -> llvm::Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
                    cache->setTarget(jtmb.getTargetTriple().str() + "|" + jtmb.getCPU() + "|"
                                     + jtmb.getFeatures().getString() + "|" LLVM_VERSION_STRING);
                    auto tm = jtmb.createTargetMachine();
                    if (!tm) return tm.takeError();
                    return std::make_unique<llvm::orc::TMOwningSimpleCompiler>(std::move(*tm), cache);
                });
        }
        auto jitOrErr = builder.create();
        if (!jitOrErr) throw std::runtime_error("shared LLJIT create failed");
        sharedJit = std::move(*jitOrErr);
        cajeta::jit::installObjectDump(*sharedJit);
        if (std::getenv("CAJETA_JIT_GDB")) {
            if (auto err = llvm::orc::enableDebuggerSupport(*sharedJit)) {
                llvm::errs() << "[jit-gdb] enableDebuggerSupport failed: "
                             << cajeta::jittest::toString(std::move(err)) << "\n";
            } else {
                llvm::errs() << "[jit-gdb] GDB JIT symbolization enabled\n";
            }
        }
        auto& mainJD = sharedJit->getMainJITDylib();
        installHostSymbols(*sharedJit, mainJD);

        auto jdOrErr = sharedJit->createJITDylib("CajetaStdlib");
        if (!jdOrErr) throw std::runtime_error("shared StdlibJD create failed");
        sharedStdlibJD = &*jdOrErr;
        // An explicit order, self then main, not addToLinkOrder: LLJIT seeds a
        // new dylib's order with its platform and process-symbols dylibs, and
        // appending main behind them let the stdlib's own CRT imports resolve
        // through the process generator first. On COFF that is ucrtbase's
        // malloc/free, a second CRT instance beside the bridged host table on
        // main, and every JIT'd free faulted in RtlFreeHeap (114 of 901 release
        // tests on the v0.42.0 Windows dry-run). Main keeps the generator for
        // libc, behind its own bridged definitions.
        sharedStdlibJD->setLinkOrder(
            { { &mainJD, llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly } });

        // Materialize the stdlib ONCE: clone its IR, bitcode-roundtrip into a
        // fresh long-lived context (O0 alwaysinline like the per-test path),
        // add to the stdlib dylib, run its global ctors once.
        llvm::Module* srcMod = stdlibModule->getLlvmModule();
        noteDefinitions(*srcMod);
        auto clone = llvm::CloneModule(*srcMod);
        std::vector<std::string> clinitNames = exportClinits(*clone);
        auto staticNames = collectStatics(*clone, sharedJit->getDataLayout());
        llvm::SmallVector<char, 0> bc;
        { llvm::raw_svector_ostream os(bc); llvm::WriteBitcodeToFile(*clone, os); }
        std::string digest = cajeta::buildtool::sha256Hex(std::string(bc.data(), bc.size()));
        if (digest.rfind("sha256:", 0) == 0) digest.erase(0, 7);
        auto memBuf = llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef(bc.data(), bc.size()), StdlibObjectCache::kPrefix + digest);
        auto tsCtx = std::make_unique<llvm::LLVMContext>();
        llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
#if LLVM_VERSION_MAJOR >= 21
        auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
            return llvm::parseBitcodeFile(memBuf->getMemBufferRef(), *ctx); });
#else
        auto parsed = llvm::parseBitcodeFile(memBuf->getMemBufferRef(),
                                             *tsContext.getContext());
#endif
        if (!parsed)
            throw std::runtime_error("shared stdlib bitcode reparse failed: "
                + cajeta::jittest::toString(parsed.takeError()));
        cajeta::optimizeModule(**parsed, nullptr, cajeta::OptLevel::O0);
        (*parsed)->setModuleIdentifier(StdlibObjectCache::kPrefix + digest);
        llvm::orc::ThreadSafeModule tsm(std::move(*parsed), std::move(tsContext));
        if (auto err = sharedJit->addIRModule(*sharedStdlibJD, std::move(tsm)))
            throw std::runtime_error("shared stdlib addIRModule failed: "
                + cajeta::jittest::toString(std::move(err)));
        if (auto err = sharedJit->initialize(*sharedStdlibJD))
            throw std::runtime_error("shared stdlib initialize failed: "
                + cajeta::jittest::toString(std::move(err)));
        if (std::getenv("CAJETA_PRIME_TIMING")) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - materializeStart).count();
            std::fprintf(stderr, "[shared-stdlib] object %s digest=%.12s materialize=%lldms\n",
                         !objectCache ? "UNCACHED" : objectCache->served() ? "HIT" : "MISS",
                         digest.c_str(), (long long) ms);
        }
        for (const std::string& name : clinitNames) {
            if (auto fn = resolveStdlib<void (*)()>(name.c_str())) stdlibClinits.push_back(fn);
        }
        for (auto& [name, size] : staticNames) {
            if (auto* addr = resolveStdlib<void*>(name.c_str()))
                stdlibStatics.emplace_back(addr, std::string((const char*) addr, size));
        }
        if (std::getenv("CAJETA_PRIME_TIMING"))
            std::fprintf(stderr, "[shared-stdlib] %zu clinits, %zu static fields\n",
                         stdlibClinits.size(), stdlibStatics.size());
        epochPush = resolveStdlib<int32_t (*)()>("__cajeta_rt_epoch_push");
        epochPopTo = resolveStdlib<void (*)(int32_t)>("__cajeta_rt_epoch_pop_to");
        if (!epochPush || !epochPopTo)
            throw std::runtime_error("shared stdlib lacks the runtime epoch entry points");
        sharedJitReady = true;
        ++sharedMaterializations;
    }
};

} // namespace

CajetaJit::CajetaJit() = default;

// A process-wide net under the per-JIT temp-root cleanup. Most tests hold their
// CajetaJit in a local and ~CajetaJit reclaims the tree; a few hold one in a
// function-local static for the compile cost, and MEASURED 2026-08-22 those
// destructors do not run before the process ends — a clean ProfilerClock +
// ProfilerIntegrity run left one pair each behind. Bounded (one per singleton,
// not one per test) but not zero, and "not zero" is what accumulates across
// runs. Anything still registered at exit is removed here.
//
// A killed process still leaks, and nothing destructor-based can change that.
// The claim this makes good on is narrower and worth stating exactly: a NORMAL
// exit leaves nothing behind.
namespace {
class TempRootRegistry {
public:
    static TempRootRegistry& instance() {
        static TempRootRegistry r;
        return r;
    }
    void add(const std::string& root) {
        std::lock_guard<std::mutex> lk(mu_);
        roots_.push_back(root);
    }
    void drop(const std::string& root) {
        std::lock_guard<std::mutex> lk(mu_);
        roots_.erase(std::remove(roots_.begin(), roots_.end(), root), roots_.end());
    }
    void sweep() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!std::getenv("CAJETA_KEEP_TEMP")) {
            for (const auto& r : roots_) {
                std::error_code ec;
                std::filesystem::remove_all(r, ec);
            }
        }
        roots_.clear();
    }

private:
    // No atexit registration: test/main.cpp terminates with _Exit to dodge a
    // two-LLVM ManagedStatic teardown crash, so the atexit chain never runs.
    // sweepTempRoots() is called from each exit point instead.
    TempRootRegistry() = default;
    std::mutex mu_;
    std::vector<std::string> roots_;
};
} // namespace

void sweepTempRoots() { TempRootRegistry::instance().sweep(); }

size_t CajetaJit::sharedStdlibMaterializations() {
    return StdlibReuseCache::instance().sharedMaterializations;
}

size_t CajetaJit::sharedStdlibLinkOrderSize() {
    auto& cache = StdlibReuseCache::instance();
    if (!cache.sharedJitReady || !cache.sharedStdlibJD) return 0;
    size_t n = 0;
    cache.sharedStdlibJD->withLinkOrderDo(
        [&](const llvm::orc::JITDylibSearchOrder& so) { n = so.size(); });
    return n;
}

void* CajetaJit::sharedStdlibResolves(const char* name) {
    auto& cache = StdlibReuseCache::instance();
    if (!cache.sharedJitReady || !cache.sharedStdlibJD) return nullptr;
    llvm::orc::JITDylibSearchOrder order;
    cache.sharedStdlibJD->withLinkOrderDo(
        [&](const llvm::orc::JITDylibSearchOrder& so) { order = so; });
    auto sym = cache.sharedJit->getExecutionSession().lookup(
        order, cache.sharedJit->mangleAndIntern(name));
    if (!sym) { llvm::consumeError(sym.takeError()); return nullptr; }
    return (void*) sym->getAddress().getValue();
}

size_t CajetaJit::sharedStdlibObjectsServed() {
    auto& cache = StdlibReuseCache::instance().objectCache;
    return cache ? cache->served() : 0;
}

size_t CajetaJit::sharedStdlibObjectsCompiled() {
    auto& cache = StdlibReuseCache::instance().objectCache;
    return cache ? cache->compiled() : 0;
}

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
    if (jit || sharedJit) {
        auto sym = resolve("__cajeta_task_shutdown");
        if (sym) {
            auto fn = reinterpret_cast<void(*)()>(sym->getAddress().getValue());
            if (fn) fn();
        } else {
            cajeta::jittest::consumeError(sym.takeError());
        }
    }
    // Shared-dylib mode: drop what this test's ctors registered, then remove
    // its user dylib so JDs don't accumulate across the suite. The borrowed
    // sharedJit + shared stdlib dylib persist.
    if (sharedJit) StdlibReuseCache::instance().endExecution(epochDepth);
    if (sharedJit && userDylib) {
        if (auto err = sharedJit->getExecutionSession().removeJITDylib(*userDylib))
            cajeta::jittest::consumeError(std::move(err));
    }
    // The multi-source path's temp source/archive roots. Removed LAST, after
    // the module is done with them. Nothing removed these before: a full suite
    // run left ~7,800 dirs behind and they accumulated across runs until the
    // per-user tmpfs quota broke unrelated builds. See JitTempDirCleanupTests.
    for (const auto& root : tempRoots) {
        if (!std::getenv("CAJETA_KEEP_TEMP")) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
        TempRootRegistry::instance().drop(root);
    }
}

// 5.1.11 / 5.2.8 — diagnostics collected by the last compile (see the header).
// thread_local: concurrent-compile tests run CajetaJit::compile from several
// threads at once, and a shared vector's clear/assign races are a double
// free. Assertions read from the thread that compiled, so per-thread
// storage preserves the API.
namespace {
    thread_local std::vector<cajeta::CollectedDiagnostic> g_lastDiagnostics;
}

const std::vector<cajeta::CollectedDiagnostic>& CajetaJit::lastDiagnostics() {
    return g_lastDiagnostics;
}

bool CajetaJit::sawDiagnostic(const std::string& code) {
    for (auto& d : g_lastDiagnostics) {
        if (d.code == code) return true;
    }
    return false;
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

    // 5.1.11 / 5.2.8 — install a DiagnosticEngine for this compile so WARNINGS
    // (which are reported, not thrown) are captured instead of dropped on the
    // floor. Parked in g_lastDiagnostics for the test to assert on. The guard
    // restores the previous active engine even if the compile throws, so an
    // error-path test doesn't leave a dangling cursor into a dead stack frame.
    cajeta::DiagnosticEngine diagEngine;
    // Warnings-capture ONLY: errors keep throwing (fail-loud tests depend on
    // it; a collected error would let codegen run into a null type and crash).
    diagEngine.setCollectErrors(false);
    cajeta::DiagnosticEngine* prevEngine = cajeta::DiagnosticEngine::active();
    // A test that installed its OWN engine (EngineScope) keeps it — the
    // harness engine must not shadow it, or the test's asserted diagnostics
    // land in the wrong collector (SliceLint et al.).
    if (!prevEngine) {
        cajeta::DiagnosticEngine::setActive(&diagEngine);
    }
    g_lastDiagnostics.clear();
    struct DiagGuard {
        cajeta::DiagnosticEngine& engine;
        cajeta::DiagnosticEngine* prev;
        ~DiagGuard() {
            if (!prev) {
                g_lastDiagnostics = engine.finalize();
                cajeta::DiagnosticEngine::setActive(prev);
            }
        }
    } diagGuard{diagEngine, prevEngine};

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
    // CAJETA_STDLIB_REUSE is read per compile: the harness exports it for the
    // sweep, a bare run leaves it unset, and a test may set it for itself.
    bool reuseStdlib = std::getenv("CAJETA_STDLIB_REUSE") != nullptr
                    && stdlibReusable(opts);
    // The reuse cache (one shared LLVMContext + a process-global captured
    // baseline mutated by restoreBaseline) is single-threaded by construction;
    // concurrent compiles racing on it corrupt the heap (ConcurrentCompileTests
    // SIGABRT under reuse). Confine reuse to the thread that first primed it —
    // off-owner-thread compiles take the fresh, fully-isolated path (the same
    // one default mode uses, which those tests pass on). #14 (per-thread
    // context) removes the restriction.
    if (reuseStdlib) {
        static std::mutex reuseOwnerMutex;
        static std::thread::id reuseOwnerThread;
        static bool reuseOwnerSet = false;
        std::lock_guard<std::mutex> lk(reuseOwnerMutex);
        if (!reuseOwnerSet) {
            reuseOwnerThread = std::this_thread::get_id();
            reuseOwnerSet = true;
        } else if (std::this_thread::get_id() != reuseOwnerThread) {
            reuseStdlib = false;
        }
    }
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

    static thread_local std::mt19937_64 rng(std::random_device{}());  // per-thread (U5)
    auto sourceRoot = std::filesystem::temp_directory_path()
                    / ("cajeta_multi_" + std::to_string(rng()));
    std::filesystem::create_directories(sourceRoot);

    // Own the temp roots from the moment they exist, so a throw anywhere below
    // (a CajetaException surfaced out of compile(), a failed JIT) still takes
    // them with it. On the success path ownership TRANSFERS to the returned
    // CajetaJit (see the release just before `return jitState`), whose
    // destructor removes them once the module is done.
    struct TempRootGuard {
        std::vector<std::filesystem::path> roots;
        bool armed = true;
        ~TempRootGuard() {
            if (!armed) return;
            for (const auto& r : roots) {
                if (!std::getenv("CAJETA_KEEP_TEMP")) {
                    std::error_code ec;
                    std::filesystem::remove_all(r, ec);
                }
                TempRootRegistry::instance().drop(r.string());
            }
        }
    } tempRootGuard;
    tempRootGuard.roots.push_back(sourceRoot);
    TempRootRegistry::instance().add(sourceRoot.string());

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
    tempRootGuard.roots.push_back(archiveRoot);
    TempRootRegistry::instance().add(archiveRoot.string());

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
            stdlibCache.ensureSharedStdlibDylib();
            stdlibCache.restoreBaseline();
            cajeta::Compiler::setSharedContext(
                cajeta::StdlibReuseCore::instance().context());
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
        if (!opts.cpu.empty()) {
            compiler->setCpu(opts.cpu);
        }
        if (!opts.features.empty()) {
            compiler->setFeatures(opts.features);
        }
        compiler->getMutableFlags().lineInfo = opts.lineInfoEnabled;
        compiler->getMutableFlags().stackTraceCapture = opts.stackTraceCaptureEnabled;
        compiler->getMutableFlags().profiler = opts.profiler;
        compiler->getMutableFlags().profilerSelect = opts.profilerSelect;
        compiler->getMutableFlags().withCalibration = opts.withCalibration;
        if (opts.session || !opts.sessionHostName.empty()) {
            compiler->setSessionState(opts.session, opts.sessionHostName);
        }
        if (opts.debugInfoEnabled) {
            compiler->getMutableFlags().debugInfo = true;
            compiler->getMutableFlags().debugInfoLevel = cajeta::DebugInfo::Full;
            // Ids are dense and sequential from 0 per compile; a previous test's
            // entries would shift this one's.
            cajeta::dbg::globalDbgLocTable().clear();
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
    // jit-drop-backfill parity (spec §2/§3, the buildJit pipeline's exact
    // pair): synthesize any drop thunks left as bare extern declarations
    // (a consumer codegen'd before the owner's module existed), then
    // promote every drop-thunk definition off linkonce_odr so the
    // in-process linkModules merge below can't lazy-discard one that no
    // already-linked module references yet. Without the pin, a multi-file
    // test whose owner module links after its consumer loses the thunk —
    // "Symbols not found: __cajeta_<type>_drop" at LLJIT materialization
    // (PlaceholderOwnedFieldTests pins the shape).
    {
        std::vector<cajeta::CajetaModulePtr> scanModules(modulesList.begin(),
                                                         modulesList.end());
        if (reuseStdlib && stdlibCache.stdlibModule) {
            scanModules.push_back(stdlibCache.stdlibModule);
        }
        cajeta::backfillDropFunctions(scanModules, scanModules);
        cajeta::pinDropFunctionDefinitions(scanModules);
    }
    for (auto& m : modulesList) {
        if (m == primary) continue;
        std::unique_ptr<llvm::Module> donor(m->getLlvmModule());
        if (llvm::Linker::linkModules(*primary->getLlvmModule(),
                                       std::move(donor))) {
            throw std::runtime_error("JIT module-merge failed");
        }
    }
    // Under reuse the stdlib is not in this Compiler's module list: user code
    // resolves it cross-dylib from the persistent CajetaStdlib JITDylib (U7).
    const bool useSharedDylib = reuseStdlib;
    if (useSharedDylib) stdlibCache.materializeStdlibDelta();
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
            {
                auto all = compiler->getModules();
                cajeta::xpu::scanLaunchBlocks({all.begin(), all.end()});
            }
            // Sweep unless the test gates (Options::xpuKernelGateErrors); the
            // override is restored whatever the registration does.
            struct GateScope {
                explicit GateScope(bool gate) { cajeta::xpu::setKernelGateWarns(!gate); }
                ~GateScope() { cajeta::xpu::clearKernelGateWarnsOverride(); }
            } gateScope(opts.xpuKernelGateErrors);
            cajeta::xpu::resetKernelGate();
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
            if (opts.xpuKernelGateErrors) cajeta::xpu::throwIfKernelGateFailed();
        }
        if (!opts.withCalibration.empty()) {
            std::vector<cajeta::xpu::CalibrationRecordBlob> records;
            std::string calError;
            if (!cajeta::xpu::parseCalibrationSnapshot(opts.withCalibration, records, calError)) {
                throw std::runtime_error("withCalibration: " + calError);
            }
            cajeta::xpu::emitCalibrationRegistration(records, *primary->getLlvmModule());
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
        // Under reuse the stdlib is not in this module, so append its IR as
        // the shared dylib materialized it: the capture keeps reading as the
        // whole program after the same O0 inlining.
        if (useSharedDylib) {
            auto stdlibClone = llvm::CloneModule(*stdlibCache.stdlibModule->getLlvmModule());
            cajeta::optimizeModule(*stdlibClone, nullptr, cajeta::OptLevel::O0);
            stdlibClone->print(irStream, nullptr);
        }
        irStream.flush();
    }
    llvm::orc::ThreadSafeModule tsModule(std::move(*parsed), std::move(tsContext));

    if (useSharedDylib) {
        // Borrow the process-wide shared LLJIT; this test's code lives in a
        // fresh user dylib that links the shared CajetaStdlib JITDylib (+ host
        // symbols on the shared main dylib). No per-test LLJIT, no stdlib clone.
        stdlibCache.ensureSharedStdlibDylib();
        jitState->sharedJit = stdlibCache.sharedJit.get();
        static std::atomic<uint64_t> userJdCounter{0};
        std::string jdName =
            "cajeta.user." + std::to_string(userJdCounter.fetch_add(1));
        auto jdOrErr = jitState->sharedJit->createJITDylib(jdName);
        if (!jdOrErr) {
            throw std::runtime_error("user JITDylib create failed: "
                + cajeta::jittest::toString(jdOrErr.takeError()));
        }
        jitState->userDylib = &*jdOrErr;
        // U7c cross-dylib EH fix: force the SHARED STDLIB ahead of the main
        // dylib in the link order. createJITDylib seeds the link order with the
        // process-symbol main dylib first, so a user `try`'s __cajeta_exc_push
        // resolved to the NATIVE runtime while the stdlib `throw` (intra-StdlibJD)
        // used the JIT runtime — two different __cajeta_main_exc_top TLS slots →
        // uncaught. setLinkOrder (self, StdlibJD, main) makes ALL runtime symbols
        // in user code resolve to the SAME JIT copy the stdlib uses. Native libc
        // still resolves via main (StdlibJD/user don't define it).
        jitState->userDylib->setLinkOrder(
            { { stdlibCache.sharedStdlibJD,
                llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly },
              { &jitState->sharedJit->getMainJITDylib(),
                llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly } },
            /*LinkAgainstThisJITDylibFirst=*/true);
        if (auto err = jitState->sharedJit->addIRModule(*jitState->userDylib,
                                                        std::move(tsModule))) {
            throw std::runtime_error("user addIRModule failed: "
                + cajeta::jittest::toString(std::move(err)));
        }
        mark("shared-dylib user JD + addIRModule");
    } else {
    auto jitOrErr = cajeta::test::makeCoffSafeJit();
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

    stdlibCache.installHostSymbols(*jitState->jit, jitState->jit->getMainJITDylib());
    }  // end fresh-LLJIT setup

    // The active JIT + dylib for the rest of the flow. Shared mode targets this
    // test's user dylib (links the shared stdlib); default mode targets the
    // per-test LLJIT's main dylib. lookup(JD, name) with JD==Main is identical
    // to lookup(name), so the default path is behaviour-preserving.
    llvm::orc::LLJIT* activeJit =
        useSharedDylib ? jitState->sharedJit : jitState->jit.get();
    llvm::orc::JITDylib& activeDylib =
        useSharedDylib ? *jitState->userDylib : activeJit->getMainJITDylib();

    // Diagnostic (CAJETA_PRIME_TIMING): where the allocator family resolves
    // from each dylib, beside the host's own addresses. On COFF a name that
    // resolves outside the bridged host table lands in a second CRT instance.
    if (std::getenv("CAJETA_PRIME_TIMING")) {
        // Resolve through the dylib's LINK ORDER, as its modules' imports do;
        // a plain lookup(JD, name) searches that one dylib only.
        auto show = [&](const char* tag, llvm::orc::LLJIT& j, llvm::orc::JITDylib& d) {
            auto& es = j.getExecutionSession();
            llvm::orc::JITDylibSearchOrder order;
            d.withLinkOrderDo([&](const llvm::orc::JITDylibSearchOrder& so) { order = so; });
            for (const char* name : {"free", "malloc"}) {
                auto sym = es.lookup(order, j.mangleAndIntern(name));
                if (sym) {
                    std::fprintf(stderr, "[sym] %s %s = %p (order of %zu)\n", tag, name,
                                 (void*) sym->getAddress().getValue(), order.size());
                } else {
                    std::fprintf(stderr, "[sym] %s %s = <%s>\n", tag, name,
                                 cajeta::jittest::toString(sym.takeError()).c_str());
                }
            }
        };
        std::fprintf(stderr, "[sym] host free = %p malloc = %p\n",
                     (void*) &std::free, (void*) &std::malloc);
        show("active", *activeJit, activeDylib);
        if (useSharedDylib) {
            show("stdlibJD", *activeJit, *stdlibCache.sharedStdlibJD);
            show("sharedMain", *activeJit, activeJit->getMainJITDylib());
        }
    }

    // Run any global ctors / static initializers (P6.2 clinit, etc.)
    // before handing control to test code. LLJIT does NOT run
    // llvm.global_ctors automatically — the host runtime's
    // `__attribute__((constructor))` functions (__cajeta_runtime_init,
    // __cajeta_hash_seed_init) only fire because the runtime is also
    // statically linked into the test binary. Anything that lives ONLY
    // in the JIT module (per-class clinit emitted by
    // CajetaClass::generateStaticInitializers) needs this initialize
    // call to execute its initializer. (Shared mode: stdlib ctors already ran
    // once at StdlibJD init; this runs only THIS user dylib's ctors.)
    if (useSharedDylib) jitState->epochDepth = stdlibCache.beginExecution();
    if (auto err = activeJit->initialize(activeDylib)) {
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
    if (auto sym = jitState->resolve("__cajeta_set_poison_free")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getAddress().getValue());
        if (setFn) setFn(desiredPoison);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_poison_free(desiredPoison);

    int desiredValidate = opts.dropChainValidateEnabled ? 1 : 0;
    if (auto sym = jitState->resolve("__cajeta_set_drop_chain_validate")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getAddress().getValue());
        if (setFn) setFn(desiredValidate);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_drop_chain_validate(desiredValidate);

    // stackTraceCapture is NOT set here: the compiler emits a module global
    // ctor from CompilerFlags, which jit->initialize() above already ran.
    // Poking the setter here would mask a regression in that codegen.

    // Reuse mode: release this test's user struct-type names from the shared
    // context so the next test can re-declare same-named classes (`test.S`)
    // without colliding with this test's now-stale, already-bodied structs.
    // The JIT module is a separate bitcode-roundtripped context, so the live
    // test is unaffected. (llvmModule is primary's compile-context module.)
    if (reuseStdlib) {
        stdlibCache.clearTransientStructNames(llvmModule);
    }

    // Hand the temp roots to the JIT: they outlive this call, but not the
    // module that was built from them.
    for (const auto& r : tempRootGuard.roots) jitState->tempRoots.push_back(r.string());
    tempRootGuard.armed = false;

    return jitState;
}

llvm::Expected<llvm::orc::ExecutorSymbolDef> CajetaJit::resolve(const std::string& name) {
    if (!sharedJit) {
        auto addr = jit->lookup(name);
        if (!addr) return addr.takeError();
        return llvm::orc::ExecutorSymbolDef(*addr, llvm::JITSymbolFlags());
    }
    llvm::orc::JITDylibSearchOrder order{
        { userDylib, llvm::orc::JITDylibLookupFlags::MatchAllSymbols },
        { StdlibReuseCache::instance().sharedStdlibJD,
          llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly },
        { &sharedJit->getMainJITDylib(),
          llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly } };
    return sharedJit->getExecutionSession().lookup(order, sharedJit->mangleAndIntern(name));
}

void* CajetaJit::lookupAddress(const std::string& shortName) {
    auto it = nameMap.find(shortName);
    if (it == nameMap.end()) return nullptr;
    auto sym = resolve(it->second);
    if (!sym) {
        cajeta::jittest::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getAddress().getValue());
}

void* CajetaJit::lookupRawSymbol(const std::string& exactName) {
    auto sym = resolve(exactName);
    if (!sym) {
        cajeta::jittest::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getAddress().getValue());
}

} // namespace cajeta_test
