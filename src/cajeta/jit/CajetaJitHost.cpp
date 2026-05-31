//
// In-process JIT host implementation. The compile→merge→JIT pipeline mirrors
// the proven path in test/jit/JitTestHelper.cpp; the differences are: it reads
// real .cajeta files from a source root (rather than temp-written strings),
// routes llvm::Error consumption through the RTTI-free shim, skips the XPU
// kernel host-launch registration (CP1 host-only smoke), and invokes a chosen
// static no-arg entry directly after jit->initialize() — exactly how every JIT
// test calls generated functions.
//
#include "cajeta/jit/CajetaJitHost.h"

#include "cajeta/jit/CajetaJitErrorShim.h"
#include "cajeta/jit/CajetaJitWinSymbols.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

namespace cajeta::jit {

namespace {

// ORC needs the native asm parser in addition to the codegen targets the
// Compiler ctor already initializes. Idempotent.
void ensureJitInitialized() {
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        initialized = true;
    }
}

// Collect every *.cajeta file under `root` (recursive).
std::vector<std::filesystem::path> collectSources(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".cajeta") {
            out.push_back(it->path());
        }
    }
    return out;
}

// Resolve a dotted `package.Class.method` entry to its cajeta-mangled IR
// function name (`package.Class::method(...)`). Returns "" if not found.
std::string findEntryMangled(llvm::Module* mod, const std::string& dottedEntry) {
    std::string target = entryTargetFromDotted(dottedEntry);
    if (target.empty()) return "";
    std::string withParen = target + "(";
    for (auto& fn : *mod) {
        if (fn.isDeclaration()) continue;
        std::string name = fn.getName().str();
        if (name == target || name.rfind(withParen, 0) == 0) {
            return name;
        }
    }
    return "";
}

// Count call sites to @__cajeta_dbg_safepoint inside one function (CP2: one
// per statement). Static — reads the IR, independent of execution.
int countSafepointCalls(llvm::Function* fn) {
    if (!fn) return 0;
    int n = 0;
    for (auto& bb : *fn) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                llvm::Function* callee = call->getCalledFunction();
                if (callee && callee->getName() == "__cajeta_dbg_safepoint") n++;
            }
        }
    }
    return n;
}

} // namespace

std::string entryTargetFromDotted(const std::string& dotted) {
    auto lastDot = dotted.rfind('.');
    if (lastDot == std::string::npos || lastDot + 1 >= dotted.size()) return "";
    if (lastDot == 0) return "";  // ".method" — no class segment
    return dotted.substr(0, lastDot) + "::" + dotted.substr(lastDot + 1);
}

int runJit(const JitRunOptions& opts, JitRunResult* result) {
    ensureJitInitialized();

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path sourceRoot = fs::absolute(opts.sourceRoot, ec);
    if (ec || !fs::is_directory(sourceRoot)) {
        std::cerr << "cajeta jit-run: source root is not a directory: "
                  << opts.sourceRoot << "\n";
        return 2;
    }

    std::vector<fs::path> sourcePaths = collectSources(sourceRoot);
    if (sourcePaths.empty()) {
        std::cerr << "cajeta jit-run: no .cajeta files under " << sourceRoot << "\n";
        return 2;
    }

    auto compiler = std::make_unique<Compiler>();
    compiler->setMode(CompilerMode::Debug);
    // Debugger CP2: opt into statement-boundary safepoint emission. Reset the
    // global loc table so this compile's loc_ids start at 0.
    compiler->getMutableFlags().debugInfo = opts.debugInfo;
    if (opts.debugInfo) cajeta::dbg::globalDbgLocTable().clear();

    fs::path archiveRoot = fs::temp_directory_path()
                         / ("cajeta_jitrun_" + sourceRoot.filename().string());
    fs::create_directories(archiveRoot, ec);

    // Pre-scan so cross-file forward references can create placeholders.
    cajeta::prescanSourceRoot(sourceRoot.string());

    cajeta::CajetaModulePtr primary;
    try {
        for (auto& sourcePath : sourcePaths) {
            auto m = compiler->createModule(sourcePath.string(),
                                            sourceRoot.string(),
                                            archiveRoot.string());
            compiler->compile(m);
            if (!primary) primary = m;
        }
    } catch (cajeta::Exception& e) {
        std::cerr << "cajeta jit-run: [" << e.getErrorId() << "] "
                  << e.getMessage() << "\n";
        return 1;
    }
    if (!primary) {
        std::cerr << "cajeta jit-run: no modules compiled\n";
        return 1;
    }

    // Cross-module sweeps (only valid once every module has parsed).
    cajeta::CajetaModule::validatePlaceholders();
    cajeta::CajetaModule::resolveAdviceMatches();
    cajeta::CajetaModule::setActiveProfile("debug");
    cajeta::CajetaModule::resolveDependencyGraph();

    // Phase 1 (signatures) + Phase 2 (bodies), looped to quiescence — a body
    // can instantiate a stdlib template that adds new methods.
    size_t prevMethodCount = 0;
    while (true) {
        size_t methodCount = 0;
        for (auto& m : compiler->getModules()) methodCount += m->getAllMethods().size();
        for (auto& m : compiler->getModules())
            for (auto& method : m->getAllMethods()) method->getLlvmFunctionType();
        for (auto& m : compiler->getModules())
            for (auto& method : m->getAllMethods()) method->generateCode();
        size_t after = 0;
        for (auto& m : compiler->getModules()) after += m->getAllMethods().size();
        if (after == methodCount && after == prevMethodCount) break;
        prevMethodCount = after;
    }

    // Per-class static initializers (clinit), registered with llvm.global_ctors.
    for (auto& m : compiler->getModules())
        for (auto& [name, klass] : m->getStructures())
            if (klass) klass->generateStaticInitializers();

    // Merge every secondary module (incl. the stdlib module carrying the
    // embedded runtime) into primary's llvm::Module.
    for (auto& m : compiler->getModules()) {
        if (m == primary) continue;
        std::unique_ptr<llvm::Module> donor(m->getLlvmModule());
        if (llvm::Linker::linkModules(*primary->getLlvmModule(), std::move(donor))) {
            std::cerr << "cajeta jit-run: module merge failed\n";
            return 1;
        }
    }

    llvm::Module* llvmModule = primary->getLlvmModule();

    std::string entryName = findEntryMangled(llvmModule, opts.entryMethod);
    if (entryName.empty()) {
        std::cerr << "cajeta jit-run: could not find static no-arg entry `"
                  << opts.entryMethod << "` (package.Class.method)\n";
        return 1;
    }

    if (result) {
        result->entrySafepointsEmitted =
            countSafepointCalls(llvmModule->getFunction(entryName));
    }

    if (std::getenv("CAJETA_DUMP_IR")) llvmModule->print(llvm::errs(), nullptr);

    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*llvmModule, &verifyStream)) {
        std::cerr << "cajeta jit-run: IR verify failed: " << verifyErr << "\n";
        return 1;
    }

    // Round-trip the module through bitcode into a fresh context owned by a
    // ThreadSafeModule (the documented LLJIT path when the source context
    // isn't owned by the caller).
    auto tsCtx = std::make_unique<llvm::LLVMContext>();
    llvm::SmallVector<char, 0> bitcodeBuf;
    {
        llvm::raw_svector_ostream os(bitcodeBuf);
        llvm::WriteBitcodeToFile(*llvmModule, os);
    }
    auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(bitcodeBuf.data(), bitcodeBuf.size()), "cajeta_jitrun");
    llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
#if LLVM_VERSION_MAJOR >= 21
    auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
        return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), *ctx);
    });
#else
    auto parsed = llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                         *tsContext.getContext());
#endif
    if (!parsed) {
        std::cerr << "cajeta jit-run: bitcode reparse failed: "
                  << cajeta::jit::toString(parsed.takeError()) << "\n";
        return 1;
    }
    llvm::orc::ThreadSafeModule tsModule(std::move(*parsed), std::move(tsContext));

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        std::cerr << "cajeta jit-run: LLJIT create failed: "
                  << cajeta::jit::toString(jitOrErr.takeError()) << "\n";
        return 1;
    }
    std::unique_ptr<llvm::orc::LLJIT> jit = std::move(*jitOrErr);

    if (auto err = jit->addIRModule(std::move(tsModule))) {
        std::cerr << "cajeta jit-run: addIRModule failed: "
                  << cajeta::jit::toString(std::move(err)) << "\n";
        return 1;
    }

    // Resolve host process symbols (printf, malloc, runtime helpers, ...).
    auto& mainDylib = jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        std::cerr << "cajeta jit-run: process-symbol generator failed: "
                  << cajeta::jit::toString(generator.takeError()) << "\n";
        return 1;
    }
    mainDylib.addGenerator(std::move(*generator));

    // Windows/MinGW: bind statically-linked CRT/libgcc functions the generator
    // can't see via the PE export table.
    {
        size_t winSymCount = 0;
        const JitWinSym* winSyms = winJitSymbols(&winSymCount);
        if (winSymCount) {
            auto& execSession = jit->getExecutionSession();
            llvm::orc::SymbolMap winSymMap;
            for (size_t i = 0; i < winSymCount; ++i) {
                winSymMap[execSession.intern(winSyms[i].name)] =
                    llvm::orc::ExecutorSymbolDef(
                        llvm::orc::ExecutorAddr::fromPtr(winSyms[i].addr),
                        llvm::JITSymbolFlags::Exported);
            }
            cajeta::jit::cantFail(
                mainDylib.define(llvm::orc::absoluteSymbols(std::move(winSymMap))));
        }
    }

    // Run global ctors (runtime init + per-class clinit). LLJIT does not run
    // llvm.global_ctors automatically.
    if (auto err = jit->initialize(mainDylib)) {
        std::cerr << "cajeta jit-run: LLJIT initialize failed: "
                  << cajeta::jit::toString(std::move(err)) << "\n";
        return 1;
    }

    auto entrySym = jit->lookup(entryName);
    if (!entrySym) {
        std::cerr << "cajeta jit-run: entry symbol lookup failed: "
                  << cajeta::jit::toString(entrySym.takeError()) << "\n";
        return 1;
    }
    // Run the program. The entry runs on this thread; user spawns lazily start
    // the carrier thread. Honor an int32 return as the process exit code (and
    // report it) so a dependency-free `return N` program is observably correct;
    // void entries just run for their side effects.
    llvm::Function* entryLlvm = llvmModule->getFunction(entryName);
    bool returnsInt32 = entryLlvm && entryLlvm->getReturnType()->isIntegerTy(32);

    // CP2: reset the JIT module's safepoint counter immediately before the
    // entry runs, so safepointsExecuted measures only the entry's execution —
    // not the global ctors run by jit->initialize() above.
    if (auto rs = jit->lookup("__cajeta_dbg_reset_safepoint_count")) {
        if (auto f = reinterpret_cast<void(*)()>(rs->getValue())) f();
    } else {
        cajeta::jit::consumeError(rs.takeError());
    }

    int rc = 0;
    if (returnsInt32) {
        auto fn = reinterpret_cast<int(*)()>(entrySym->getValue());
        rc = fn();
        std::cerr << "[jit-run] entry " << opts.entryMethod
                  << " returned " << rc << "\n";
    } else {
        auto fn = reinterpret_cast<void(*)()>(entrySym->getValue());
        fn();
        std::cerr << "[jit-run] entry " << opts.entryMethod
                  << " completed (void)\n";
    }

    if (result) {
        if (auto cs = jit->lookup("__cajeta_dbg_safepoint_count")) {
            auto f = reinterpret_cast<long(*)()>(cs->getValue());
            result->safepointsExecuted = f ? f() : 0;
        } else {
            cajeta::jit::consumeError(cs.takeError());
        }
    }

    // Join any carrier thread cleanly before tearing down the JIT module (the
    // module owns the carrier's code). Mirrors CajetaJit's destructor.
    if (auto sym = jit->lookup("__cajeta_task_shutdown")) {
        if (auto fn = reinterpret_cast<void(*)()>(sym->getValue())) fn();
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }

    return rc;
}

int dispatchJitRun(int argc, const char* argv[]) {
    // argv: cajeta jit-run [-g|--debug-info] <sourceRoot> <entryMethod> [args...]
    JitRunOptions opts;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-g" || a == "--debug-info" || a == "--debug-info=on") {
            opts.debugInfo = true;
        } else if (a == "--debug-info=off") {
            opts.debugInfo = false;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() < 2) {
        std::cerr << "usage: cajeta jit-run [-g] <sourceRoot>"
                     " <package.Class.method> [args...]\n";
        return 2;
    }
    opts.sourceRoot = positional[0];
    opts.entryMethod = positional[1];
    for (size_t i = 2; i < positional.size(); ++i)
        opts.programArgs.push_back(positional[i]);
    return runJit(opts);
}

} // namespace cajeta::jit
