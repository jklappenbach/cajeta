//
// In-process JIT host implementation. The compile→merge→JIT pipeline mirrors
// the proven path in test/jit/JitTestHelper.cpp; the differences are: it reads
// real .cajeta files from a source root (rather than temp-written strings),
// routes llvm::Error consumption through the RTTI-free shim, skips the XPU
// kernel host-launch registration (CP1 host-only smoke), and invokes a chosen
// static no-arg entry directly after jit->initialize() — exactly how every JIT
// test calls generated functions.
//
// CP3 adds debug sessions: the compile+build step is shared (buildJit), and
// startDebugSession runs the entry on a background thread wired to a
// DebugController so an armed safepoint parks until the caller resumes.
//
#include "cajeta/jit/CajetaJitHost.h"

#include "cajeta/jit/CajetaJitErrorShim.h"
#include "cajeta/jit/CajetaJitWinSymbols.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

// Result of the shared compile→merge→build-LLJIT pipeline. Owns the Compiler
// (keeps the source llvm::Module/context alive) and the LLJIT for as long as
// the program may run.
struct BuiltJit {
    std::unique_ptr<Compiler> compiler;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::string entryName;          // cajeta-mangled IR name of the entry fn
    bool returnsInt32 = false;
    int entrySafepointsEmitted = 0; // static count inside the entry fn
    int errorCode = 0;              // 0 ok; else a runJit-style return code
};

// Shared pipeline: compile every .cajeta under opts.sourceRoot, merge modules,
// build + initialize an LLJIT, and resolve the entry. On failure sets
// errorCode (and prints to stderr) and leaves jit null.
BuiltJit buildJit(const JitRunOptions& opts) {
    BuiltJit out;
    ensureJitInitialized();

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path sourceRoot = fs::absolute(opts.sourceRoot, ec);
    if (ec || !fs::is_directory(sourceRoot)) {
        std::cerr << "cajeta jit: source root is not a directory: "
                  << opts.sourceRoot << "\n";
        out.errorCode = 2;
        return out;
    }

    std::vector<fs::path> sourcePaths = collectSources(sourceRoot);
    if (sourcePaths.empty()) {
        std::cerr << "cajeta jit: no .cajeta files under " << sourceRoot << "\n";
        out.errorCode = 2;
        return out;
    }

    out.compiler = std::make_unique<Compiler>();
    Compiler* compiler = out.compiler.get();
    compiler->setMode(CompilerMode::Debug);
    // Statement-boundary safepoint emission (CP2). Reset the global loc table
    // so this compile's loc_ids start at 0.
    compiler->getMutableFlags().debugInfo = opts.debugInfo;
    if (opts.debugInfo) cajeta::dbg::globalDbgLocTable().clear();

    fs::path archiveRoot = fs::temp_directory_path()
                         / ("cajeta_jitrun_" + sourceRoot.filename().string());
    fs::create_directories(archiveRoot, ec);

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
        std::cerr << "cajeta jit: [" << e.getErrorId() << "] "
                  << e.getMessage() << "\n";
        out.errorCode = 1;
        return out;
    }
    if (!primary) {
        std::cerr << "cajeta jit: no modules compiled\n";
        out.errorCode = 1;
        return out;
    }

    cajeta::CajetaModule::validatePlaceholders();
    cajeta::CajetaModule::resolveAdviceMatches();
    cajeta::CajetaModule::setActiveProfile("debug");
    cajeta::CajetaModule::resolveDependencyGraph();

    // Phase 1 (signatures) + Phase 2 (bodies) to quiescence.
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

    for (auto& m : compiler->getModules())
        for (auto& [name, klass] : m->getStructures())
            if (klass) klass->generateStaticInitializers();

    for (auto& m : compiler->getModules()) {
        if (m == primary) continue;
        std::unique_ptr<llvm::Module> donor(m->getLlvmModule());
        if (llvm::Linker::linkModules(*primary->getLlvmModule(), std::move(donor))) {
            std::cerr << "cajeta jit: module merge failed\n";
            out.errorCode = 1;
            return out;
        }
    }

    llvm::Module* llvmModule = primary->getLlvmModule();

    out.entryName = findEntryMangled(llvmModule, opts.entryMethod);
    if (out.entryName.empty()) {
        std::cerr << "cajeta jit: could not find static no-arg entry `"
                  << opts.entryMethod << "` (package.Class.method)\n";
        out.errorCode = 1;
        return out;
    }
    llvm::Function* entryLlvm = llvmModule->getFunction(out.entryName);
    out.returnsInt32 = entryLlvm && entryLlvm->getReturnType()->isIntegerTy(32);
    out.entrySafepointsEmitted = countSafepointCalls(entryLlvm);

    if (std::getenv("CAJETA_DUMP_IR")) llvmModule->print(llvm::errs(), nullptr);

    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*llvmModule, &verifyStream)) {
        std::cerr << "cajeta jit: IR verify failed: " << verifyErr << "\n";
        out.errorCode = 1;
        return out;
    }

    // Round-trip the module through bitcode into a fresh context owned by a
    // ThreadSafeModule (the documented LLJIT path).
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
        std::cerr << "cajeta jit: bitcode reparse failed: "
                  << cajeta::jit::toString(parsed.takeError()) << "\n";
        out.errorCode = 1;
        return out;
    }
    llvm::orc::ThreadSafeModule tsModule(std::move(*parsed), std::move(tsContext));

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        std::cerr << "cajeta jit: LLJIT create failed: "
                  << cajeta::jit::toString(jitOrErr.takeError()) << "\n";
        out.errorCode = 1;
        return out;
    }
    out.jit = std::move(*jitOrErr);

    if (auto err = out.jit->addIRModule(std::move(tsModule))) {
        std::cerr << "cajeta jit: addIRModule failed: "
                  << cajeta::jit::toString(std::move(err)) << "\n";
        out.jit.reset();
        out.errorCode = 1;
        return out;
    }

    auto& mainDylib = out.jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        out.jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        std::cerr << "cajeta jit: process-symbol generator failed: "
                  << cajeta::jit::toString(generator.takeError()) << "\n";
        out.jit.reset();
        out.errorCode = 1;
        return out;
    }
    mainDylib.addGenerator(std::move(*generator));

    {
        size_t winSymCount = 0;
        const JitWinSym* winSyms = winJitSymbols(&winSymCount);
        if (winSymCount) {
            auto& execSession = out.jit->getExecutionSession();
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

    if (auto err = out.jit->initialize(mainDylib)) {
        std::cerr << "cajeta jit: LLJIT initialize failed: "
                  << cajeta::jit::toString(std::move(err)) << "\n";
        out.jit.reset();
        out.errorCode = 1;
        return out;
    }

    return out;
}

// Look up a `void(*)()` symbol in the JIT and call it (best-effort).
void callVoidSymbol(llvm::orc::LLJIT* jit, const char* name) {
    if (auto sym = jit->lookup(name)) {
        if (auto fn = reinterpret_cast<void(*)()>(sym->getValue())) fn();
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }
}

// --- CP3 safepoint trampoline ------------------------------------------------
// The JIT'd code's __cajeta_dbg_safepoint calls the installed handler through a
// plain function pointer (no name resolution), so this file-local function with
// a C-compatible signature suffices. It forwards to the active session's
// controller. Only one debug session runs in-process at a time.
std::mutex g_activeMutex;
cajeta::dbg::DebugController* g_activeController = nullptr;

void safepointTrampoline(int32_t locId, int fiberId) {
    cajeta::dbg::DebugController* c;
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        c = g_activeController;
    }
    if (c) c->onSafepoint(locId, static_cast<long>(fiberId));
}

// Install (or clear, when handler is null) the safepoint handler in the JIT
// module via its __cajeta_dbg_set_safepoint_handler symbol.
void installHandler(llvm::orc::LLJIT* jit, void (*handler)(int32_t, int)) {
    using SetHandlerFn = void (*)(void (*)(int32_t, int));
    if (auto sym = jit->lookup("__cajeta_dbg_set_safepoint_handler")) {
        if (auto setFn = reinterpret_cast<SetHandlerFn>(sym->getValue())) {
            setFn(handler);
        }
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }
}

} // namespace

std::string entryTargetFromDotted(const std::string& dotted) {
    auto lastDot = dotted.rfind('.');
    if (lastDot == std::string::npos || lastDot + 1 >= dotted.size()) return "";
    if (lastDot == 0) return "";  // ".method" — no class segment
    return dotted.substr(0, lastDot) + "::" + dotted.substr(lastDot + 1);
}

int runJit(const JitRunOptions& opts, JitRunResult* result) {
    BuiltJit built = buildJit(opts);
    if (built.errorCode != 0 || !built.jit) return built.errorCode;

    llvm::orc::LLJIT* jit = built.jit.get();
    if (result) result->entrySafepointsEmitted = built.entrySafepointsEmitted;

    auto entrySym = jit->lookup(built.entryName);
    if (!entrySym) {
        std::cerr << "cajeta jit-run: entry symbol lookup failed: "
                  << cajeta::jit::toString(entrySym.takeError()) << "\n";
        return 1;
    }

    // CP2: reset the JIT module's safepoint counter so safepointsExecuted
    // measures only the entry's execution, not the global ctors above.
    callVoidSymbol(jit, "__cajeta_dbg_reset_safepoint_count");

    int rc = 0;
    if (built.returnsInt32) {
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

    // Join any carrier thread cleanly before tearing down the JIT module.
    callVoidSymbol(jit, "__cajeta_task_shutdown");
    return rc;
}

// --- JitDebugSession ---------------------------------------------------------

struct JitDebugSession::Impl {
    BuiltJit built;
    cajeta::dbg::DebugController controller;
    std::thread thread;
    std::atomic<bool> finished{false};
    int exitCode = 0;
    bool joined = false;
};

JitDebugSession::JitDebugSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

JitDebugSession::~JitDebugSession() {
    if (impl_) join();
}

cajeta::dbg::DebugController& JitDebugSession::controller() {
    return impl_->controller;
}

bool JitDebugSession::isFinished() const {
    return impl_->finished.load();
}

int JitDebugSession::join() {
    if (impl_->joined) return impl_->exitCode;
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->joined = true;
    // Detach the handler + active controller now the program has stopped.
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        if (g_activeController == &impl_->controller) g_activeController = nullptr;
    }
    if (impl_->built.jit) installHandler(impl_->built.jit.get(), nullptr);
    return impl_->exitCode;
}

std::unique_ptr<JitDebugSession> startDebugSession(
        const JitRunOptions& opts,
        const std::vector<Breakpoint>& breakpoints,
        std::string* error) {
    // Debug sessions always emit safepoints.
    JitRunOptions dbgOpts = opts;
    dbgOpts.debugInfo = true;

    auto impl = std::make_unique<JitDebugSession::Impl>();
    impl->built = buildJit(dbgOpts);
    if (impl->built.errorCode != 0 || !impl->built.jit) {
        if (error) *error = "compile/JIT failed (see stderr)";
        return nullptr;
    }

    // Arm: match each breakpoint against the loc table by file BASENAME + line.
    namespace fs = std::filesystem;
    const auto& table = cajeta::dbg::globalDbgLocTable();
    for (const auto& bp : breakpoints) {
        for (size_t id = 0; id < table.size(); ++id) {
            const auto& loc = table.at(static_cast<int32_t>(id));
            if (loc.line != bp.line) continue;
            std::string base = fs::path(loc.file).filename().string();
            if (base == bp.file || loc.file == bp.file) {
                impl->controller.arm(static_cast<int32_t>(id));
            }
        }
    }

    llvm::orc::LLJIT* jit = impl->built.jit.get();

    // Install the trampoline and publish this session's controller as active.
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        g_activeController = &impl->controller;
    }
    installHandler(jit, &safepointTrampoline);
    callVoidSymbol(jit, "__cajeta_dbg_reset_safepoint_count");

    auto entrySym = jit->lookup(impl->built.entryName);
    if (!entrySym) {
        if (error) *error = "entry symbol lookup failed";
        cajeta::jit::consumeError(entrySym.takeError());
        {
            std::lock_guard<std::mutex> lock(g_activeMutex);
            g_activeController = nullptr;
        }
        installHandler(jit, nullptr);
        return nullptr;
    }
    void* entryAddr = reinterpret_cast<void*>(entrySym->getValue());
    bool returnsInt32 = impl->built.returnsInt32;

    JitDebugSession::Impl* raw = impl.get();
    raw->thread = std::thread([raw, entryAddr, returnsInt32]() {
        if (returnsInt32) {
            raw->exitCode = reinterpret_cast<int(*)()>(entryAddr)();
        } else {
            reinterpret_cast<void(*)()>(entryAddr)();
            raw->exitCode = 0;
        }
        // Shut the fiber carrier down from the program thread (it owns the
        // carrier), then mark finished.
        callVoidSymbol(raw->built.jit.get(), "__cajeta_task_shutdown");
        raw->finished.store(true);
    });

    return std::make_unique<JitDebugSession>(std::move(impl));
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
