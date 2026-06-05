//
// Implementation of the JIT test helper.
//

#include "JitTestHelper.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/xpu/XpuTarget.h"

#include "JitWinSymbols.h"
#include "JitErrorShim.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
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
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        initialized = true;
    }
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
    static std::mt19937_64 rng(std::random_device{}());
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

} // namespace

CajetaJit::CajetaJit() = default;

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
    if (jit) {
        auto sym = jit->lookup("__cajeta_task_shutdown");
        if (sym) {
            auto fn = reinterpret_cast<void(*)()>(sym->getValue());
            if (fn) fn();
        } else {
            cajeta::jittest::consumeError(sym.takeError());
        }
    }
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

    static std::mt19937_64 rng(std::random_device{}());
    auto sourceRoot = std::filesystem::temp_directory_path()
                    / ("cajeta_multi_" + std::to_string(rng()));
    std::filesystem::create_directories(sourceRoot);

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

    auto jitState = std::unique_ptr<CajetaJit>(new CajetaJit);

    auto compiler = std::make_unique<Compiler>();
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
    auto archiveRoot = std::filesystem::temp_directory_path()
                     / ("cajeta_archive_" + sourceRoot.filename().string());
    std::filesystem::create_directories(archiveRoot);

    // Pre-scan the just-written sources into the archive so
    // forward references across files can create placeholders
    // (rather than throw or silently null) when their type's
    // declaration arrives later in the parse order.
    cajeta::prescanSourceRoot(sourceRoot.string());

    cajeta::CajetaModulePtr primary;
    for (auto& sourcePath : sourcePaths) {
        auto m = compiler->createModule(sourcePath.string(),
                                        sourceRoot.string(),
                                        archiveRoot.string());
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
        if (!primary) primary = m;
    }
    (void) fqEntryClass;

    // The compiler's stdlib module holds the parsed cajeta.error.*
    // classes + the runtime bitcode (parsed once per Compiler, see
    // Compiler::ensureStdlibModule). The merge below pulls it into
    // `primary` alongside the user modules — that's where extern
    // decls in user IR get resolved to real definitions.

    // Cross-module sweeps. Compiler::compile(module) already runs
    // buildPendingPrototypes + emitUnrecoverableMarker per call;
    // the rest of these only make sense after every module has
    // parsed, so they live here.
    cajeta::CajetaModule::validatePlaceholders();
    cajeta::CajetaModule::resolveAdviceMatches();
    cajeta::CajetaModule::setActiveProfile("test");
    cajeta::CajetaModule::resolveDependencyGraph();

    // Phase 1 (signature registration) + Phase 2 (body codegen),
    // looped until quiescent. A user method's body can codegen an
    // intrinsic that instantiates a stdlib template (e.g. `xs.stream()`
    // → ArrayStream<int32>), which lands the new methods in stdlib's
    // structures AFTER stdlib's earlier pass already ran. The do/while
    // re-runs both phases over every module until no new methods
    // surface — Method::generateCode and ::getLlvmFunctionType are
    // both idempotent so re-visiting already-emitted methods is a
    // cheap no-op.
    size_t prevMethodCount = 0;
    while (true) {
        size_t methodCount = 0;
        for (auto& m : compiler->getModules()) {
            methodCount += m->getAllMethods().size();
        }
        for (auto& m : compiler->getModules()) {
            for (auto& method : m->getAllMethods()) {
                method->getLlvmFunctionType();
            }
        }
        for (auto& m : compiler->getModules()) {
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
        for (auto& m : compiler->getModules()) {
            after += m->getAllMethods().size();
        }
        if (after == methodCount && after == prevMethodCount) break;
        prevMethodCount = after;
    }
    // P6.2 — after quiescence, emit per-class clinit-style ctors for
    // any static fields with non-foldable initializers. Mirrors what
    // Compiler.cpp does for non-JIT compilation.
    for (auto& m : compiler->getModules()) {
        for (auto& [name, klass] : m->getStructures()) {
            if (klass) klass->generateStaticInitializers();
        }
    }

    // Merge every secondary module into `primary`'s llvm::Module.
    // After the parse-stdlib-once refactor, each module owns only
    // its own classes' definitions + extern decls for everything
    // else, so a plain linkModules call works (no need for the old
    // OverrideFromSrc workaround). The Linker resolves each extern
    // decl to the single definition it finds across all donors.
    auto modulesList = compiler->getModules();
    for (auto& m : modulesList) {
        if (m == primary) continue;
        std::unique_ptr<llvm::Module> donor(m->getLlvmModule());
        if (llvm::Linker::linkModules(*primary->getLlvmModule(),
                                       std::move(donor))) {
            throw std::runtime_error("JIT module-merge failed");
        }
    }

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
            for (cajeta::xpu::Backend be : backends) {
                std::string arch =
                    be == cajeta::xpu::Backend::Nvptx  ? "sm_89"
                  : be == cajeta::xpu::Backend::Amdgpu ? "gfx1151"
                  : be == cajeta::xpu::Backend::Spirv  ? "vulkan1.3"
                  :                                      "";
                cajeta::xpu::emitKernelRegistration(
                    be, kernels, *primary->getLlvmModule(), arch);
            }
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
    llvm::orc::ThreadSafeModule tsModule(std::move(*parsed), std::move(tsContext));

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        throw std::runtime_error("LLJIT create failed");
    }
    jitState->jit = std::move(*jitOrErr);

    if (auto err = jitState->jit->addIRModule(std::move(tsModule))) {
        throw std::runtime_error("LLJIT addIRModule failed");
    }

    // Make host process symbols (printf, malloc, etc.) resolvable from JIT'd code.
    auto& mainDylib = jitState->jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jitState->jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        throw std::runtime_error("LLJIT process-symbol generator failed");
    }
    mainDylib.addGenerator(std::move(*generator));

#ifdef _WIN32
    // The generator above dlsym's the current process, but MinGW's CRT /
    // libgcc functions are statically linked into this test binary and not
    // in its PE export table, so it can't find them — the embedded runtime
    // bitcode's calls to write/open/__mingw_fprintf/stat64i32/... then fail
    // to materialize, cascading to every runtime-dependent symbol. Bind them
    // explicitly to the addresses JitWinSymbols.c took (in a TU compiled with
    // the same MinGW headers as the runtime, so each address is the exact
    // entry point the bitcode references).
    {
        size_t winSymCount = 0;
        const CajetaJitWinSym* winSyms = cajeta_jit_win_symbols(&winSymCount);
        auto& execSession = jitState->jit->getExecutionSession();
        llvm::orc::SymbolMap winSymMap;
        for (size_t i = 0; i < winSymCount; ++i) {
            winSymMap[execSession.intern(winSyms[i].name)] =
                llvm::orc::ExecutorSymbolDef(
                    llvm::orc::ExecutorAddr::fromPtr(winSyms[i].addr),
                    llvm::JITSymbolFlags::Exported);
        }
        cajeta::jittest::cantFail(
            mainDylib.define(llvm::orc::absoluteSymbols(std::move(winSymMap))));
    }
#endif

    // Run any global ctors / static initializers (P6.2 clinit, etc.)
    // before handing control to test code. LLJIT does NOT run
    // llvm.global_ctors automatically — the host runtime's
    // `__attribute__((constructor))` functions (__cajeta_runtime_init,
    // __cajeta_hash_seed_init) only fire because the runtime is also
    // statically linked into the test binary. Anything that lives ONLY
    // in the JIT module (per-class clinit emitted by
    // CajetaClass::generateStaticInitializers) needs this initialize
    // call to execute its initializer.
    if (auto err = jitState->jit->initialize(mainDylib)) {
        throw std::runtime_error("LLJIT initialize failed: "
            + cajeta::jittest::toString(std::move(err)));
    }

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
    if (auto sym = jitState->jit->lookup("__cajeta_set_poison_free")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getValue());
        if (setFn) setFn(desiredPoison);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_poison_free(desiredPoison);

    int desiredValidate = opts.dropChainValidateEnabled ? 1 : 0;
    if (auto sym = jitState->jit->lookup("__cajeta_set_drop_chain_validate")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getValue());
        if (setFn) setFn(desiredValidate);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_drop_chain_validate(desiredValidate);

    int desiredTrace = opts.stackTraceCaptureEnabled ? 1 : 0;
    if (auto sym = jitState->jit->lookup("__cajeta_set_stack_trace_capture")) {
        auto setFn = reinterpret_cast<void(*)(int)>(sym->getValue());
        if (setFn) setFn(desiredTrace);
    } else {
        cajeta::jittest::consumeError(sym.takeError());
    }
    ::__cajeta_set_stack_trace_capture(desiredTrace);

    return jitState;
}

void* CajetaJit::lookupAddress(const std::string& shortName) {
    auto it = nameMap.find(shortName);
    if (it == nameMap.end()) return nullptr;
    auto sym = jit->lookup(it->second);
    if (!sym) {
        cajeta::jittest::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getValue());
}

void* CajetaJit::lookupRawSymbol(const std::string& exactName) {
    auto sym = jit->lookup(exactName);
    if (!sym) {
        cajeta::jittest::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getValue());
}

} // namespace cajeta_test
