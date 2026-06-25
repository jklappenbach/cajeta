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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/NativeLink.h"
#include "cajeta/buildtool/NativeProvision.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
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

    // REFL-2 — fill the reflective invoke-adapter bodies + finalize/register
    // #ClassObjects now that every method's LLVM function exists. Mirrors
    // Compiler::compile's AOT pass and the JitTestHelper pipeline; WITHOUT it
    // the `__cajeta_*_reflect_invoke/new` thunks stay undefined and the JIT
    // materialization fails ("Symbols not found"). Idempotent.
    for (auto& [key, type] : cajeta::CajetaType::getCanonicalMap()) {
        if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(type)) {
            klass->emitReflectInvokeBody();
            klass->emitReflectNewBody();
            klass->finalizeClassObject();
        }
    }

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
    // Native-dependency requirements (native-deps unit 8): read the live
    // @Native libs from the module BEFORE it is moved into the JIT. ORC
    // generators (added after the process generator below) are lazy, so DCE is
    // automatic — a generator is consulted only for a symbol actually
    // materialized. The JIT NEVER fetches at run time; artifacts must be local.
    std::set<std::string> liveNativeLibs = cajeta::collectLiveNativeLibs(**parsed);

    llvm::orc::ThreadSafeModule tsModule(std::move(*parsed), std::move(tsContext));

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        std::cerr << "cajeta jit: LLJIT create failed: "
                  << cajeta::jit::toString(jitOrErr.takeError()) << "\n";
        out.errorCode = 1;
        return out;
    }
    out.jit = std::move(*jitOrErr);

    // GDB JIT symbolization. On in debug mode (so `cajeta jit-run -g` yields
    // named JIT frames under a debugger instead of bare addresses), off in
    // release (enableDebuggerSupport installs a JITLink plugin that synthesizes
    // and registers a debug object per module — real per-module overhead paid
    // whether or not a debugger ever attaches). CAJETA_JIT_GDB=1 force-enables
    // it regardless of mode for ad-hoc release-JIT diagnostics. Requires the
    // JITLink ObjectLinkingLayer (the LLVM 22 default on x86-64 ELF); on RTDyld
    // it returns an Error we surface and continue (symbolization simply absent).
    if (opts.debugInfo || std::getenv("CAJETA_JIT_GDB")) {
        if (auto err = llvm::orc::enableDebuggerSupport(*out.jit)) {
            std::cerr << "cajeta jit: GDB symbolization unavailable: "
                      << cajeta::jit::toString(std::move(err)) << "\n";
        }
    }

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

    // Native-dependency JIT generators (native-deps unit 8). For each
    // referenced @Native lib, resolve a LOCAL artifact (.cja native/ extraction
    // / CAJETA_NATIVE_PATH / ~/.cajeta/native — never the network) and add an
    // ORC generator so its symbols resolve at materialization. Lazy → a lib
    // whose symbols are never materialized is never loaded (DCE for JIT). A
    // genuinely-referenced-but-absent lib surfaces as an undefined symbol at
    // materialization (precise placement message lands in unit 11).
    if (!liveNativeLibs.empty()) {
        auto& execSession = out.jit->getExecutionSession();
        const char prefix = out.jit->getDataLayout().getGlobalPrefix();
        const std::string platform = cajeta::hostNativePlatform();
        const std::vector<std::string> dirs = cajeta::nativeLinkSearchDirs();
        for (const auto& lib : liveNativeLibs) {
            auto art = cajeta::findNativeJitArtifact(lib, platform, dirs);
            if (!art) continue;  // absent → lazy lookup fails loud only if needed
            if (art->isStatic) {
                auto gen = llvm::orc::StaticLibraryDefinitionGenerator::Load(
                    out.jit->getObjLinkingLayer(), art->path.c_str());
                if (gen) mainDylib.addGenerator(std::move(*gen));
                else std::cerr << "cajeta jit: native lib '" << lib
                               << "' load failed: "
                               << cajeta::jit::toString(gen.takeError()) << "\n";
            } else {
                auto gen = llvm::orc::DynamicLibrarySearchGenerator::Load(
                    art->path.c_str(), prefix);
                if (gen) mainDylib.addGenerator(std::move(*gen));
                else std::cerr << "cajeta jit: native lib '" << lib
                               << "' load failed: "
                               << cajeta::jit::toString(gen.takeError()) << "\n";
            }
        }
    }
    // NOTE: the fp128 soft-float helpers (__trunctfdf2, __fixtfdi, ...) that the
    // stdlib's Float128 emits are NOT installed here. Apple arm64 has no
    // __float128 type and ships no compiler-rt tf family, so there is nothing to
    // take the address of. Instead they are compiled (as target-neutral integer
    // IR) into the embedded runtime bitcode and linked into every JIT module —
    // see runtime/native/cajeta_fp128_builtins.ll and src/CMakeLists.txt.

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

void safepointTrampoline(int32_t locId, int fiberId, void* frameTop) {
    cajeta::dbg::DebugController* c;
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        c = g_activeController;
    }
    if (c) c->onSafepoint(locId, static_cast<long>(fiberId), frameTop);
}

// Install (or clear, when handler is null) the safepoint handler in the JIT
// module via its __cajeta_dbg_set_safepoint_handler symbol.
void installHandler(llvm::orc::LLJIT* jit, void (*handler)(int32_t, int, void*)) {
    using SetHandlerFn = void (*)(void (*)(int32_t, int, void*));
    if (auto sym = jit->lookup("__cajeta_dbg_set_safepoint_handler")) {
        if (auto setFn = reinterpret_cast<SetHandlerFn>(sym->getValue())) {
            setFn(handler);
        }
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }
}

// CP6f-3 exception trampoline — forwards a throw at the runtime chokepoint to
// the active session's controller. onException() no-ops unless exceptions are
// armed, so this is always installed (arming is a controller flag the DAP
// server flips via setExceptionBreakpoints).
void exceptionTrampoline(void* throwable, int fiberId, void* frameTop) {
    cajeta::dbg::DebugController* c;
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        c = g_activeController;
    }
    if (c) c->onException(throwable, static_cast<long>(fiberId), frameTop);
}

// Install (or clear) the exception handler via __cajeta_dbg_set_exception_handler.
void installExceptionHandler(llvm::orc::LLJIT* jit,
                             void (*handler)(void*, int, void*)) {
    using SetFn = void (*)(void (*)(void*, int, void*));
    if (auto sym = jit->lookup("__cajeta_dbg_set_exception_handler")) {
        if (auto setFn = reinterpret_cast<SetFn>(sym->getValue())) setFn(handler);
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

    // Native-deps unit 16: native resolution/provisioning is done (buildJit);
    // entering the execution phase. The net seam now hard-fails any native
    // network op for the remainder of the run (spec INV-2). Belt-and-suspenders:
    // the JIT resolves only local artifacts, so nothing here reaches out.
    cajeta::buildtool::setNativePhase(cajeta::buildtool::NativePhase::Execution);

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
    if (impl_->built.jit) {
        installHandler(impl_->built.jit.get(), nullptr);
        installExceptionHandler(impl_->built.jit.get(), nullptr);
    }
    return impl_->exitCode;
}

std::vector<JitDebugSession::FiberSnapshot> JitDebugSession::liveFibers() {
    std::vector<FiberSnapshot> out;
    llvm::orc::LLJIT* jit = impl_->built.jit.get();
    if (!jit) return out;

    // Resolve the registry accessors in the JIT module (the registry is
    // populated by the JIT'd program's runtime copy, not the host's native
    // copy). Any miss -> empty (graceful: a build without the CP6f-2a runtime
    // simply reports no fibers).
    auto resolve = [jit](const char* name) -> void* {
        if (auto sym = jit->lookup(name)) {
            return reinterpret_cast<void*>(sym->getValue());
        } else {
            cajeta::jit::consumeError(sym.takeError());
            return nullptr;
        }
    };
    // CP6f-2d unit 1: enumerate the registry via the atomic snapshot so the
    // handle list is one consistent view, not count()+at() with the lock
    // released between (a TOCTOU under the multi-carrier scheduler). Per-handle
    // field reads (id/frameTop/state) are still individually locked; the deeper
    // "handle freed under us" guarantee comes from cross-carrier quiesce (the
    // rest of carrier-quiesce-spec.md).
    auto snapFn = reinterpret_cast<int (*)(void**, int)>(resolve("__cajeta_dbg_fiber_snapshot"));
    auto idFn = reinterpret_cast<long (*)(void*)>(resolve("__cajeta_dbg_fiber_id_of"));
    auto ftFn = reinterpret_cast<void* (*)(void*)>(resolve("__cajeta_dbg_fiber_frame_top"));
    auto stFn = reinterpret_cast<int (*)(void*)>(resolve("__cajeta_dbg_fiber_state"));
    if (!idFn || !ftFn || !stFn) return out;

    std::vector<void*> handles;
    if (snapFn) {
        int n = snapFn(nullptr, 0);          // count only
        if (n > 0) {
            handles.resize(static_cast<size_t>(n));
            int got = snapFn(handles.data(), n);
            if (got > n) {                   // grew between the two calls — retry larger
                handles.resize(static_cast<size_t>(got));
                got = snapFn(handles.data(), got);
            }
            handles.resize(static_cast<size_t>(got < static_cast<int>(handles.size())
                                                   ? got : static_cast<int>(handles.size())));
        }
    } else {
        // Fallback for a pre-snapshot runtime: the old (TOCTOU-prone) path.
        auto countFn = reinterpret_cast<int (*)()>(resolve("__cajeta_dbg_fiber_count"));
        auto atFn = reinterpret_cast<void* (*)(int)>(resolve("__cajeta_dbg_fiber_at"));
        if (countFn && atFn) {
            int n = countFn();
            for (int i = 0; i < n; ++i) {
                if (void* h = atFn(i)) handles.push_back(h);
            }
        }
    }

    for (void* handle : handles) {
        if (!handle) continue;
        out.push_back(FiberSnapshot{
            static_cast<int>(idFn(handle)),
            ftFn(handle),
            stFn(handle),
        });
    }
    return out;
}

std::unique_ptr<JitDebugSession> startDebugSession(
        const JitRunOptions& opts,
        const std::vector<Breakpoint>& breakpoints,
        std::string* error,
        bool armExceptions) {
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
    // CP6f-3: arm break-on-throw BEFORE the program thread starts (below), so a
    // program that throws immediately can't race past the arm.
    if (armExceptions) impl->controller.armException();

    llvm::orc::LLJIT* jit = impl->built.jit.get();

    // Install the trampoline and publish this session's controller as active.
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        g_activeController = &impl->controller;
    }
    installHandler(jit, &safepointTrampoline);
    installExceptionHandler(jit, &exceptionTrampoline);
    callVoidSymbol(jit, "__cajeta_dbg_reset_safepoint_count");
    // CP6f-3c: disable throw-site backtrace capture in debug sessions. The
    // debugger supplies the stack itself (stackTrace), and backtrace(3) at the
    // throw site hangs/faults when the entry runs on the session's spawned
    // program thread under `cajeta dap` (mingw unwinder on a non-main thread).
    if (auto sym = jit->lookup("__cajeta_set_stack_trace_capture")) {
        using SetCap = void (*)(int);
        if (auto fn = reinterpret_cast<SetCap>(sym->getValue())) fn(0);
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }

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
