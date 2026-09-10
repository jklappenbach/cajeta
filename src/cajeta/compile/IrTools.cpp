#include "cajeta/compile/IrTools.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"

#include <iostream>
#include <string>
#include <string_view>

namespace cajeta {

    namespace {

        struct Args {
            std::string input;
            std::string output;
            // Empty, not "generic": that is what `llc` defaults to, and the two differ in
            // subtarget nop selection, which would break byte-identical output.
            std::string cpu;
            std::string features;
            std::string relocModel = "pic";
            bool help = false;
            std::string error;
        };

        bool takeValue(std::string_view arg, std::string_view name, std::string& out) {
            const std::string eq = std::string("--") + std::string(name) + "=";
            if (arg.rfind(eq, 0) == 0) {
                out = std::string(arg.substr(eq.size()));
                return true;
            }
            return false;
        }

        Args parseArgs(int argc, const char* argv[]) {
            Args a;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                if (arg == "--help" || arg == "-h") { a.help = true; continue; }
                if (arg == "-o") {
                    if (i + 1 >= argc) { a.error = "-o needs a path"; return a; }
                    a.output = argv[++i];
                    continue;
                }
                if (takeValue(arg, "output", a.output)) continue;
                if (takeValue(arg, "cpu", a.cpu)) continue;
                if (takeValue(arg, "features", a.features)) continue;
                if (takeValue(arg, "relocation-model", a.relocModel)) continue;
                // `-filetype=obj` is accepted and ignored, so an llc command line ports
                // by changing the program name and nothing else.
                if (arg.rfind("-filetype=", 0) == 0) {
                    if (arg != "-filetype=obj") {
                        a.error = "only -filetype=obj is supported (got " +
                                  std::string(arg) + ")";
                        return a;
                    }
                    continue;
                }
                if (arg.rfind("-relocation-model=", 0) == 0) {
                    a.relocModel = std::string(arg.substr(18));
                    continue;
                }
                if (!arg.empty() && arg.front() == '-') {
                    a.error = "unknown option '" + std::string(arg) + "'";
                    return a;
                }
                if (a.input.empty()) { a.input = std::string(arg); continue; }
                a.error = "more than one input file";
                return a;
            }
            return a;
        }

        std::unique_ptr<llvm::Module> parseInput(
            llvm::LLVMContext& ctx, const std::string& path, const char* verb) {
            llvm::SMDiagnostic err;
            // parseIRFile takes bitcode and textual IR alike, so no pre-disassembly.
            auto m = llvm::parseIRFile(path, err, ctx);
            if (!m) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                err.print("cajeta", os);
                std::cerr << "cajeta " << verb << ": " << msg << std::endl;
            }
            return m;
        }

    } // namespace

    // Give the session-install symbols weak definitions before lowering. They are
    // extern on purpose (the host defines them), but an AOT object has no host, so the
    // references would not resolve; weak lets a host's strong definitions still win.
    void defineSessionInstallSymbolsWeakly(llvm::Module& m) {
        static const char* kSessionSymbols[] = {
            "__cajeta_install_hook",   // int32_t (*)(...)  — the host's hook
            "__cajeta_install_ctx",    // void*             — its context
            "__cajeta_install_out",    // char[2048]        — the result buffer
        };
        for (const char* name : kSessionSymbols) {
            llvm::GlobalVariable* g = m.getNamedGlobal(name);
            if (g == nullptr || !g->isDeclaration()) continue;
            // Null is what the host starts them at, and `__cajeta_session_install`
            // already reports "no live session" for a null hook.
            g->setInitializer(llvm::Constant::getNullValue(g->getValueType()));
            g->setLinkage(llvm::GlobalValue::WeakAnyLinkage);
        }
    }

    int irLowerCommand(int argc, const char* argv[]) {
        Args a = parseArgs(argc, argv);
        if (a.help || (a.input.empty() && a.error.empty())) {
            std::cout
                << "Usage: cajeta lower <in.ll|in.bc> -o <out.o> [options]\n"
                << "\n"
                << "Lower LLVM IR to a native object with THIS compiler's LLVM,\n"
                << "the equivalent of `llc -filetype=obj`. Use this instead of a\n"
                << "separately installed llc: a mismatched LLVM cannot parse the\n"
                << "IR cajeta emits, and the failure surfaces as a parser error\n"
                << "that names nothing useful.\n"
                << "\n"
                << "Options:\n"
                << "  -o <path>                  Output object (required)\n"
                << "  --cpu=<name>               Target CPU (default generic; `native` for the host)\n"
                << "  --features=<list>          Subtarget features\n"
                << "  --relocation-model=<m>     static|pic|dynamic-no-pic (default pic)\n"
                << "  -filetype=obj              Accepted and ignored (llc compatibility)\n";
            return a.input.empty() && !a.help ? 1 : 0;
        }
        if (!a.error.empty()) {
            std::cerr << "cajeta lower: " << a.error << "\n";
            return 1;
        }
        if (a.output.empty()) {
            std::cerr << "cajeta lower: -o <out.o> is required\n";
            return 1;
        }

        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();

        llvm::LLVMContext ctx;
        auto module = parseInput(ctx, a.input, "lower");
        if (!module) return 1;

        defineSessionInstallSymbolsWeakly(*module);

        // The module's own triple wins: it was emitted by this compiler, and overriding
        // it with the host's would silently miscompile a cross-target build.
        std::string tripleStr = module->getTargetTriple().str();
        if (tripleStr.empty()) {
            tripleStr = llvm::sys::getDefaultTargetTriple();
        }
        llvm::Triple triple(tripleStr);

        std::string err;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target) {
            std::cerr << "cajeta lower: no target for '" << tripleStr << "': " << err << "\n";
            return 1;
        }

        std::string cpu = a.cpu;
        std::string features = a.features;
        if (cpu == "native") {
            cpu = llvm::sys::getHostCPUName().str();
            llvm::SubtargetFeatures feats(a.features);
            for (const auto& f : llvm::sys::getHostCPUFeatures())
                feats.AddFeature(f.first(), f.second);
            features = feats.getString();
        }

        std::optional<llvm::Reloc::Model> rm = llvm::Reloc::PIC_;
        if (a.relocModel == "static") rm = llvm::Reloc::Static;
        else if (a.relocModel == "dynamic-no-pic") rm = llvm::Reloc::DynamicNoPIC;
        else if (a.relocModel != "pic") {
            std::cerr << "cajeta lower: unknown relocation model '" << a.relocModel << "'\n";
            return 1;
        }

        llvm::TargetOptions opt;
        // Emit llvm.global_ctors as `.init_array`, not the legacy `.ctors`: TargetOptions
        // defaults this FALSE, and modern glibc startup silently never runs `.ctors`.
        // Compiler.cpp carries the same line, and these objects link beside those.
        opt.UseInitArray = true;
        std::unique_ptr<llvm::TargetMachine> tm(
            target->createTargetMachine(triple, cpu, features, opt, rm));
        if (!tm) {
            std::cerr << "cajeta lower: could not create a target machine for "
                      << tripleStr << "\n";
            return 1;
        }
        module->setDataLayout(tm->createDataLayout());

        std::error_code ec;
        llvm::raw_fd_ostream dest(a.output, ec, llvm::sys::fs::OF_None);
        if (ec) {
            std::cerr << "cajeta lower: cannot write " << a.output << ": "
                      << ec.message() << "\n";
            return 1;
        }

        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            std::cerr << "cajeta lower: target cannot emit object files for "
                      << tripleStr << "\n";
            return 1;
        }
        pm.run(*module);
        dest.flush();
        return 0;
    }

    int irDisasmCommand(int argc, const char* argv[]) {
        Args a = parseArgs(argc, argv);
        if (a.help || (a.input.empty() && a.error.empty())) {
            std::cout
                << "Usage: cajeta disasm <in.bc> -o <out.ll>\n"
                << "\n"
                << "Print LLVM bitcode as textual IR with THIS compiler's LLVM,\n"
                << "the equivalent of `llvm-dis`. Writes stdout when -o is absent.\n";
            return a.input.empty() && !a.help ? 1 : 0;
        }
        if (!a.error.empty()) {
            std::cerr << "cajeta disasm: " << a.error << "\n";
            return 1;
        }

        llvm::LLVMContext ctx;
        auto module = parseInput(ctx, a.input, "disasm");
        if (!module) return 1;

        if (a.output.empty()) {
            module->print(llvm::outs(), nullptr);
            return 0;
        }
        std::error_code ec;
        llvm::raw_fd_ostream dest(a.output, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            std::cerr << "cajeta disasm: cannot write " << a.output << ": "
                      << ec.message() << "\n";
            return 1;
        }
        module->print(dest, nullptr);
        dest.flush();
        return 0;
    }

}
