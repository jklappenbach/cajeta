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

    enum class EmitMode {
        IR,    // Default: text LLVM IR (.ll) per module
        Obj,   // Native object file (.o) for the configured target
        Exe,   // Linked executable (requires lld in-process; see D1 / Compiler.cpp)
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
        // Default-on. Set false by --bounds=off; propagated to each CajetaModule on
        // creation so ArrayIndexExpression can choose whether to emit the check.
        bool boundsCheckEnabled = true;
        // Output mode. Default IR (write .ll). --emit=obj or --emit=exe switches to
        // native codegen for the configured target.
        EmitMode emitMode = EmitMode::IR;
        // Optional output path override for single-file builds (--output / -o).
        // When empty, .ll/.o files land in the archive root mirroring the source tree
        // and executables land at <archive-root>/<entry-name>.
        string outputPath;

        // Collected .o paths from Obj/Exe emissions, fed to the linker for Exe mode.
        std::vector<string> objectFiles;

        // (Re)build the TargetMachine for the current triple/cpu/features. Called from
        // the constructor and again after any CLI flag changes the target settings.
        void rebuildTargetMachine();

        // Per-module emit dispatch based on emitMode.
        void emitForModule(CajetaModulePtr module);

        // Phase-2 linker step for --emit=exe; collects everything in objectFiles and
        // invokes lld (when CAJETA_HAS_LLD is defined at CMake-configure time).
        void linkExecutable(const string& archiveRootPath);

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

        bool isBoundsCheckEnabled() const { return boundsCheckEnabled; }
        void setBoundsCheckEnabled(bool v) { boundsCheckEnabled = v; }

        const string& getTargetTriple() const { return targetTriple; }
        void setTargetTriple(const string& triple) { targetTriple = triple; rebuildTargetMachine(); }

        EmitMode getEmitMode() const { return emitMode; }
        void setEmitMode(EmitMode m) { emitMode = m; }

        const string& getOutputPath() const { return outputPath; }
        void setOutputPath(const string& p) { outputPath = p; }

        llvm::TargetMachine* getTargetMachine() const { return targetMachine; }

        CajetaModulePtr createModule(string sourcePath, string sourceRootPath, string targetRootPath);

        list<CajetaModulePtr> getModules() {
            return modules;
        }
    };
} // code