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
#include "cajeta/method/Method.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

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
CajetaJit::~CajetaJit() = default;

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
        compiler->compile(m);
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
                method->generateCode();
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
    auto parsed = llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                          *tsContext.getContext());
    if (!parsed) {
        throw std::runtime_error("JIT bitcode reparse failed");
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
            + llvm::toString(std::move(err)));
    }

    return jitState;
}

void* CajetaJit::lookupAddress(const std::string& shortName) {
    auto it = nameMap.find(shortName);
    if (it == nameMap.end()) return nullptr;
    auto sym = jit->lookup(it->second);
    if (!sym) {
        llvm::consumeError(sym.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(sym->getValue());
}

} // namespace cajeta_test
