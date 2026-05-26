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

    // Emit the per-module `__cajeta_unrecoverable_vtable_marker` global
    // pointing at UnrecoverableException's vtable. Must be called AFTER
    // CajetaModule::buildPendingPrototypes — the vtable doesn't exist
    // until the deferred-prototype pass lays out the stdlib classes.
    // Idempotent; safe to call multiple times per module.
    void emitUnrecoverableMarker(CajetaModulePtr module);

    enum class EmitMode {
        IR,       // Default: exploded text LLVM IR (.ll) per module
        Obj,      // Exploded native object files (.o) per module
        Cja,      // Single .cja archive — project-only IR (no stdlib, no deps)
        Uber,     // Single .cja archive — project + stdlib + transitively-referenced deps under deps/<name>-<ver>/
        Exe,      // Linked executable (requires lld in-process; see D1 / Compiler.cpp)
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
        // accumulate). Read by emitArchive(uber=true) to bundle each
        // listed archive's entries into the output with the
        // Origin::Dependency tag, nested under `deps/<name>-<version>/`.
        std::vector<string> classpath;
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