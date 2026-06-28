//
// Created by James Klappenbach on 10/24/22.
//

#pragma once

#include <iostream>
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

    // Thrown by the template instantiator when reuse-cache hazard detection is
    // armed (Compiler::setReuseHazardArmed) and a novel stdlib-template
    // instantiation would emit into a per-test user module. Deliberately NOT a
    // cajeta::Exception subclass so the compile loop's catch(Exception&)
    // diagnostic wrapper ignores it and it propagates straight to the JIT
    // harness's fresh-fallback handler. Carries no payload — it's a control
    // signal, not an error.
    struct ReuseHazardAbort {};

    // Walk every .cajeta file under `rootPath` and register every
    // declared class/interface/struct's (canonical, shortName) pair
    // in the archive (CajetaType::registerArchive). Used by the
    // multi-file compile paths (Compiler::compile(entryMethod, ...)
    // and the JIT helper's multi-source overload) to populate the
    // archive registry BEFORE any module parses, so cross-file
    // forward references can create placeholders for names that
    // are known-to-exist somewhere in the compilation unit.
    void prescanSourceRoot(const std::string& rootPath);

    // Emit a per-module global ctor that registers UnrecoverableException's
    // vtable with the runtime (__cajeta_set_unrecoverable_vtable). Must be
    // called AFTER CajetaModule::buildPendingPrototypes — the vtable doesn't
    // exist until the deferred-prototype pass lays out the stdlib classes.
    // Idempotent; safe to call multiple times per module.
    void emitUnrecoverableMarker(CajetaModulePtr module);

    enum class EmitMode {
        IR,       // Default: exploded text LLVM IR (.ll) per module
        Obj,      // Exploded native object files (.o) per module
        Cja,      // Single .cja archive — project-only IR (no stdlib, no deps)
        Uber,     // Single .cja archive — project + stdlib + transitively-referenced deps under deps/<name>-<ver>/
        Exe,      // Linked executable (requires lld in-process; see D1 / Compiler.cpp)
    };

    // XPU (GPU compute) backend selection for the AOT path. None (default)
    // leaves @Kernel methods as host stubs — the host-only program is
    // byte-identical to a no-XPU build. Nvptx builds each @Kernel's cubin and
    // embeds it + a registration ctor into the host module (mirroring the JIT
    // helper), so a compiled `kernel.launch(...)` resolves the device function
    // at runtime. (cajeta-xpu.md — was JIT-test-only before these CLI flags.)
    enum class XpuBackend {
        None,     // Default: no device codegen.
        Nvptx,    // NVIDIA: AST → device IR → PTX → ptxas → cubin, registered in-module.
        Amdgpu,   // AMD: AST → device IR → AMDGCN ISA → lld → hsaco, registered in-module.
        Vulkan,   // Vulkan: AST → device IR → SPIR-V (descriptor-set SSBOs), registered in-module.
        Cpu,      // CPU: AST → host IR (grid→threads), linked into the module + registered.
    };

    // What device artifact (if any) to also drop to disk for inspection,
    // alongside the normal --emit output. None registers cubins only.
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

    class Compiler {
    private:
        string targetTriple;
        const llvm::Target* target;
        llvm::LLVMContext llvmContext;
        // The context every module this Compiler creates is bound to. Defaults
        // to the owned `llvmContext`. When a process-global shared context has
        // been installed (test stdlib-reuse path, see s_sharedContext), it
        // points there instead so the stdlib module + its cached llvm::Type*
        // survive across Compiler instances. Production leaves it at the owned
        // context — behavior is identical.
        llvm::LLVMContext* activeContext = &llvmContext;
        // This compile's per-thread mutable state (thread-safe-compiler Unit 1).
        // The scope guard makes it the calling thread's current context for this
        // Compiler's lifetime, restoring the previous current on destruction so a
        // persistent prime context stays current beneath per-test Compilers.
        // Declared before the ctor body so currentCompilationContext() is set
        // before later units' reset/init run. Unit 1: marker only, no reads yet.
        CompilationContext compilationContext;
        ScopedCompilationContext compilationContextScope{&compilationContext};
        // Process-global shared context for the test stdlib-reuse path. When
        // non-null, newly constructed Compilers bind to it and SKIP the
        // global-state reset/init (the StdlibCache owns that lifecycle).
        // nullptr in production — every Compiler is fully self-contained.
        static llvm::LLVMContext* s_sharedContext;
        // One-time guard: the FIRST Compiler built under a shared context
        // resets + inits the global type tables in that context (priming);
        // every Compiler after reuses them. Reset to false when the shared
        // context is cleared so a later priming starts clean.
        static bool s_sharedInitialized;
        // Armed only during a JIT-harness stdlib-reuse attempt; see
        // setReuseHazardArmed / ReuseHazardAbort. Always false in production.
        static bool s_reuseHazardArmed;
        string cpu = "generic";
        string features = "";
        llvm::TargetMachine* targetMachine;
        llvm::TargetOptions opt;
        std::optional<llvm::Reloc::Model> RM;
        list<CajetaModulePtr> modules;
        // Compiler mode + per-feature toggle struct (docs/CompilerModes.md).
        // CLI flavor flags (--debug, --release, ...) set `mode` and reset `flags`
        // to that mode's defaults; per-feature flags (--bounds=, --source-tags=, ...)
        // override individual fields after. Forwarded to each CajetaModule on creation
        // so codegen sites can read the effective toggle (bounds-check emission,
        // source-tag passing, etc.).
        CompilerMode mode = CompilerMode::Debug;
        CompilerFlags flags = CompilerFlags::defaultsForMode(CompilerMode::Debug);
        // Output mode. Default IR (write .ll). --emit=obj or --emit=exe switches to
        // native codegen for the configured target.
        EmitMode emitMode = EmitMode::IR;
        // XPU device backend + per-kernel artifact emit (--xpu-backend /
        // --xpu-emit / --xpu-arch). Default None: the AOT path is host-only and
        // unchanged. xpuArch is the device arch handed to the backend's
        // TargetMachine + assembler (e.g. "sm_89" for nvptx, "gfx1151" for
        // amdgpu); consulted whenever xpuBackend != None.
        // Selected device backends (empty = None). A list so a single binary
        // can bundle several targets (e.g. --xpu-backend=vulkan,cpu); the
        // runtime dispatcher (Increment 4) picks the best available at launch.
        // Registration runs for every entry; --xpu-emit artifacts are dropped
        // for whichever listed backend the emit kind belongs to.
        vector<XpuBackend> xpuBackends;
        XpuEmit xpuEmit = XpuEmit::None;
        string xpuArch = "sm_89";
        // Optional output path override for single-file builds (--output / -o).
        // When empty, .ll/.o files land in the archive root mirroring the source tree
        // and executables land at <archive-root>/<entry-name>.
        string outputPath;

        // Entry method (`pkg.Class.method` dotted form) — set by
        // Compiler::compile(entryMethod, ...) before parsing fires. Read by
        // emitCMainShim after Phase-2 codegen, when binary emit is on.
        string entryMethod;

        // Package source root (trailing-slash normalized) — set by
        // Compiler::compile(...) so emitArchive can locate the package's
        // `skills/` dir and embed skill members into the `.cja` (skill-discovery
        // D.3). Empty when compile() hasn't run (e.g. unit tests).
        string skillSourceRoot;

        // Optional override (via `--skill-root=<dir>`) for where the package's
        // `skills/` dir lives. The build tool sets this to the PROJECT root
        // (where `cajeta.json` and `skills/` sit), since the compile source
        // root is the deeper `src/main/cajeta`. Empty → fall back to the
        // compile source root (the low-level `cajeta <entry> <src> <arc>` form).
        string skillRootOverride;

        // Collected .o paths from Obj/Exe emissions, fed to the linker for Exe mode.
        std::vector<string> objectFiles;

        // --classpath archive paths. Set via CLI (one or more
        // --classpath=a.cja,b.cja args; comma-separates and repeats both
        // accumulate). Consumed twice:
        //   1. ingestClasspath() at compile-start, which reads each
        //      archive's ClassSource entries and re-parses them into
        //      `externalModules`, registering their classes in the
        //      canonical-name map so user code can resolve imports
        //      against them.
        //   2. emitArchive(uber=true), which bundles each archive's
        //      bitcode entries into the uber output under
        //      `deps/<name>-<version>/`.
        std::vector<string> classpath;

        // Modules built from classpath archives' ClassSource entries.
        // Each holds a parsed CajetaModule whose classes were
        // registered in the canonical-name map; the LLVM module
        // backing it is NEVER emitted/linked (the corresponding
        // bitcode lives in the consumer's bundled dep — uber — or in
        // the classpath archive consumed at the next compile-stage
        // link).
        std::list<CajetaModulePtr> externalModules;
        // Default behavior of --emit=uber. true → prune classpath
        // entries to the transitively-referenced closure (set via
        // --prune-uber=on, the default); false → bundle every
        // classpath entry (set via --prune-uber=off).
        bool pruneUber = true;

        // (Re)build the TargetMachine for the current triple/cpu/features. Called from
        // the constructor and again after any CLI flag changes the target settings.
        void rebuildTargetMachine();

        // Per-module emit dispatch based on emitMode.
        void emitForModule(CajetaModulePtr module);

        // XPU device codegen for the AOT path (--xpu-backend=nvptx). Collects
        // every @Kernel across the parsed modules, embeds each one's cubin +
        // registration ctor into its host module (NvptxRegistration, the same
        // pass the JIT helper runs), and — when --xpu-emit is ptx/cubin — also
        // drops a per-kernel .ptx/.cubin under `archiveRootPath` for inspection.
        // No-op when xpuBackend == None. Never throws: unsupported kernels
        // (XPU-N01) or a missing ptxas are skipped with a diagnostic.
        void emitXpuKernels(const std::string& archiveRootPath);

        // Walk every archive on `classpath`, re-parse each ClassSource
        // entry into a fresh CajetaModule, and register the resulting
        // CajetaClass objects in the canonical-name map. Called once,
        // immediately after the stdlib parse and before user-source
        // prescan, so user imports can resolve against classpath
        // classes during their own parse. No-op when classpath empty.
        void ingestClasspath();

        // Archive emit (--emit=cja or --emit=uber). Bundles every
        // module's LLVM bitcode into a single .cja file. Cja form
        // uses ArchiveKind::Cja and ships ONLY the user's project
        // bitcode (stdlib + classpath deps stripped — a library
        // archive). Uber form uses ArchiveKind::Uber and bundles
        // the project, stdlib, and every transitively-referenced
        // classpath dep nested under deps/<name>-<version>/ —
        // a runnable, self-contained deployment artifact. The
        // output path is `outputPath` when set via -o, else
        // `<archiveRoot>/cajeta.cja`.
        void emitArchive(const std::string& archiveRootPath, bool uber);

        // Phase-2 linker step for --emit=exe; collects everything in objectFiles and
        // invokes lld (when CAJETA_HAS_LLD is defined at CMake-configure time).
        void linkExecutable(const string& archiveRootPath);

        // Emit a C-callable `main` symbol that invokes the user's static entry
        // method (the `entryMethod` positional arg, dotted form like
        // `demo.Hello.run`). Synthesized into the stdlib module so the linker
        // resolves the symbol regardless of which user module declared the
        // entry. Only runs for binary emit (obj/exe) where the program needs
        // a C ABI entry point. No-op (with diagnostic) if the entry method
        // doesn't resolve.
        void emitCMainShim(const std::string& entryMethod);

        // Tier-1 RTA (plans/compiler/stdlib-tree-shaking.md). Shared reachability
        // engine: collect every module in the final link, then BFS the by-name
        // reference graph from the entry + ctor/ABI roots (incl. EH personality +
        // prefix/prologue data). Conservative over-approximation (a referenced
        // vtable keeps all its slots) — the sound direction for gating emission.
        void collectLinkModules(std::vector<llvm::Module*>& lmods);
        // excludeClinitRoots: drop the __cajeta_clinit_* entries from the
        // global_ctors roots — i.e. "what the program reaches if no static
        // initializer ran" (the oracle Tier-1.5 clinit-DCE queries).
        std::unordered_set<std::string> computeReachableSymbols(
            const std::vector<llvm::Module*>& lmods,
            std::unordered_map<std::string, llvm::GlobalValue*>& defs,
            bool excludeClinitRoots = false);

        // Phase A: diff reachable vs all defined functions and print what Phase B
        // would strip. Analysis only — emits nothing, mutates no IR.
        void reportTreeShake();
        // Phase B/C: deleteBody() unreachable cajeta method bodies (--tree-shake=on)
        // so their native references vanish and the linker never pulls them.
        // Runs after the main shim + all ctors exist, before per-module emit.
        void pruneUnreachable();
        // Tier-1.5 clinit-DCE (lean-linker-dce.md §3.6.7): strip the static
        // initializer of a class whose statics are dead and whose clinit triggers
        // only dead code. Runs BEFORE pruneUnreachable so the cascade is picked up.
        void pruneDeadClinits();

    public:
        Compiler(int argc, const char* argv[]) : Compiler() { }

        Compiler() {
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmPrinters();
            llvm::InitializeAllAsmParsers();
            targetTriple = llvm::sys::getDefaultTargetTriple();
            if (s_sharedContext) {
                // Stdlib-reuse path (tests): a process-global context + the
                // parsed stdlib and its llvm::Type* cache persist across
                // Compilers. The first Compiler primes the global type tables
                // in that context; the rest reuse them (the test harness
                // restores a clean per-test baseline itself). Either way we
                // bind the context and build this instance's target machine.
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
                // Drop cached llvm::Type* and module/method tables from any
                // previous Compiler; they hold pointers tied to a
                // now-destroyed LLVMContext. Without this each new Compiler
                // instance crashes on first use of a stale entry.
                CajetaType::resetGlobals();
                CajetaModule::resetGlobals();
                rebuildTargetMachine();
                if (target) {
                    CajetaType::init(*activeContext);
                }
            }
        }

        ~Compiler() { }

        void compile(string entryMethod, string sourceRootPath, string archiveRootPath);

        void compile(CajetaModulePtr module);

        // Ensure the Compiler's dedicated stdlib module exists and is
        // parsed + prototype-built. Idempotent — created lazily on
        // first call, returned thereafter. Driven by every entry
        // point (compile(module), compile(entryMethod, ...)) so any
        // caller order works.
        CajetaModulePtr ensureStdlibModule();

        // Lazy stdlib instrumentation / control. The set of stdlib packages
        // actually parsed (eager + on-demand) and the lazy bookkeeping are
        // process-global. stdlibPackageParsed / stdlibParsedPackages are the
        // test probes for the on-demand-parse behavior; resetLazyStdlibState
        // lets the stdlib-reuse harness clear the bookkeeping when it restores
        // its per-test baseline (the fresh-Compiler path resets it itself in
        // parseStdlibInto). See the lazy stdlib loader in Compiler.cpp.
        static bool stdlibPackageParsed(const std::string& pkg);
        static const std::set<std::string>& stdlibParsedPackages();
        static void resetLazyStdlibState();

        const string& getCpu() const {
            return cpu;
        }

        void setCpu(const string& cpu) {
            this->cpu = cpu;
            rebuildTargetMachine(); // CPU feeds the subtarget — rebuild like setTargetTriple
        }

        const string& getFeatures() const {
            return features;
        }

        void setFeatures(const string& features) {
            this->features = features;
            rebuildTargetMachine();
        }

        // Install (or clear, with nullptr) the process-global shared LLVMContext
        // used by the test stdlib-reuse path. Compilers constructed after this
        // bind their modules to `ctx` and skip the per-instance global-state
        // reset, so a primed stdlib persists across them. Test-only;
        // production never calls this and is unaffected.
        // Toggling the shared context on/off (reuse vs fresh-isolated test)
        // must NOT clear s_sharedInitialized: the shared context is primed once
        // per process and its stdlib persists. Clearing it here would make the
        // next shared-context Compiler re-run resetGlobals/init and wipe the
        // cached baseline. A fresh-isolated test (ctx == nullptr) takes the
        // else-branch and is unaffected either way.
        static void setSharedContext(llvm::LLVMContext* ctx) { s_sharedContext = ctx; }
        static llvm::LLVMContext* getSharedContext() { return s_sharedContext; }
        llvm::LLVMContext* getActiveContext() { return activeContext; }

        // Reuse-cache hazard gate (test-only). When ARMED (set by the JIT test
        // harness during a stdlib-reuse attempt), the template instantiator
        // throws ReuseHazardAbort the first time a NOVEL stdlib-template
        // instantiation would emit its IR into a per-test USER module
        // (emitOwner != stdlib module) — the one operation that contaminates the
        // shared reuse context with module-bound llvm pointers outliving that
        // user module. The harness catches it and transparently re-runs the test
        // on a fresh, fully-isolated Compiler. Disarmed in production and on the
        // fresh fallback path, so --emit and non-reuse tests never throw.
        static void setReuseHazardArmed(bool v) { s_reuseHazardArmed = v; }
        static bool isReuseHazardArmed() { return s_reuseHazardArmed; }

        // Bounds-check accessors — convenience over the new flags struct.
        // Existing callers (CLI parser, CajetaModule plumbing) read these;
        // new sites should go through getFlags() / setFlags() instead.
        bool isBoundsCheckEnabled() const { return flags.bounds != BoundsCheck::Off; }
        void setBoundsCheckEnabled(bool v) {
            flags.bounds = v ? BoundsCheck::On : BoundsCheck::Off;
        }

        // Compiler mode + per-feature flag accessors. setMode resets the
        // entire flag set to that mode's defaults; per-feature overrides
        // should follow setMode in the CLI parser.
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

        // Single-backend setter (backward compatible): None clears the list,
        // any other value makes it the sole selected backend.
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
        // The first selected backend, or None if none — for callers that still
        // think single-backend (and the no-op guard in emitXpuKernels).
        XpuBackend getXpuBackend() const {
            return xpuBackends.empty() ? XpuBackend::None : xpuBackends.front();
        }
        XpuEmit getXpuEmit() const { return xpuEmit; }
        void setXpuEmit(XpuEmit e) { xpuEmit = e; }
        const string& getXpuArch() const { return xpuArch; }
        void setXpuArch(const string& a) { xpuArch = a; }

        void addClasspath(string s) { classpath.push_back(std::move(s)); }
        const std::vector<string>& getClasspath() const { return classpath; }

        // Uber-archive reachability pruning. Default true — only bundle
        // classpath-archive entries whose canonical name appears as a
        // substring in user / stdlib / already-reachable-dep bitcode.
        // Catches direct references AND transitive reach through the
        // iterative closure. Set to false to bundle ALL classpath entries
        // (the v1 "include everything" behavior) — useful when the user
        // knows about reflective / dynamic-dispatch paths the bitcode
        // scan can't see.
        void setPruneUber(bool v) { pruneUber = v; }
        void setSkillRootOverride(string s) { skillRootOverride = std::move(s); }
        bool getPruneUber() const { return pruneUber; }

        const string& getOutputPath() const { return outputPath; }
        void setOutputPath(const string& p) { outputPath = p; }

        llvm::TargetMachine* getTargetMachine() const { return targetMachine; }

        CajetaModulePtr createModule(string sourcePath, string sourceRootPath, string targetRootPath);

        list<CajetaModulePtr> getModules() {
            return modules;
        }
    };
} // code