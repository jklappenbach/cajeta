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
#include <string>
#include "../error/Exception.h"

using namespace std;
namespace cajeta {
    class CajetaModule;

    class AbstractSyntaxNode;

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
    };

    // What device artifact (if any) to also drop to disk for inspection,
    // alongside the normal --emit output. None registers cubins only.
    enum class XpuEmit {
        None,     // Default: registration only, no standalone artifact.
        Ptx,      // Write a per-kernel .ptx next to the module output.
        Cubin,    // Write a per-kernel .cubin (implies ptxas must be present).
    };

    class Compiler {
    private:
        string targetTriple;
        const llvm::Target* target;
        llvm::LLVMContext llvmContext;
        string cpu = "generic";
        string features = "";
        llvm::TargetMachine* targetMachine;
        llvm::TargetOptions opt;
        std::optional<llvm::Reloc::Model> RM;
        list<CajetaModulePtr> modules;
        // Compiler mode + per-feature toggle struct (cajeta-docs/CompilerModes.md).
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
        // unchanged. xpuArch is the SM target handed to the NVPTX TargetMachine
        // and ptxas (e.g. "sm_89"); only consulted when xpuBackend == Nvptx.
        XpuBackend xpuBackend = XpuBackend::None;
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

    public:
        Compiler(int argc, const char* argv[]) : Compiler() { }

        Compiler() {
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmPrinters();
            llvm::InitializeAllAsmParsers();
            targetTriple = llvm::sys::getDefaultTargetTriple();
            // Drop cached llvm::Type* and module/method tables from any previous Compiler;
            // they hold pointers tied to a now-destroyed LLVMContext. Without this each
            // new Compiler instance crashes on first use of a stale entry.
            CajetaType::resetGlobals();
            CajetaModule::resetGlobals();
            rebuildTargetMachine();
            if (target) {
                CajetaType::init(llvmContext);
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

        const string& getCpu() const {
            return cpu;
        }

        void setCpu(const string& cpu) {
            this->cpu = cpu;
        }

        const string& getFeatures() const {
            return features;
        }

        void setFeatures(const string& features) {
            this->features = features;
        }

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

        XpuBackend getXpuBackend() const { return xpuBackend; }
        void setXpuBackend(XpuBackend b) { xpuBackend = b; }
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