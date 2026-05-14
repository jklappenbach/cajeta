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
    ensureJitInitialized();

    auto [sourceRoot, sourcePath] = writeSourceToTemp(source, fqClassName);

    auto jitState = std::unique_ptr<CajetaJit>(new CajetaJit);

    // Compiler keeps its own LLVMContext; we hand the resulting module off to LLJIT
    // wrapped in a ThreadSafeModule that owns a copy of that context.
    auto compiler = std::make_unique<Compiler>();
    compiler->setBoundsCheckEnabled(opts.boundsCheckEnabled);
    auto archiveRoot = std::filesystem::temp_directory_path()
                     / ("cajeta_archive_" + sourceRoot.filename().string());
    std::filesystem::create_directories(archiveRoot);
    auto module = compiler->createModule(sourcePath.string(), sourceRoot.string(),
                                          archiveRoot.string());
    compiler->compile(module);

    // AspectModel.md § A3: pointcut matching runs after parse and
    // before any per-method codegen begins. Mirrors what
    // Compiler::compile(entryMethod, ...) does on its multi-file
    // path so JIT-based tests see the same advice-cache state.
    cajeta::CajetaModule::resolveAdviceMatches();

    // AspectModel.md § A8: DI graph validation. JIT-based tests
    // default to the "test" profile so @TestComponent overrides
    // and test-only @Profile("test") components are picked up.
    cajeta::CajetaModule::setActiveProfile("test");
    cajeta::CajetaModule::resolveDependencyGraph();

    // Two-phase codegen (signature registration then body lowering). Mirrors what
    // Compiler::compile(entryMethod,...) does for multi-file builds.
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

    // Link the embedded runtime into the module (no-op if no runtime symbols were
    // referenced; idempotent if linked already).
    module->linkRuntime();

    // Capture short→full name mapping before transferring the module to JIT.
    llvm::Module* llvmModule = module->getLlvmModule();
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
