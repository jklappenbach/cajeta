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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/DropBackfill.h"
#include "cajeta/compile/NativeLink.h"
#include "cajeta/buildtool/NativeProvision.h"
#include "cajeta/buildtool/Lockfile.h"
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
// Resolve the entry function, accepting the two shapes the compiled-binary
// shim accepts (Compiler::emitCMainShim): a no-arg `main()`, or the canonical
// application entry `static int32 main(String[] args)`.
//
// Matching is on the LLVM SIGNATURE, not on the spelling of the parameter, so
// `String[]` and `cajeta.lang.String[]` both resolve (spec 7.2.2). The earlier
// version bound only `target + "()"`, which made every conventional entry
// unlaunchable; the version before THAT prefix-matched `method(` and would call
// a parameterized method through a no-arg pointer, which is UB. Returning the
// arity here lets the caller pick a correctly-typed pointer (spec 7.2.5).
std::string findEntryMangled(llvm::Module* mod, const std::string& dottedEntry,
                             bool* takesArgs) {
    if (takesArgs) *takesArgs = false;
    std::string target = entryTargetFromDotted(dottedEntry);
    if (target.empty()) return "";

    const std::string noArg = target + "()";
    const std::string withParams = target + "(";
    std::string argsForm;

    for (auto& fn : *mod) {
        if (fn.isDeclaration()) continue;
        std::string name = fn.getName().str();
        if (name == noArg || name == target) return name;   // no-arg wins
        // Exactly one pointer parameter is the `String[] args` shape. Any other
        // arity is a different overload and is NOT an entry point.
        if (name.rfind(withParams, 0) == 0 && argsForm.empty() &&
            fn.arg_size() == 1 && fn.getArg(0)->getType()->isPointerTy()) {
            argsForm = name;
        }
    }
    if (!argsForm.empty()) {
        if (takesArgs) *takesArgs = true;
        return argsForm;
    }
    return "";
}


// The runtime's argv marshaller — the SAME one Compiler::emitCMainShim calls for
// a compiled binary (spec 7.2.4: one marshalling rule, or the debugger and the
// binary disagree about what a program's args are). Resolved out of the JIT so
// the allocation and the String vtable come from the program's own module.
extern "C" void* __cajeta_args_make(int64_t argc, char** argv,
                                    void* string_vtable, int64_t str_size,
                                    int64_t off_lentag, int64_t off_aux,
                                    int64_t off_base, int64_t off_cplen);

// The String ABI facts makeEntryArgs needs, split from their derivation so a
// whole-program cache HIT (no type world) can carry them in the slot's meta
// sidecar (fast-debug-launch 4.2.4). On a cold build they are derived from
// the live types right after LLJIT initialize.
struct EntryArgsABI {
    bool valid = false;
    int64_t strSize = 0;
    int64_t offLenTag = 0;
    int64_t offAux = 0;
    int64_t offBase = 0;
    int64_t offCpLen = 0;
    std::string vtableSymbol;  // `<canonical>#VTable`
};

// Derive the String ABI from the live type world (cold path only).
// Layout from the LLJIT's DataLayout — the one the JIT'd code actually
// uses, exactly as the shim reads the module's. Nothing about the String
// ABI is hardcoded here or there.
//
// NOT from compiler->getModules(): buildJit LINKS every non-primary module
// into the primary, and Linker::linkModules consumes the donor, so reaching
// back through that list yields a destroyed Module and segfaults in
// getStructLayout. The struct TYPE is context-owned and outlives the merge.
EntryArgsABI deriveEntryArgsABI(llvm::orc::LLJIT* jit) {
    EntryArgsABI abi;
    if (!jit) return abi;
    auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(
        cajeta::CajetaType::of("String"));
    if (!klass) return abi;
    auto* strStructTy = llvm::dyn_cast_or_null<llvm::StructType>(
        klass->getLlvmType());
    if (!strStructTy) return abi;

    const llvm::DataLayout& dl = jit->getDataLayout();
    const llvm::StructLayout* sl = dl.getStructLayout(strStructTy);
    abi.strSize = (int64_t) dl.getTypeAllocSize(strStructTy);
    abi.offLenTag = (int64_t) sl->getElementOffset(1);
    abi.offAux    = (int64_t) sl->getElementOffset(2);
    abi.offBase   = (int64_t) sl->getElementOffset(3);
    abi.offCpLen  = (int64_t) sl->getElementOffset(4);
    abi.vtableSymbol = klass->getQName()->toCanonical() + "#VTable";
    abi.valid = true;
    return abi;
}

// Build the cajeta `String[]` to hand a `main(String[] args)` entry.
// Returns nullptr if the String class or its layout is unavailable, in which
// case the caller must NOT invoke a parameterized entry.
void* makeEntryArgs(llvm::orc::LLJIT* jit,
                    const std::vector<std::string>& programArgs,
                    const EntryArgsABI& abi) {
    if (!jit || !abi.valid) return nullptr;

    // The vtable lives in the JIT'd module, so take its RUNTIME address.
    // Looked up by its canonical NAME (`<class>#VTable`, the same string
    // StructureMetadata emits) — NOT via klass->getVirtualTableGlobal():
    // that cached GlobalVariable* can point into a donor module the merge
    // already consumed (late drop-thunk synthesis re-homes it there), and
    // dereferencing it here is a read of freed memory.
    void* vtable = nullptr;
    if (auto sym = jit->lookup(abi.vtableSymbol)) {
        vtable = reinterpret_cast<void*>(sym->getValue());
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }

    std::vector<char*> argv;
    argv.reserve(programArgs.size());
    for (auto& a : programArgs) argv.push_back(const_cast<char*>(a.c_str()));

    return __cajeta_args_make((int64_t) argv.size(),
                              argv.empty() ? nullptr : argv.data(),
                              vtable, abi.strSize, abi.offLenTag, abi.offAux,
                              abi.offBase, abi.offCpLen);
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
    // True when the entry is `main(String[] args)`; the caller must then build
    // the args array and invoke through an int(*)(void*) pointer.
    bool entryTakesArgs = false;
    int entrySafepointsEmitted = 0; // static count inside the entry fn
    int errorCode = 0;              // 0 ok; else a runJit-style return code
    JitBuildPhases phases;          // wall-clock breakdown (fast-debug-launch 1.2.1)
    EntryArgsABI entryArgsABI;      // derived cold / read from slot meta on hit
    bool cacheHit = false;          // served from the whole-program slot (4.1.1)
};

// --- Whole-program merged-module cache (fast-debug-launch Unit 4) -----------
// Slot layout under <cacheDir>/jit/<key>/:
//   program.bc     — the MERGED program's bitcode (exactly the bytes the LLJIT
//                    round-trip serializes on a cold build)
//   program.meta   — entry facts + String ABI (tab-delimited, versioned)
//   program.dbgloc — the loc-table sidecar (only for -g builds)
// The key is content-addressed (compiler version+git ⊕ flags ⊕ entry ⊕ every
// source digest), so "stale" is impossible by construction; load failures of
// any kind mean MISS, never an error.
//
// Classpath archives are NOT part of the key because the JIT path loads
// none: dispatchJitRun has no --classpath and buildJit never walks archives
// (everything comes from sources + the embedded stdlib, both keyed). If the
// JIT ever grows classpath support, fold each archive's digest here or the
// cache serves stale programs across dep bumps.

struct WholeProgramSlot {
    std::filesystem::path dir;
    std::filesystem::path bc() const { return dir / "program.bc"; }
    std::filesystem::path meta() const { return dir / "program.meta"; }
    std::filesystem::path dbgloc() const { return dir / "program.dbgloc"; }
};

std::string wholeProgramKey(const JitRunOptions& opts,
                            const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& sourceRoot) {
    std::ostringstream in;
    in << CAJETA_VERSION << '+' << CAJETA_GIT_HASH << '\n'
       << "mode=debug\n"
       << "debugInfo=" << (opts.debugInfo ? 1 : 0) << '\n'
       << "entry=" << opts.entryMethod << '\n';
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(sources.size());
    for (const auto& p : sources) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream bytes;
        bytes << f.rdbuf();
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(p, sourceRoot, ec);
        entries.emplace_back((ec ? p : rel).generic_string(),
                             cajeta::buildtool::sha256Hex(bytes.str()));
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& [rel, digest] : entries) in << rel << ':' << digest << '\n';
    return cajeta::buildtool::sha256Hex(in.str());
}

// Best-effort slot write; a failure never fails the launch. Files land via
// tmp + rename so a concurrent launch can't read a torn slot.
void writeWholeProgramSlot(const WholeProgramSlot& slot, const BuiltJit& built,
                           llvm::StringRef bcBytes, bool debugInfo) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(slot.dir, ec);
    if (ec) return;

    auto place = [](const fs::path& target, auto writeFn) -> bool {
        fs::path tmp = target;
        tmp += ".tmp";
        if (!writeFn(tmp)) return false;
        std::error_code renameEc;
        fs::rename(tmp, target, renameEc);
        return !renameEc;
    };

    bool ok = place(slot.bc(), [&](const fs::path& p) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(bcBytes.data(), (std::streamsize) bcBytes.size());
        return out.good();
    });
    ok = ok && place(slot.meta(), [&](const fs::path& p) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << "cajeta-jitmeta-v1\n"
            << "entry\t" << built.entryName << '\n'
            << "takesArgs\t" << (built.entryTakesArgs ? 1 : 0) << '\n'
            << "returnsInt32\t" << (built.returnsInt32 ? 1 : 0) << '\n'
            << "safepoints\t" << built.entrySafepointsEmitted << '\n';
        if (built.entryArgsABI.valid) {
            const EntryArgsABI& a = built.entryArgsABI;
            out << "strabi\t" << a.strSize << '\t' << a.offLenTag << '\t'
                << a.offAux << '\t' << a.offBase << '\t' << a.offCpLen << '\t'
                << a.vtableSymbol << '\n';
        }
        return out.good();
    });
    if (ok && debugInfo) {
        ok = place(slot.dbgloc(), [&](const fs::path& p) {
            return cajeta::dbg::writeDbgLocSidecar(
                p.string(), cajeta::dbg::globalDbgLocTable());
        });
    }
    if (!ok) {
        // Leave no half-slot behind: a partial slot would MISS anyway (meta
        // gate), but clean up so the next write starts fresh.
        fs::remove_all(slot.dir, ec);
    }
}

// Parse program.meta. Strict: any anomaly = miss.
bool loadSlotMeta(const std::filesystem::path& path, BuiltJit& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != "cajeta-jitmeta-v1") return false;
    bool haveEntry = false, haveTakes = false, haveReturns = false,
         haveSafepoints = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream fields(line);
        std::string tag;
        if (!std::getline(fields, tag, '\t')) return false;
        try {
            if (tag == "entry") {
                if (!std::getline(fields, out.entryName)) return false;
                haveEntry = !out.entryName.empty();
            } else if (tag == "takesArgs") {
                std::string v;
                if (!std::getline(fields, v)) return false;
                out.entryTakesArgs = (v == "1");
                haveTakes = true;
            } else if (tag == "returnsInt32") {
                std::string v;
                if (!std::getline(fields, v)) return false;
                out.returnsInt32 = (v == "1");
                haveReturns = true;
            } else if (tag == "safepoints") {
                std::string v;
                if (!std::getline(fields, v)) return false;
                out.entrySafepointsEmitted = std::stoi(v);
                haveSafepoints = true;
            } else if (tag == "strabi") {
                std::string sz, o1, o2, o3, o4;
                EntryArgsABI a;
                if (!std::getline(fields, sz, '\t') ||
                    !std::getline(fields, o1, '\t') ||
                    !std::getline(fields, o2, '\t') ||
                    !std::getline(fields, o3, '\t') ||
                    !std::getline(fields, o4, '\t') ||
                    !std::getline(fields, a.vtableSymbol)) return false;
                a.strSize = std::stoll(sz);
                a.offLenTag = std::stoll(o1);
                a.offAux = std::stoll(o2);
                a.offBase = std::stoll(o3);
                a.offCpLen = std::stoll(o4);
                a.valid = !a.vtableSymbol.empty();
                out.entryArgsABI = a;
            }
            // Unknown tags: ignored (forward compatibility within v1).
        } catch (...) {
            return false;
        }
    }
    return haveEntry && haveTakes && haveReturns && haveSafepoints;
}

// Build + initialize the LLJIT from a MERGED program's bitcode bytes — the
// tail of the cold pipeline, shared verbatim with the whole-program cache HIT
// path (fast-debug-launch 4.2.4). On failure sets out.errorCode, resets
// out.jit, and returns false.
bool buildLLJITFromBuffer(llvm::StringRef bcBytes, const JitRunOptions& opts,
                          BuiltJit& out) {
    // Round-trip the module through bitcode into a fresh context owned by a
    // ThreadSafeModule (the documented LLJIT path).
    auto tsCtx = std::make_unique<llvm::LLVMContext>();
    auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(bcBytes, "cajeta_jitrun");
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
        return false;
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
        return false;
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
        return false;
    }

    auto& mainDylib = out.jit->getMainJITDylib();
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        out.jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        std::cerr << "cajeta jit: process-symbol generator failed: "
                  << cajeta::jit::toString(generator.takeError()) << "\n";
        out.jit.reset();
        out.errorCode = 1;
        return false;
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
        return false;
    }
    return true;
}

// Attempt a whole-program cache hit. Any anomaly — missing/corrupt file,
// meta mismatch, bitcode that will not build — is a MISS (returns false with
// out reset), never an error: the caller falls back to the full compile.
bool tryLoadWholeProgramSlot(const WholeProgramSlot& slot,
                             const JitRunOptions& opts, BuiltJit& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(slot.bc(), ec) ||
        !fs::is_regular_file(slot.meta(), ec)) return false;
    if (!loadSlotMeta(slot.meta(), out)) return false;
    if (opts.debugInfo) {
        // The cached module's safepoints carry baked loc ids; the sidecar is
        // the only thing that can decode them, so no sidecar = no hit.
        auto& table = cajeta::dbg::globalDbgLocTable();
        table.clear();
        if (!cajeta::dbg::loadDbgLocSidecar(slot.dbgloc().string(), table)) {
            table.clear();
            return false;
        }
    }
    auto buf = llvm::MemoryBuffer::getFile(slot.bc().string());
    if (!buf) return false;
    if (!buildLLJITFromBuffer((*buf)->getBuffer(), opts, out)) {
        out.jit.reset();
        out.errorCode = 0;  // miss, not failure — the full compile runs next
        return false;
    }
    return true;
}

// Shared pipeline: compile every .cajeta under opts.sourceRoot, merge modules,
// build + initialize an LLJIT, and resolve the entry. On failure sets
// errorCode (and prints to stderr) and leaves jit null.
// buildJit() below wraps this to stamp phases.totalSeconds on every exit path.
BuiltJit buildJitImpl(const JitRunOptions& opts) {
    BuiltJit out;
    ensureJitInitialized();

    using Clock = std::chrono::steady_clock;
    // Consecutive-segment timing: endPhase() closes the current segment into
    // its slot and opens the next. The codegen quiescence loop instead
    // accumulates into the stdlib/user buckets directly (its bookkeeping
    // between method loops stays unattributed, so sum(phases) <= total).
    Clock::time_point phaseStart = Clock::now();
    auto endPhase = [&phaseStart](double& slot) {
        Clock::time_point n = Clock::now();
        slot += std::chrono::duration<double>(n - phaseStart).count();
        phaseStart = n;
    };
    auto progress = [&opts](const char* phase, const std::string& detail,
                            int current, int total) {
        if (opts.onProgress) opts.onProgress(phase, detail, current, total);
    };

    progress("collect", "", 0, 0);

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

    // Whole-program cache attempt (fast-debug-launch 4.2.4): on a hit the
    // Compiler is never constructed — the launch pays digesting + bitcode
    // load + LLJIT materialization only.
    if (!opts.cacheDir.empty()) {
        endPhase(out.phases.collectSeconds);
        WholeProgramSlot slot{fs::path(opts.cacheDir) / "jit"
                              / wholeProgramKey(opts, sourcePaths, sourceRoot)};
        if (tryLoadWholeProgramSlot(slot, opts, out)) {
            out.cacheHit = true;
            progress("jit", "cached", 0, 0);
            endPhase(out.phases.jitSeconds);
            return out;
        }
    }

    out.compiler = std::make_unique<Compiler>();
    Compiler* compiler = out.compiler.get();
    compiler->setMode(CompilerMode::Debug);
    // Statement-boundary safepoint emission (CP2). Reset the global loc table
    // so this compile's loc_ids start at 0.
    compiler->getMutableFlags().debugInfo = opts.debugInfo;
    // Keep the level in step with the bool the JIT host sets directly, so the
    // cache flag set and any level-driven codegen see the same world.
    compiler->getMutableFlags().debugInfoLevel =
        opts.debugInfo ? DebugInfo::Full : DebugInfo::Line;
    if (opts.debugInfo) cajeta::dbg::globalDbgLocTable().clear();

    fs::path archiveRoot = fs::temp_directory_path()
                         / ("cajeta_jitrun_" + sourceRoot.filename().string());
    fs::create_directories(archiveRoot, ec);

    cajeta::prescanSourceRoot(sourceRoot.string());
    endPhase(out.phases.collectSeconds);

    cajeta::CajetaModulePtr primary;
    try {
        const int totalSources = (int) sourcePaths.size();
        int currentSource = 0;
        for (auto& sourcePath : sourcePaths) {
            std::error_code relEc;
            fs::path rel = fs::relative(sourcePath, sourceRoot, relEc);
            progress("parse", (relEc ? sourcePath : rel).string(),
                     ++currentSource, totalSources);
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
    endPhase(out.phases.parseSeconds);
    progress("codegen", "", 0, 0);

    // stdlib vs user attribution: the whole parsed stdlib lives in the ONE
    // process-wide CajetaModule::stdlibModule; everything else is user code.
    // This split gates plan Unit 7 (stdlib cache slots).
    auto codegenBucket = [&out](const cajeta::CajetaModulePtr& m) -> double& {
        return m == cajeta::CajetaModule::getStdlibModule()
                   ? out.phases.codegenStdlibSeconds
                   : out.phases.codegenUserSeconds;
    };
    auto timeInto = [](double& slot, const auto& fn) {
        Clock::time_point s = Clock::now();
        fn();
        slot += std::chrono::duration<double>(Clock::now() - s).count();
    };

    // Phase 1 (signatures) + Phase 2 (bodies) to quiescence. Codegen-phase
    // diagnostics (immutable-field writes, unknown with(...) labels, …)
    // throw from generateCode — report them like parse-phase errors instead
    // of escaping to std::terminate.
    try {
        size_t prevMethodCount = 0;
        while (true) {
            size_t methodCount = 0;
            for (auto& m : compiler->getModules()) methodCount += m->getAllMethods().size();
            for (auto& m : compiler->getModules())
                timeInto(codegenBucket(m), [&] {
                    for (auto& method : m->getAllMethods()) method->getLlvmFunctionType();
                });
            for (auto& m : compiler->getModules())
                timeInto(codegenBucket(m), [&] {
                    for (auto& method : m->getAllMethods()) method->generateCode();
                });
            size_t after = 0;
            for (auto& m : compiler->getModules()) after += m->getAllMethods().size();
            if (after == methodCount && after == prevMethodCount) break;
            prevMethodCount = after;
        }

        for (auto& m : compiler->getModules())
            timeInto(codegenBucket(m), [&] {
                for (auto& [name, klass] : m->getStructures())
                    if (klass) klass->generateStaticInitializers();
            });
    } catch (cajeta::Exception& e) {
        std::cerr << "cajeta jit: [" << e.getErrorId() << "] "
                  << e.getMessage() << "\n";
        out.errorCode = 1;
        return out;
    }

    phaseStart = Clock::now();  // close the codegen segment (bucketed above)
    progress("finalize", "", 0, 0);

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

    // Drop-function backfill (shared with the AOT incremental path —
    // DropBackfill.h). Consumers can reference `__cajeta[_stack]_<type>_drop`
    // thunks whose lazy synthesis never fired (instantiations created only
    // indirectly during stdlib codegen); without this, LLJIT initialize fails
    // with `Symbols not found` on any program big enough to dangle one
    // (jit-drop-backfill spec §3, surfaced on samples/tour).
    {
        // getModules() returns by value — bind ONE copy before taking
        // iterators (a begin/end pair from two temporaries never meets).
        auto jitModules = compiler->getModules();
        std::vector<cajeta::CajetaModulePtr> scanModules(jitModules.begin(),
                                                         jitModules.end());
        cajeta::backfillDropFunctions(scanModules, scanModules);
        // Then pin every definition (incl. freshly backfilled ones) so the
        // in-process linkModules merge can't lazy-discard them.
        cajeta::pinDropFunctionDefinitions(scanModules);
    }
    endPhase(out.phases.finalizeSeconds);
    progress("merge", "", 0, 0);

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

    out.entryName = findEntryMangled(llvmModule, opts.entryMethod,
                                     &out.entryTakesArgs);
    if (out.entryName.empty()) {
        std::cerr << "cajeta jit: could not find static entry `"
                  << opts.entryMethod
                  << "` — expected `main()` or `main(String[] args)`\n";
        out.errorCode = 1;
        return out;
    }
    llvm::Function* entryLlvm = llvmModule->getFunction(out.entryName);
    out.returnsInt32 = entryLlvm && entryLlvm->getReturnType()->isIntegerTy(32);
    out.entrySafepointsEmitted = countSafepointCalls(entryLlvm);

    endPhase(out.phases.mergeSeconds);
    progress("jit", "", 0, 0);

    if (std::getenv("CAJETA_DUMP_IR")) llvmModule->print(llvm::errs(), nullptr);

    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*llvmModule, &verifyStream)) {
        std::cerr << "cajeta jit: IR verify failed: " << verifyErr << "\n";
        out.errorCode = 1;
        return out;
    }

    llvm::SmallVector<char, 0> bitcodeBuf;
    {
        llvm::raw_svector_ostream os(bitcodeBuf);
        llvm::WriteBitcodeToFile(*llvmModule, os);
    }
    if (!buildLLJITFromBuffer(
            llvm::StringRef(bitcodeBuf.data(), bitcodeBuf.size()), opts, out))
        return out;

    // ABI for makeEntryArgs, derived while the type world is still alive; it
    // rides the slot meta so a HIT launch never needs CajetaType (4.2.4).
    out.entryArgsABI = deriveEntryArgsABI(out.jit.get());

    // Populate the whole-program slot (4.2.3): the bytes are EXACTLY what the
    // LLJIT round-trip just consumed, so a hit replays this build bit-for-bit.
    if (!opts.cacheDir.empty()) {
        writeWholeProgramSlot(
            WholeProgramSlot{fs::path(opts.cacheDir) / "jit"
                             / wholeProgramKey(opts, sourcePaths, sourceRoot)},
            out, llvm::StringRef(bitcodeBuf.data(), bitcodeBuf.size()),
            opts.debugInfo);
    }
    endPhase(out.phases.jitSeconds);

    return out;
}

// Public shape of the pipeline: run the impl and stamp the wall total on
// every exit path (error returns leave a partial but consistent record).
BuiltJit buildJit(const JitRunOptions& opts) {
    using Clock = std::chrono::steady_clock;
    Clock::time_point t0 = Clock::now();
    BuiltJit out = buildJitImpl(opts);
    out.phases.totalSeconds =
        std::chrono::duration<double>(Clock::now() - t0).count();
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
    if (result) {
        result->entrySafepointsEmitted = built.entrySafepointsEmitted;
        result->phases = built.phases;
        result->cacheHit = built.cacheHit;
    }

    auto entrySym = jit->lookup(built.entryName);
    if (!entrySym) {
        std::cerr << "cajeta jit-run: entry symbol lookup failed: "
                  << cajeta::jit::toString(entrySym.takeError()) << "\n";
        return 1;
    }

    // CP2: reset the JIT module's safepoint counter so safepointsExecuted
    // measures only the entry's execution, not the global ctors above.
    callVoidSymbol(jit, "__cajeta_dbg_reset_safepoint_count");

    // A parameterized entry is invoked through a correctly-typed pointer, never
    // the no-arg one (spec 7.2.5 — the UB the old narrowing guarded against).
    void* entryArgs = nullptr;
    if (built.entryTakesArgs) {
        entryArgs = makeEntryArgs(jit, opts.programArgs, built.entryArgsABI);
        if (!entryArgs) {
            std::cerr << "cajeta jit: entry `" << opts.entryMethod
                      << "` takes String[] but the args array could not be "
                         "materialized\n";
            return 1;
        }
    }

    int rc = 0;
    void* addr = reinterpret_cast<void*>(entrySym->getValue());
    if (built.returnsInt32) {
        rc = built.entryTakesArgs
                 ? reinterpret_cast<int(*)(void*)>(addr)(entryArgs)
                 : reinterpret_cast<int(*)()>(addr)();
        std::cerr << "[jit-run] entry " << opts.entryMethod
                  << " returned " << rc << "\n";
    } else {
        if (built.entryTakesArgs) reinterpret_cast<void(*)(void*)>(addr)(entryArgs);
        else reinterpret_cast<void(*)()>(addr)();
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
        bool armExceptions,
        bool stopOnEntry,
        const std::function<void()>& beforeRun) {
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
    // Same rule for stopOnEntry: arm before the thread starts, or the program
    // races past its own first statement.
    if (stopOnEntry) impl->controller.armEntry();

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
    bool takesArgs = impl->built.entryTakesArgs;

    // Materialize args BEFORE the program thread starts, so a failure here is a
    // clean launch failure rather than a crash inside the debuggee.
    void* entryArgs = nullptr;
    if (takesArgs) {
        entryArgs = makeEntryArgs(jit, opts.programArgs,
                                  impl->built.entryArgsABI);
        if (!entryArgs) {
            if (error) *error = "entry takes String[] but args could not be built";
            {
                std::lock_guard<std::mutex> lock(g_activeMutex);
                g_activeController = nullptr;
            }
            installHandler(jit, nullptr);
            return nullptr;
        }
    }

    JitDebugSession::Impl* raw = impl.get();
    // Last thing before the program runs: anything the debuggee must observe
    // but the build must not have seen (the DAP launch environment).
    if (beforeRun) beforeRun();

    raw->thread = std::thread([raw, entryAddr, returnsInt32, takesArgs, entryArgs]() {
        if (returnsInt32) {
            raw->exitCode = takesArgs
                ? reinterpret_cast<int(*)(void*)>(entryAddr)(entryArgs)
                : reinterpret_cast<int(*)()>(entryAddr)();
        } else {
            if (takesArgs) reinterpret_cast<void(*)(void*)>(entryAddr)(entryArgs);
            else reinterpret_cast<void(*)()>(entryAddr)();
            raw->exitCode = 0;
        }
        // Shut the fiber carrier down from the program thread (it owns the
        // carrier), then mark finished.
        callVoidSymbol(raw->built.jit.get(), "__cajeta_task_shutdown");
        raw->finished.store(true);
    });

    return std::make_unique<JitDebugSession>(std::move(impl));
}

// Portable setenv. POSIX `setenv` does not exist in the Windows CRT, which
// spells it `_putenv_s` — the same split `__cajeta_env_set` already handles in
// runtime/native/cajeta_rt_lang.c. Both write the CRT environment the
// in-process JIT runtime later reads back with getenv().
static void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, /*overwrite=*/1);
#endif
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
        } else if (a.rfind("--cache-dir=", 0) == 0) {
            // fast-debug-launch 4.2.1: whole-program JIT cache root.
            opts.cacheDir = a.substr(std::string("--cache-dir=").size());
        } else if (a == "--diag-format=json") {
            // Route an uncaught throw through the runtime NDJSON emitter. The
            // in-process JIT runtime reads CAJETA_DIAG_FORMAT lazily on the first
            // uncaught throw (diagnostic-exceptions U1, 1.2.3).
            setEnvVar("CAJETA_DIAG_FORMAT", "json");
        } else if (a == "--diag-format=text") {
            setEnvVar("CAJETA_DIAG_FORMAT", "text");
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

    // fast-debug-launch 2.2.2: the same progress seam the DAP server narrates
    // through, as plain stderr lines (stdout stays the program's).
    opts.onProgress = [](const std::string& phase, const std::string& detail,
                         int current, int total) {
        if (phase == "parse" && total > 0)
            std::cerr << "[jit] parse " << current << "/" << total
                      << " " << detail << "\n";
        else if (total == 0)
            std::cerr << "[jit] " << phase << "\n";
    };

    // CAJETA_JIT_PHASES=1: dump the build-phase wall-clock breakdown to stderr
    // (fast-debug-launch 1.3.1 — the measurement that gates stdlib caching).
    if (std::getenv("CAJETA_JIT_PHASES")) {
        JitRunResult result;
        int code = runJit(opts, &result);
        const auto& ph = result.phases;
        std::cerr << "[jit-phases] collect=" << ph.collectSeconds
                  << "s parse=" << ph.parseSeconds
                  << "s codegen(stdlib)=" << ph.codegenStdlibSeconds
                  << "s codegen(user)=" << ph.codegenUserSeconds
                  << "s finalize=" << ph.finalizeSeconds
                  << "s merge=" << ph.mergeSeconds
                  << "s jit=" << ph.jitSeconds
                  << "s total=" << ph.totalSeconds << "s\n";
        return code;
    }
    return runJit(opts);
}

} // namespace cajeta::jit
