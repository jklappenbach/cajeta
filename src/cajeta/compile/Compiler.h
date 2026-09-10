//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include <iostream>
#include <mutex>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/TargetRegistry.h>
#include <filesystem>
#include <utility>
#include "antlr4-runtime/antlr4-runtime.h"
#include "../type/CajetaType.h"
#include "CajetaModule.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"
#include "CompilerMode.h"
#include "CompilationContext.h"
#include "CacheManifest.h"
#include <optional>
#include <functional>
#include <list>
#include <string>
#include <set>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "../error/Exception.h"

namespace llvm { class Module; class GlobalValue; }

using namespace std;
namespace cajeta {
    class CajetaModule;

    class AbstractSyntaxNode;

    // Control signal thrown when a novel stdlib-template instantiation would emit into a
    // per-test user module. NOT a cajeta::Exception, so catch(Exception&) lets it through.
    struct ReuseHazardAbort {};

    // Register every class declared under `rootPath` in the archive BEFORE any module
    // parses, so cross-file forward references get placeholders. suppressConsole drops
    // ANTLR's console listener; the authoritative parse re-reports what it hides.
    void prescanSourceRoot(const std::string& rootPath, bool suppressConsole = false);

    // Every `.cajeta` file under `rootPath`, SORTED: parse order decides synthesized names
    // and archive keys, so it must not follow enumeration order. Caller owns the list.
    std::list<std::string>* listModulePaths(std::string rootPath);

    // Emit a per-module global ctor registering UnrecoverableException's vtable with the
    // runtime. Must run AFTER CajetaModule::buildPendingPrototypes lays it out. Idempotent.
    void emitUnrecoverableMarker(CajetaModulePtr module);

    enum class EmitMode {
        IR,       // Default: exploded text LLVM IR (.ll) per module
        Obj,      // Exploded native object files (.o) per module
        Cja,      // Single .cja archive — project-only IR (no stdlib, no deps)
        Uber,     // Single .cja archive — project + stdlib + transitively-referenced deps under deps/<name>-<ver>/
        Exe,      // Linked executable (requires lld in-process; see D1 / Compiler.cpp)
    };

    // Device backend for the AOT path. None leaves @Kernel methods as host stubs; any other
    // value embeds the device image + a registration ctor into the host module.
    enum class XpuBackend {
        None,     // Default: no device codegen.
        Nvptx,    // NVIDIA: AST → device IR → PTX → ptxas → cubin, registered in-module.
        Amdgpu,   // AMD: AST → device IR → AMDGCN ISA → lld → hsaco, registered in-module.
        Vulkan,   // Vulkan: AST → device IR → SPIR-V (descriptor-set SSBOs), registered in-module.
        Cpu,      // CPU: AST → host IR (grid→threads), linked into the module + registered.
    };

    // What device artifact, if any, to also drop to disk alongside the --emit output.
    enum class XpuEmit {
        None,     // Default: registration only, no standalone artifact.
        Ptx,      // NVPTX: write a per-kernel .ptx next to the module output.
        Cubin,    // NVPTX: write a per-kernel .cubin (implies ptxas present).
        Isa,      // AMDGPU: write a per-kernel .isa (AMDGCN assembly text).
        Hsaco,    // AMDGPU: write a per-kernel .hsaco (implies ld.lld present).
        Spirv,    // Vulkan: write a per-kernel .spv (Khronos SPIR-V binary).
        Spvasm,   // Vulkan: write a per-kernel .spvasm (SPIR-V disassembly text).
        Object,   // CPU: write a per-kernel .o (native relocatable object).
    };

    class SessionState;

    // Parse a SYNTHESIZED compilation unit (template instantiation and friends) through
    // the same two-stage SLL/LL path real sources take; `what` names it in diagnostics.
    antlr4::tree::ParseTree* parseSyntheticCompilationUnit(
        CajetaParser& parser, antlr4::CommonTokenStream& tokens,
        const std::string& what);

    class Compiler {
    private:
        string targetTriple;
        const llvm::Target* target;
        llvm::LLVMContext llvmContext;
        // The context this Compiler's modules bind to: the owned one, or s_sharedContext.
        llvm::LLVMContext* activeContext = &llvmContext;
        // Per-thread mutable state for this compile; the guard makes it the calling thread's
        // current context. Declared before the ctor body, so init already sees it current.
        CompilationContext compilationContext;
        ScopedCompilationContext compilationContextScope{&compilationContext};
        // Process-global shared context for the test stdlib-reuse path: Compilers built under
        // it SKIP the global reset/init. thread_local, so other threads take the fresh path.
        static thread_local llvm::LLVMContext* s_sharedContext;
        // One-time guard: the FIRST Compiler under a shared context primes the type tables.
        static thread_local bool s_sharedInitialized;
        // Armed only during a JIT-harness stdlib-reuse attempt; see setReuseHazardArmed.
        static thread_local bool s_reuseHazardArmed;
        // Default target CPU; `native` is the host cpu name plus its detected features. A
        // .cja carries bitcode, so the cpu binds only at final --emit=exe/obj.
        string cpu = "native";
        string features = "";
        llvm::TargetMachine* targetMachine;
        llvm::TargetOptions opt;
        std::optional<llvm::Reloc::Model> RM;
        list<CajetaModulePtr> modules;
        // Files materializeUserClass force-compiled on demand; compile(module) skips them so
        // the driver loop does not redeclare their classes. NOT a general once-per-path mark.
        std::set<std::string> materializedSourcePaths;
        // materializeUserClass recursion guard (record A ↔ record B cycles).
        std::set<std::string> materializeInFlight;
        // Roots from the most recent createModule, reused for on-demand sibling modules.
        string lastSourceRoot;
        string lastTargetRoot;
        // Compiler mode + per-feature toggles: a flavor flag sets `mode` and resets `flags`
        // to its defaults, per-feature flags override fields after. Forwarded to each module.
        CompilerMode mode = CompilerMode::Debug;
        CompilerFlags flags = CompilerFlags::defaultsForMode(CompilerMode::Debug);
        // Host-owned; not freed here. See setSessionState.
        SessionState* sessionState = nullptr;
        string sessionHostName;
        // Output mode: default IR writes .ll, --emit=obj/exe lowers native objects.
        EmitMode emitMode = EmitMode::IR;
        // Selected device backends (empty = None); a list so one binary can bundle several
        // targets. xpuArch is the arch handed to each backend's TargetMachine + assembler.
        vector<XpuBackend> xpuBackends;
        XpuEmit xpuEmit = XpuEmit::None;
        string xpuArch = "sm_89";
        // Kernel manifests, as (archive member name, JSON text); emitArchive writes each one
        // beside the class bitcode it describes.
        vector<std::pair<string, string>> xpuManifestMembers;
        // Output override for single-file builds; empty mirrors the source tree instead.
        string outputPath;

        // Entry method (`pkg.Class.method`), set before parsing; read by emitCMainShim.
        string entryMethod;

        // Source root (trailing-slash normalized) so emitArchive can find `skills/`.
        string skillSourceRoot;

        // Override for where `skills/` lives: the PROJECT root, not the compile source root.
        string skillRootOverride;

        // Collected .o paths from Obj/Exe emissions, fed to the linker for Exe mode.
        std::vector<string> objectFiles;

        // The build tool's clean/dirty designation; an empty optional means full rebuild.
        string cacheManifestPath;
        std::optional<CacheManifest> cacheManifest;

        // Load + validate the manifest, force the v1 interaction guards (treeShake=Off,
        // linkMode=Full), check the discriminator. False only on a malformed manifest.
        bool setupCacheManifest();

        // --classpath archives, consumed twice: ingestClasspath() re-parses their ClassSource
        // entries, and emitArchive(uber) bundles their bitcode under `deps/<name>-<version>/`.
        std::vector<string> classpath;

        // Classpath-archive modules: registered canonically, but NEVER emitted or linked.
        std::list<CajetaModulePtr> externalModules;
        // --emit=uber default: prune classpath entries to the referenced closure.
        bool pruneUber = true;

        // (Re)build the TargetMachine for the current triple/cpu/features.
        void rebuildTargetMachine();

        // Per-module emit dispatch based on emitMode.
        void emitForModule(CajetaModulePtr module);

        // Device codegen for the AOT path: embed every @Kernel's image + registration ctor in
        // its host module, dropping --xpu-emit artifacts under `archiveRootPath`. Never throws.
        void emitXpuKernels(const std::string& archiveRootPath);


        // Archive emit. Cja ships ONLY the project bitcode (a library archive); uber adds the
        // stdlib + every referenced dep under deps/. Output: `outputPath`, else cajeta.cja.
        void emitArchive(const std::string& archiveRootPath, bool uber);

        // Write the weak C stubs (optix, session install) for symbols only a JIT host defines.
        void writeAotStubs(const string& archiveRootPath);
        // Phase-2 link for --emit=exe: hands objectFiles to lld (when CAJETA_HAS_LLD).
        void linkExecutable(const string& archiveRootPath);

        // Emit a C-callable `main` invoking the user's static entry method (dotted form),
        // into the stdlib module so the link resolves it. Binary emit only; never throws.
        void emitCMainShim(const std::string& entryMethod);

        // Every module taking part in the final link — the reachability engine's input.
        void collectLinkModules(std::vector<llvm::Module*>& lmods);
        // BFS the by-name reference graph from the entry + ctor/ABI roots over `lmods` into
        // `defs`, conservatively. excludeClinitRoots drops __cajeta_clinit_*; rootAllLocals
        // also seeds every local symbol, required on COFF where locals are never erased.
        std::unordered_set<std::string> computeReachableSymbols(
            const std::vector<llvm::Module*>& lmods,
            std::unordered_map<std::string, llvm::GlobalValue*>& defs,
            bool excludeClinitRoots = false,
            bool rootAllLocals = false);

        // Phase A: print what Phase B would strip. Analysis only — mutates no IR.
        void reportTreeShake();
        // Phase B/C (--tree-shake=on): deleteBody() unreachable cajeta methods so the
        // linker never pulls them. Runs after the main shim + all ctors, before emit.
        void pruneUnreachable();
        // Tier-1.5 clinit-DCE: strip the static initializer of a class whose statics are
        // dead. Runs BEFORE pruneUnreachable so the cascade is picked up.
        void pruneDeadClinits();

    public:
        Compiler(int argc, const char* argv[]) : Compiler() { }

        // Registers the LLVM targets once per process, then binds the context: a shared one is
        // primed once, otherwise the global tables reset — cached types die with a context.
        Compiler() {
            static std::once_flag llvmTargetInitOnce;
            std::call_once(llvmTargetInitOnce, [] {
                llvm::InitializeAllTargets();
                llvm::InitializeAllTargetMCs();
                llvm::InitializeAllAsmPrinters();
                llvm::InitializeAllAsmParsers();
            });
            targetTriple = llvm::sys::getDefaultTargetTriple();
            if (s_sharedContext) {
                activeContext = s_sharedContext;
                rebuildTargetMachine();
                if (!s_sharedInitialized) {
                    CajetaType::resetGlobals();
                    CajetaModule::resetGlobals();
                    if (target) {
                        CajetaType::init(*activeContext);
                    }
                    s_sharedInitialized = true;
                }
            } else {
                CajetaType::resetGlobals();
                CajetaModule::resetGlobals();
                rebuildTargetMachine();
                if (target) {
                    CajetaType::init(*activeContext);
                }
            }
            compilationContext.llvmContext = activeContext;
        }

        ~Compiler() { }

        // Full drive of `sourceRootPath`; `entryMethod` is the static entry binary emit wraps.
        void compile(string entryMethod, string sourceRootPath, string archiveRootPath);

        // Resolve and codegen one already-created module.
        void compile(CajetaModulePtr module);

        // Compile the module declaring `canonical` on demand — the user-source analog of the
        // lazy stdlib drain. False when it has no source, is already compiled, or is in flight.
        bool materializeUserClass(const std::string& canonical);

        // Single-file diagnostics: stdlib + this file through the semantic/validation/DI
        // passes, then STOP before codegen. skipContextRegistration skips the sibling sweep
        // and classpath ingest; afterContextRegistration runs after one, before the parse.
        void lint(const string& file, const string& sourceRoot = "",
                  const string& shadow = "",
                  bool skipContextRegistration = false,
                  const std::function<void()>& afterContextRegistration = {});

        // Whole-root xref export: parse every .cajeta under `root`, continuing past broken
        // files, and write ONE xref document to flags.emitXref. Returns the parse failures.
        int lintRoot(const string& root);

        // Emit the linted file's xref records as NDJSON on stderr; `shadow`, when set, is the
        // ORIGINAL path a staged buffer stands in for. No-op unless flags.emitXref is "-".
        void emitLintXrefStream(const string& file, const string& sourceRoot,
                                const string& shadow);

        // The Compiler's stdlib module, parsed and prototype-built; created lazily, once.
        CajetaModulePtr ensureStdlibModule();

        // Re-parse every classpath archive's ClassSource entries into fresh modules, once,
        // after the stdlib parse. Public because the JIT host drives the phases by hand.
        void ingestClasspath();

        // Which stdlib packages are parsed (eager + on-demand) is process-global: the first
        // two are probes, resetLazyStdlibState clears the bookkeeping for a harness.
        static bool stdlibPackageParsed(const std::string& pkg);
        static const std::set<std::string>& stdlibParsedPackages();
        static void resetLazyStdlibState();

        // Opaque snapshot of the lazy-stdlib bookkeeping, so a restored context baseline
        // reinstates a sibling-triggered lazy parse instead of the eager floor.
        struct LazyStdlibState {
            std::set<std::string> parsedPackages;
            std::set<std::string> prescanned;
            std::set<std::string> parsed;
            std::vector<std::string> queue;
        };
        static LazyStdlibState captureLazyStdlibState();
        static void restoreLazyStdlibState(const LazyStdlibState& s);

        // Persistent stdlib-PRIME cache key: discriminator = compiler version + the sorted
        // stdlib-codegen-affecting flags, digest = content hash of the embedded stdlib
        // table + the eager/lazy prelude split. Pure and cheap; any of those re-keys.
        struct PrimeCacheKey {
            std::string discriminator;
            std::string digest;
        };
        static PrimeCacheKey stdlibPrimeCacheKey(
            const CompilerFlags& flags = CompilerFlags{});

        const string& getCpu() const {
            return cpu;
        }

        void setCpu(const string& cpu) {
            this->cpu = cpu;
            rebuildTargetMachine();
        }

        const string& getFeatures() const {
            return features;
        }

        void setFeatures(const string& features) {
            this->features = features;
            rebuildTargetMachine();
        }

        // Install (or clear, with nullptr) the process-global shared LLVMContext of the
        // test stdlib-reuse path. Toggling it must NOT clear s_sharedInitialized: the
        // context is primed once per process and its stdlib has to persist.
        static void setSharedContext(llvm::LLVMContext* ctx) { s_sharedContext = ctx; }
        static llvm::LLVMContext* getSharedContext() { return s_sharedContext; }
        llvm::LLVMContext* getActiveContext() { return activeContext; }

        // Reuse-cache hazard gate (test-only). While armed, the template instantiator
        // throws ReuseHazardAbort the first time a NOVEL stdlib instantiation would emit
        // into a per-test user module — the one operation that contaminates the context.
        static void setReuseHazardArmed(bool v) { s_reuseHazardArmed = v; }
        static bool isReuseHazardArmed() { return s_reuseHazardArmed; }

        // Convenience over `flags`; new sites should use getFlags()/setFlags() instead.
        bool isBoundsCheckEnabled() const { return flags.bounds != BoundsCheck::Off; }
        void setBoundsCheckEnabled(bool v) {
            flags.bounds = v ? BoundsCheck::On : BoundsCheck::Off;
        }

        // Session compile: script units parsed by this Compiler compile INTO `state` —
        // entry codegen seeds its root scope from it and writes ownership facts back — and
        // diagnostics carry `hostName`. The host owns the state; nullptr clears it.
        void setSessionState(SessionState* state, const string& hostName) {
            sessionState = state;
            sessionHostName = hostName;
        }
        SessionState* getSessionState() const { return sessionState; }
        const string& getSessionHostName() const { return sessionHostName; }

        // setMode resets the entire flag set to that mode's defaults, so per-feature
        // overrides must follow it.
        CompilerMode getMode() const { return mode; }
        void setMode(CompilerMode m) {
            mode = m;
            flags = CompilerFlags::defaultsForMode(m);
        }
        const CompilerFlags& getFlags() const { return flags; }
        CompilerFlags& getMutableFlags() { return flags; }
        void setFlags(const CompilerFlags& f) { flags = f; }

        const string& getTargetTriple() const { return targetTriple; }
        void setTargetTriple(const string& triple) { targetTriple = triple; rebuildTargetMachine(); }

        EmitMode getEmitMode() const { return emitMode; }
        void setEmitMode(EmitMode m) { emitMode = m; }

        // Single-backend setter: None clears the list, any other value becomes the only one.
        void setXpuBackend(XpuBackend b) {
            xpuBackends.clear();
            if (b != XpuBackend::None) xpuBackends.push_back(b);
        }
        // Append a backend (for the multi-target --xpu-backend=a,b list).
        void addXpuBackend(XpuBackend b) {
            if (b == XpuBackend::None) return;
            for (auto x : xpuBackends) if (x == b) return;
            xpuBackends.push_back(b);
        }
        const vector<XpuBackend>& getXpuBackends() const { return xpuBackends; }
        bool usesXpuBackend(XpuBackend b) const {
            for (auto x : xpuBackends) if (x == b) return true;
            return false;
        }
        // The first selected backend, or None — for callers that still think single-backend.
        XpuBackend getXpuBackend() const {
            return xpuBackends.empty() ? XpuBackend::None : xpuBackends.front();
        }
        XpuEmit getXpuEmit() const { return xpuEmit; }
        void setXpuEmit(XpuEmit e) { xpuEmit = e; }
        const string& getXpuArch() const { return xpuArch; }
        void setXpuArch(const string& a) { xpuArch = a; }

        void addClasspath(string s) { classpath.push_back(std::move(s)); }
        const std::vector<string>& getClasspath() const { return classpath; }

        // Uber-archive reachability pruning (default true). False bundles ALL classpath
        // entries, for reflective or dispatch paths the bitcode scan cannot see.
        void setPruneUber(bool v) { pruneUber = v; }
        void setSkillRootOverride(string s) { skillRootOverride = std::move(s); }
        void setCacheManifestPath(string p) { cacheManifestPath = std::move(p); }

        // The cache discriminator for this configuration: version + cache flag pairs +
        // profile + a content hash per classpath archive (a changed dep must re-key, since
        // source digests miss dep edits). Valid once flags/emit/target/classpath are final.
        string computeOwnCacheDiscriminator() const;
        bool getPruneUber() const { return pruneUber; }

        const string& getOutputPath() const { return outputPath; }
        void setOutputPath(const string& p) { outputPath = p; }

        llvm::TargetMachine* getTargetMachine() const { return targetMachine; }

        CajetaModulePtr createModule(string sourcePath, string sourceRootPath, string targetRootPath);

        // --lint --source-root: register sibling files' signatures (see .cpp).
        void registerLintContext(const string& root, const string& file,
                                 const string& shadow, bool json);

        list<CajetaModulePtr> getModules() {
            return modules;
        }

        // Splice ingested classpath modules into the main module list so codegen, merge and
        // JIT link treat them as ordinary modules — the JIT needs dep DEFINITIONS, not just
        // declarations. Opt-in, since archive emit keeps them external. Idempotent.
        void linkClasspathModules() {
            modules.splice(modules.end(), externalModules);
        }
    };
} // code