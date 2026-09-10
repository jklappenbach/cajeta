// In-process JIT host: compile every .cajeta under a source root, merge in the
// embedded runtime + stdlib, build an LLJIT, and run a chosen entry — plus the
// debug-session variant that runs the entry on a controller-wired thread.
#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/XpuTarget.h"

#include "cajeta/error/Diagnostics.h"

#include "cajeta/jit/CajetaJitErrorShim.h"
#include "cajeta/jit/CajetaSymbolIndex.h"
#include "cajeta/jit/CajetaDefinitionGenerator.h"
#include "cajeta/jit/CajetaLazyEmitter.h"
#include "cajeta/jit/CajetaJitWinSymbols.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "cajeta/compile/StdlibReuseCore.h"
#include "cajeta/compile/NativeLink.h"
#include "cajeta/buildtool/NativeProvision.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/jit/JitModulePrep.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/DebugTypeTable.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "cajeta/jit/JitCoffLinking.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
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
// function name, matching on the LLVM SIGNATURE so both `main()` and
// `main(String[])` bind. Returns "" if not found; `argc` gets the arity.
std::string findEntryMangled(llvm::Module* mod, const std::string& dottedEntry,
                             bool* takesArgs) {
    if (takesArgs) *takesArgs = false;
    // A script unit's entry is the synthesized
    // `<pkg>.<stem>::__cajeta_script_entry()`, so match by suffix.
    if (dottedEntry == "__cajeta_script_entry") {
        const std::string suffix = "::__cajeta_script_entry()";
        for (auto& fn : *mod) {
            if (fn.isDeclaration()) continue;
            std::string name = fn.getName().str();
            if (name.size() > suffix.size()
                && name.compare(name.size() - suffix.size(), suffix.size(),
                                suffix) == 0) {
                return name;
            }
        }
        return "";
    }
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


// Install the program's arguments into the ambient store behind `System.args`,
// calling INTO the JIT'd program: the runtime is linked into the program's own
// module, so the host's copy of the store is a different one. Every entry shape.
static void installAmbientArgs(llvm::orc::LLJIT* jit,
                               const std::vector<std::string>& programArgs) {
    if (!jit) return;
    auto sym = jit->lookup("__cajeta_args_install");
    if (!sym) { cajeta::jit::consumeError(sym.takeError()); return; }
    auto fn = reinterpret_cast<void (*)(int64_t, char**)>(sym->getValue());
    std::vector<char*> argv;
    argv.reserve(programArgs.size());
    for (auto& a : programArgs) argv.push_back(const_cast<char*>(a.c_str()));
    fn((int64_t) argv.size(), argv.empty() ? nullptr : argv.data());
}

// The String ABI facts makeEntryArgs needs, split from their derivation so a
// whole-program cache hit can carry them in the slot's meta sidecar.
struct EntryArgsABI {
    bool valid = false;
    int64_t strSize = 0;
    int64_t offLenTag = 0;
    int64_t offAux = 0;
    int64_t offBase = 0;
    int64_t offCpLen = 0;
    std::string vtableSymbol;  // `<canonical>#VTable`
};

// Derive the String ABI from the live type world (cold path only), taking the
// layout from the LLJIT's DataLayout. NOT from compiler->getModules(): the
// merge consumes donor modules, and reaching back through them reads freed memory.
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
    abi.vtableSymbol = klass->symbolBase() + "#VTable";
    abi.valid = true;
    return abi;
}

// Build the cajeta `String[]` for a `main(String[] args)` entry FROM THE AMBIENT
// STORE — installAmbientArgs must run first. Returns nullptr when the String
// class or its layout is unavailable, and the caller must then not invoke it.
void* makeEntryArgs(llvm::orc::LLJIT* jit, const EntryArgsABI& abi) {
    if (!jit || !abi.valid) return nullptr;

    // Take the vtable's RUNTIME address, looked up by its canonical
    // `<class>#VTable` name: klass->getVirtualTableGlobal() can point into a
    // donor module the merge already consumed.
    void* vtable = nullptr;
    if (auto sym = jit->lookup(abi.vtableSymbol)) {
        vtable = reinterpret_cast<void*>(sym->getValue());
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }

    auto sym = jit->lookup("__cajeta_args_make");
    if (!sym) { cajeta::jit::consumeError(sym.takeError()); return nullptr; }
    auto fn = reinterpret_cast<void* (*)(void*, int64_t, int64_t, int64_t,
                                         int64_t, int64_t)>(sym->getValue());
    return fn(vtable, abi.strSize, abi.offLenTag, abi.offAux,
              abi.offBase, abi.offCpLen);
}

// Static count of @__cajeta_dbg_safepoint call sites inside one function —
// read from the IR, independent of execution.
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

// Result of the shared compile-merge-build pipeline. Owns the Compiler (and so
// the source module/context) and the LLJIT for as long as the program may run.
struct BuiltJit {
    std::unique_ptr<Compiler> compiler;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::string entryName;          // cajeta-mangled IR name of the entry fn
    bool returnsInt32 = false;
    // True when the entry is `main(String[] args)`.
    bool entryTakesArgs = false;
    int entrySafepointsEmitted = 0; // static count inside the entry fn
    int errorCode = 0;              // 0 ok; else a runJit-style return code
    JitBuildPhases phases;          // wall-clock breakdown (fast-debug-launch 1.2.1)
    EntryArgsABI entryArgsABI;      // derived cold / read from slot meta on hit
    bool cacheHit = false;          // served from the whole-program slot (4.1.1)
    bool objectCacheHit = false;    // ALL modules materialized from pool (6.1.1)
    int moduleObjectsServed = 0;    // pool serves this launch (2.1.3)
    int moduleObjectsCompiled = 0;  // pool compiles this launch (2.1.3)
    // Kernel manifests the backends embedded into the lowered module this launch.
    std::vector<cajeta::xpu::KernelManifest> kernelManifests;
    // The ObjectCache wired into the LLJIT's compiler (null when cacheDir is
    // empty). The LLJIT holds a raw pointer, so it must outlive the JIT.
    std::unique_ptr<llvm::ObjectCache> objCache;
    // Heap-held: BuiltJit is returned by value and the generator holds a reference.
    std::unique_ptr<cajeta::CajetaSymbolIndex> symbolIndex =
        std::make_unique<cajeta::CajetaSymbolIndex>();
};


// Assign each module a debug-loc id range. Bases come from an append-only
// name-keyed registry under the cache, so an unchanged module keeps its base
// across edits and its -g IR stays byte-identical (per-process without a cache).
void assignDbgLocRanges(const std::string& cacheDir,
                        const std::vector<cajeta::CajetaModulePtr>& modules) {
    namespace fs = std::filesystem;
    std::map<std::string, int32_t> slots;
    int32_t next = 0;
    fs::path reg;
    if (!cacheDir.empty()) {
        reg = fs::path(cacheDir) / "jit" / "locranges.map";
        std::ifstream in(reg);
        std::string line;
        while (std::getline(in, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            int32_t slot = (int32_t) std::atoi(line.substr(0, tab).c_str());
            slots[line.substr(tab + 1)] = slot;
            next = std::max(next, slot + 1);
        }
    }
    std::ofstream append;
    if (!reg.empty()) {
        std::error_code ec;
        fs::create_directories(reg.parent_path(), ec);
        append.open(reg, std::ios::app);
    }
    for (const auto& m : modules) {
        const std::string name = m->remappedSourcePath();
        int32_t slot;
        auto it = slots.find(name);
        if (it != slots.end()) {
            slot = it->second;
        } else {
            slot = next++;
            slots[name] = slot;
            if (append.is_open()) append << slot << '\t' << name << '\n';
        }
        // slot+1: range 0 is RESERVED for the dense allocator, so a slot-0 base would
        // collide with dense ids and give one loc id two owners.
        const int64_t base =
            (int64_t)(slot + 1) * cajeta::CajetaModule::kDbgLocRange;
        if (base + cajeta::CajetaModule::kDbgLocRange
                > (int64_t) INT32_MAX) continue;  // id space exhausted: dense fallback
        m->dbgLocBase = (int32_t) base;
        m->dbgLocUsed = 0;
    }
}


// Map a "sha256:<hex>" digest to a filesystem-safe file stem: ':' is reserved
// in a Windows path component, so an ofstream on the raw digest fails there and
// takes the whole slot write with it. The digest itself is untouched.
inline std::string digestFileStem(const std::string& digest) {
    std::string s = digest;
    for (char& c : s)
        if (c == ':') c = '_';
    return s;
}

// Content-addressed object cache over <cacheDir>/jit/{bcpool,objpool}, keyed by
// the module's IR digest — serving a pooled artifact proves it matches the IR.
// The program slot is then just a manifest: entry meta + ordered digests.
class PoolObjectCache : public llvm::ObjectCache {
public:
    explicit PoolObjectCache(std::filesystem::path poolDir)
        : pool_(std::move(poolDir)) {}

    void notifyObjectCompiled(const llvm::Module* m,
                              llvm::MemoryBufferRef obj) override {
        compiled_++;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(pool_, ec);
        if (ec) return;
        fs::path target = pool_ / (digestFileStem(m->getModuleIdentifier()) + ".o");
        fs::path tmp = target;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(obj.getBufferStart(),
                      (std::streamsize) obj.getBufferSize());
            if (!out.good()) return;
        }
        fs::rename(tmp, target, ec);
    }

    std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* m) override {
        auto buf = llvm::MemoryBuffer::getFile(
            (pool_ / (digestFileStem(m->getModuleIdentifier()) + ".o")).string());
        if (!buf) return nullptr;
        served_++;
        return std::move(*buf);
    }

    int served() const { return served_.load(); }
    int compiled() const { return compiled_.load(); }

private:
    std::filesystem::path pool_;
    std::atomic<int> served_{0};
    std::atomic<int> compiled_{0};
};

// One module's bitcode + its pool key. The digest doubles as the LLVM module
// identifier so PoolObjectCache can address the object pool.
struct ModuleBC {
    std::string digest;   // "sha256:<hex>" of `bytes`
    std::string bytes;
};

struct WholeProgramSlot {
    std::filesystem::path dir;   // <cacheDir>/jit/<program-key>/
    std::filesystem::path meta() const { return dir / "program.meta"; }
    std::filesystem::path dbgloc() const { return dir / "program.dbgloc"; }
    std::filesystem::path typeinfo() const { return dir / "program.typeinfo"; }
    std::filesystem::path bcPool() const {
        return dir.parent_path() / "bcpool";
    }
    std::filesystem::path objPool() const {
        return dir.parent_path() / "objpool";
    }
};

// Cache key for the whole-program slot: compiler identity and flags, the entry,
// the dependency archives and the source digests. Any difference must miss.
std::string wholeProgramKey(const JitRunOptions& opts,
                            const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& sourceRoot) {
    std::ostringstream in;
    in << CAJETA_VERSION << '+' << CAJETA_GIT_HASH << '\n';
    // Fold the binary's own identity in: CAJETA_GIT_HASH bakes at CMake configure
    // time and goes stale across dev rebuilds.
    {
        std::error_code ec;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto size = std::filesystem::file_size(exe, ec);
            auto mtime = std::filesystem::last_write_time(exe, ec);
            if (!ec)
                in << "bin=" << (unsigned long long) size << ':'
                   << (long long) mtime.time_since_epoch().count() << '\n';
        }
    }
    in << "mode=debug\n"
       << "debugInfo=" << (opts.debugInfo ? 1 : 0) << '\n'
       << "entry=" << opts.entryMethod << '\n';
    // Dependency archives are part of the compiled world: content-hash each,
    // sorted, so wire order is irrelevant.
    {
        std::vector<std::string> deps;
        deps.reserve(opts.classpath.size());
        for (const auto& cp : opts.classpath) {
            std::ifstream f(cp, std::ios::binary);
            std::stringstream bytes;
            bytes << f.rdbuf();
            deps.push_back(cajeta::buildtool::sha256Hex(bytes.str()));
        }
        std::sort(deps.begin(), deps.end());
        for (const auto& d : deps) in << "dep=" << d << '\n';
    }
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

// Persist the program manifest + any pool bitcodes not already present.
// Best-effort: a failed write costs the next launch speed, nothing else.
void writeWholeProgramSlot(const WholeProgramSlot& slot, const BuiltJit& built,
                           const std::vector<ModuleBC>& modules,
                           bool debugInfo) {
    namespace fs = std::filesystem;
    std::error_code ec, ec2;
    fs::create_directories(slot.dir, ec);
    fs::create_directories(slot.bcPool(), ec2);
    if (ec || ec2) return;   // check BOTH: only ec2 masked a slot.dir failure

    auto place = [](const fs::path& target, auto writeFn) -> bool {
        fs::path tmp = target;
        tmp += ".tmp";
        if (!writeFn(tmp)) return false;
        std::error_code renameEc;
        fs::rename(tmp, target, renameEc);
        return !renameEc;
    };

    bool ok = true;
    for (const auto& m : modules) {
        fs::path bc = slot.bcPool() / (digestFileStem(m.digest) + ".bc");
        if (fs::exists(bc, ec)) continue;   // content-addressed: idempotent
        ok = place(bc, [&](const fs::path& p) {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(m.bytes.data(), (std::streamsize) m.bytes.size());
            return out.good();
        }) && ok;
    }
    ok = ok && place(slot.meta(), [&](const fs::path& p) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << "cajeta-jitmeta-v2\n"
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
        for (const auto& m : modules) out << "module\t" << m.digest << '\n';
        return out.good();
    });
    if (ok && debugInfo) {
        ok = place(slot.dbgloc(), [&](const fs::path& p) {
            return cajeta::dbg::writeDbgLocSidecar(
                p.string(), cajeta::dbg::globalDbgLocTable());
        });
        ok = ok && place(slot.typeinfo(), [&](const fs::path& p) {
            return cajeta::dbg::writeTypeSidecar(
                p.string(), cajeta::dbg::globalDebugTypeTable());
        });
    }
    if (!ok) {
        fs::remove(slot.meta(), ec);
        fs::remove(slot.dbgloc(), ec);
        fs::remove(slot.typeinfo(), ec);
    }
}

// Parse program.meta (v2). Strict: any anomaly = miss.
bool loadSlotMeta(const std::filesystem::path& path, BuiltJit& out,
                  std::vector<std::string>& moduleDigests) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != "cajeta-jitmeta-v2") return false;
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
            } else if (tag == "module") {
                std::string d;
                if (!std::getline(fields, d) || d.empty()) return false;
                moduleDigests.push_back(d);
            }
        } catch (...) {
            return false;
        }
    }
    return haveEntry && haveTakes && haveReturns && haveSafepoints
        && !moduleDigests.empty();
}

// Build + initialize the LLJIT from per-module bitcodes — the tail of the cold
// pipeline, shared verbatim with the slot-hit path. Each module parses into its
// own context. On failure sets out.errorCode, resets out.jit and returns false.
bool buildLLJITFromModules(const std::vector<ModuleBC>& modules,
                           const JitRunOptions& opts,
                           PoolObjectCache* objCache, BuiltJit& out) {
    using SplitClock = std::chrono::steady_clock;
    std::vector<llvm::orc::ThreadSafeModule> tsms;
    tsms.reserve(modules.size());
    std::set<std::string> liveNativeLibs;
    for (const auto& mbc : modules) {
        auto tsCtx = std::make_unique<llvm::LLVMContext>();
        auto memBuffer =
            llvm::MemoryBuffer::getMemBufferCopy(mbc.bytes, mbc.digest);
        llvm::orc::ThreadSafeContext tsContext(std::move(tsCtx));
        SplitClock::time_point reparseStart = SplitClock::now();
#if LLVM_VERSION_MAJOR >= 21
        auto parsed = tsContext.withContextDo([&](llvm::LLVMContext* ctx) {
            return llvm::parseBitcodeFile(memBuffer->getMemBufferRef(), *ctx);
        });
#else
        auto parsed = llvm::parseBitcodeFile(memBuffer->getMemBufferRef(),
                                             *tsContext.getContext());
#endif
        out.phases.jitReparseSeconds +=
            std::chrono::duration<double>(SplitClock::now() - reparseStart)
                .count();
        if (!parsed) {
            {
                std::ostringstream m; m << "cajeta jit: bitcode reparse failed: "
                      << cajeta::jit::toString(parsed.takeError()) << "\n";
                cajeta::logLine("error", m.str());
            }
            out.errorCode = 1;
            return false;
        }
        (*parsed)->setModuleIdentifier(mbc.digest);
        std::set<std::string> libs = cajeta::collectLiveNativeLibs(**parsed);
        liveNativeLibs.insert(libs.begin(), libs.end());
        tsms.emplace_back(std::move(*parsed), std::move(tsContext));
    }

    llvm::orc::LLJITBuilder builder;
    // COFF: RuntimeDyld's default object layer aborts the process on
    // IMAGE_REL_AMD64_ADDR32NB — see JitCoffLinking.h for the full story.
    cajeta::jit::applyCoffJitLink(builder);
    if (objCache) {
        builder.setCompileFunctionCreator(
            [objCache](llvm::orc::JITTargetMachineBuilder jtmb)
                -> llvm::Expected<
                    std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
                auto tm = jtmb.createTargetMachine();
                if (!tm) return tm.takeError();
                return std::make_unique<llvm::orc::TMOwningSimpleCompiler>(
                    std::move(*tm), objCache);
            });
    }
    auto jitOrErr = builder.create();
    if (!jitOrErr) {
        {
            std::ostringstream m; m << "cajeta jit: LLJIT create failed: "
                  << cajeta::jit::toString(jitOrErr.takeError()) << "\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 1;
        return false;
    }
    out.jit = std::move(*jitOrErr);
    cajeta::jit::installObjectDump(*out.jit);

    if (opts.debugInfo || std::getenv("CAJETA_JIT_GDB")) {
        if (auto err = llvm::orc::enableDebuggerSupport(*out.jit)) {
            {
                std::ostringstream m; m << "cajeta jit: GDB symbolization unavailable: "
                      << cajeta::jit::toString(std::move(err)) << "\n";
                cajeta::logLine("warn", m.str());
            }
        }
    }

    for (auto& tsm : tsms) {
        if (auto err = out.jit->addIRModule(std::move(tsm))) {
            {
                std::ostringstream m; m << "cajeta jit: addIRModule failed: "
                      << cajeta::jit::toString(std::move(err)) << "\n";
                cajeta::logLine("error", m.str());
            }
            out.jit.reset();
            out.errorCode = 1;
            return false;
        }
    }

    auto& mainDylib = out.jit->getMainJITDylib();
    // Added FIRST, so a host library sharing a method's name can never shadow a
    // body we can generate. Dark until lazy mode is on.
    {
        llvm::orc::LLJIT* jptr = out.jit.get();
        mainDylib.addGenerator(std::make_unique<cajeta::CajetaDefinitionGenerator>(
            *out.symbolIndex,
            [jptr](llvm::orc::ThreadSafeModule tsm,
                   llvm::orc::JITDylib& jd) -> llvm::Error {
                return jptr->addIRModule(jd, std::move(tsm));
            }));
    }
    auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        out.jit->getDataLayout().getGlobalPrefix());
    if (!generator) {
        {
            std::ostringstream m; m << "cajeta jit: process-symbol generator failed: "
                  << cajeta::jit::toString(generator.takeError()) << "\n";
            cajeta::logLine("error", m.str());
        }
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

    if (!liveNativeLibs.empty()) {
        auto& execSession = out.jit->getExecutionSession();
        (void) execSession;
        const char prefix = out.jit->getDataLayout().getGlobalPrefix();
        const std::string platform = cajeta::hostNativePlatform();
        const std::vector<std::string> dirs = cajeta::nativeLinkSearchDirs();
        for (const auto& lib : liveNativeLibs) {
            auto art = cajeta::findNativeJitArtifact(lib, platform, dirs);
            if (!art) continue;  // absent -> lazy lookup fails loud only if needed
            if (art->isStatic) {
                auto gen = llvm::orc::StaticLibraryDefinitionGenerator::Load(
                    out.jit->getObjLinkingLayer(), art->path.c_str());
                if (gen) mainDylib.addGenerator(std::move(*gen));
                else {
                    std::ostringstream m; m << "cajeta jit: native lib '" << lib
                               << "' load failed: "
                               << cajeta::jit::toString(gen.takeError()) << "\n";
                    cajeta::logLine("warn", m.str());
                }
            } else {
                auto gen = llvm::orc::DynamicLibrarySearchGenerator::Load(
                    art->path.c_str(), prefix);
                if (gen) mainDylib.addGenerator(std::move(*gen));
                else {
                    std::ostringstream m; m << "cajeta jit: native lib '" << lib
                               << "' load failed: "
                               << cajeta::jit::toString(gen.takeError()) << "\n";
                    cajeta::logLine("warn", m.str());
                }
            }
        }
    }

    SplitClock::time_point matStart = SplitClock::now();
    if (auto err = out.jit->initialize(out.jit->getMainJITDylib())) {
        {
            std::ostringstream m; m << "cajeta jit: LLJIT initialize failed: "
                  << cajeta::jit::toString(std::move(err)) << "\n";
            cajeta::logLine("error", m.str());
        }
        out.jit.reset();
        out.errorCode = 1;
        return false;
    }
    out.phases.jitMaterializeSeconds +=
        std::chrono::duration<double>(SplitClock::now() - matStart).count();
    return true;
}

// Attempt a whole-program manifest hit. Any anomaly — missing or corrupt meta,
// pool bitcode, sidecar, or a module set that will not build — is a MISS (out
// reset), never an error: the caller falls back to the full compile.
bool tryLoadWholeProgramSlot(const WholeProgramSlot& slot,
                             const JitRunOptions& opts,
                             PoolObjectCache* objCache, BuiltJit& out) {
    namespace fs = std::filesystem;
    std::vector<std::string> digests;
    if (!loadSlotMeta(slot.meta(), out, digests)) return false;
    if (opts.debugInfo) {
        auto& table = cajeta::dbg::globalDbgLocTable();
        table.clear();
        if (!cajeta::dbg::loadDbgLocSidecar(slot.dbgloc().string(), table)) {
            table.clear();
            return false;
        }
        if (!cajeta::dbg::loadTypeSidecar(slot.typeinfo().string(),
                                          cajeta::dbg::globalDebugTypeTable()))
            return false;
    }
    std::vector<ModuleBC> modules;
    modules.reserve(digests.size());
    for (const auto& d : digests) {
        auto buf = llvm::MemoryBuffer::getFile(
            (slot.bcPool() / (digestFileStem(d) + ".bc")).string());
        if (!buf) return false;
        std::string bytes((*buf)->getBufferStart(), (*buf)->getBufferSize());
        if (cajeta::buildtool::sha256Hex(bytes) != d) return false;
        modules.push_back(ModuleBC{d, std::move(bytes)});
    }
    if (!buildLLJITFromModules(modules, opts, objCache, out)) {
        out.jit.reset();
        out.errorCode = 0;  // miss, not failure — the full compile runs next
        return false;
    }
    return true;
}

// Shared pipeline: compile every .cajeta under opts.sourceRoot, merge modules,
// build + initialize an LLJIT, and resolve the entry. On failure sets errorCode
// and leaves jit null; buildJit() wraps this to stamp phases.totalSeconds.
BuiltJit buildJitImpl(const JitRunOptions& opts) {
    BuiltJit out;
    ensureJitInitialized();

    using Clock = std::chrono::steady_clock;
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
        {
            std::ostringstream m; m << "cajeta jit: source root is not a directory: "
                  << opts.sourceRoot << "\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 2;
        return out;
    }

    std::vector<fs::path> sourcePaths;
    if (!opts.scriptFile.empty()) {
        fs::path sf = fs::absolute(opts.scriptFile, ec);
        if (ec || !fs::is_regular_file(sf)) {
            std::ostringstream m;
            m << "cajeta run: script not found: " << opts.scriptFile << "\n";
            cajeta::logLine("error", m.str());
            out.errorCode = 2;
            return out;
        }
        sourcePaths.push_back(sf);
    } else {
        sourcePaths = collectSources(sourceRoot);
    }
    if (sourcePaths.empty()) {
        {
            std::ostringstream m; m << "cajeta jit: no .cajeta files under " << sourceRoot << "\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 2;
        return out;
    }

    WholeProgramSlot slot;
    std::unique_ptr<PoolObjectCache> objCache;
    size_t moduleCount = 0;
    auto recordPoolCounters = [&](size_t count) {
        moduleCount = count;
        if (!objCache) return;
        out.moduleObjectsServed = objCache->served();
        out.moduleObjectsCompiled = objCache->compiled();
        out.objectCacheHit =
            count > 0 && objCache->served() == (int) count;
    };
    if (!opts.cacheDir.empty()) {
        endPhase(out.phases.collectSeconds);
        slot.dir = fs::path(opts.cacheDir) / "jit"
                 / digestFileStem(wholeProgramKey(opts, sourcePaths, sourceRoot));
        objCache = std::make_unique<PoolObjectCache>(slot.objPool());
        std::vector<std::string> hitDigests;
        if (tryLoadWholeProgramSlot(slot, opts, objCache.get(), out)) {
            out.cacheHit = true;
            recordPoolCounters((size_t) (objCache->served()
                                         + objCache->compiled()));
            out.objCache = std::move(objCache);
            progress("jit", "cached", 0, 0);
            endPhase(out.phases.jitSeconds);
            return out;
        }
    }

    // Resident world: the primed stdlib front-end survives between sessions, so
    // this rebuild restores the baseline and pays for USER sources only.
    struct SharedContextGuard {
        bool armed = false;
        ~SharedContextGuard() {
            if (!armed) return;
            Compiler::setSharedContext(nullptr);
            cajeta::CajetaModule::setReuseEmitModule(nullptr);
        }
    } sharedCtxGuard;
    bool residentActive = false;
    cajeta::CajetaModulePtr residentStdlib;
    static thread_local cajeta::dbg::DbgLocTable residentLayerLocs;
    static thread_local std::vector<std::string> residentLayerTypeRoots;
    if (opts.resident) {
        try {
            auto& core = cajeta::StdlibReuseCore::instance();
            core.ensurePrimed();
            // Release the prior build's transient struct names ONLY when that build shared
            // this world: a non-resident session took its llvm::Types with it, so reading
            // the registries that still name them is a use-after-free.
            if (cajeta::CajetaModule::getStdlibModule() == core.getStdlibModule())
                cajeta::CajetaType::releaseThrownTransientStructNames();
            core.restoreBaseline();
            core.ensureCodegenLayer([](Compiler& prime) {
                prime.setMode(CompilerMode::Debug);
                prime.getMutableFlags().debugInfo = true;
                prime.getMutableFlags().debugInfoLevel = DebugInfo::Full;
                // Modules snapshot their flags at creation, before the lines above, and
                // debug-frame emission gates on module->getFlags().debugInfo.
                for (auto& m : prime.getModules()) {
                    CompilerFlags f = m->getFlags();
                    f.debugInfo = true;
                    f.debugInfoLevel = DebugInfo::Full;
                    m->setFlags(f);
                }
                cajeta::dbg::globalDbgLocTable().clear();
                cajeta::dbg::globalDebugTypeTable().clear();
                size_t prev = 0;
                while (true) {
                    size_t count = 0;
                    for (auto& m : prime.getModules())
                        count += m->getAllMethods().size();
                    for (auto& m : prime.getModules())
                        for (auto& method : m->getAllMethods())
                            method->getLlvmFunctionType();
                    for (auto& m : prime.getModules())
                        m->completePendingInterfaceVTables();
                    for (auto& m : prime.getModules())
                        for (auto& method : m->getAllMethods())
                            method->generateCode();
                    size_t after = 0;
                    for (auto& m : prime.getModules())
                        after += m->getAllMethods().size();
                    if (after == count && after == prev) break;
                    prev = after;
                }
                for (auto& m : prime.getModules())
                    for (auto& [name, klass] : m->getStructures())
                        if (klass) klass->generateStaticInitializers();
                residentLayerLocs.clear();
                const auto& t = cajeta::dbg::globalDbgLocTable();
                for (int32_t id : t.assignedIds())
                    residentLayerLocs.setAt(id, t.at(id));
                residentLayerTypeRoots =
                    cajeta::dbg::globalDebugTypeTable().roots();
            });
            residentStdlib = core.getStdlibModule();
            Compiler::setSharedContext(core.context());
            sharedCtxGuard.armed = true;
            residentActive = true;
            progress("parse", "resident-world", 0, 0);
        } catch (...) {
            Compiler::setSharedContext(nullptr);
            residentStdlib.reset();
            {
                std::ostringstream m; m << "cajeta jit: resident reuse unavailable, "
                         "falling back to a full build\n";
                cajeta::logLine("warn", m.str());
            }
        }
    }

    out.compiler = std::make_unique<Compiler>();
    Compiler* compiler = out.compiler.get();
    compiler->setMode(CompilerMode::Debug);
    if (!opts.scriptFile.empty()) {
        compiler->setSessionState(nullptr, opts.scriptFile);
    }
    compiler->getMutableFlags().diagFormat =
        cajeta::jsonProgressEnabled() ? DiagFormat::Json : DiagFormat::Text;
    compiler->getMutableFlags().debugInfo = opts.debugInfo;
    compiler->getMutableFlags().debugInfoLevel =
        opts.debugInfo ? DebugInfo::Full : DebugInfo::Line;
    if (opts.debugInfo) {
        cajeta::dbg::globalDbgLocTable().clear();
        cajeta::dbg::globalDebugTypeTable().clear();
        if (residentActive) {
            auto& t = cajeta::dbg::globalDbgLocTable();
            for (int32_t id : residentLayerLocs.assignedIds())
                t.setAt(id, residentLayerLocs.at(id));
            for (const auto& root : residentLayerTypeRoots)
                cajeta::dbg::globalDebugTypeTable().addRoot(root);
        }
    }

    fs::path archiveRoot = fs::temp_directory_path()
                         / ("cajeta_jitrun_" + sourceRoot.filename().string());
    fs::create_directories(archiveRoot, ec);

    cajeta::prescanSourceRoot(sourceRoot.string(),
                              cajeta::jsonProgressEnabled());
    endPhase(out.phases.collectSeconds);

    // The decorated lazy-import hook writes a thread_local accumulator, NOT
    // out.phases: it outlives this call and a captured &out would dangle.
    {
        Clock::time_point s = Clock::now();
        compiler->ensureStdlibModule();
        out.phases.parseStdlibSeconds +=
            std::chrono::duration<double>(Clock::now() - s).count();
    }
    // Dependency archives are ingested after the stdlib parse and BEFORE any user
    // source, so user imports resolve against classpath classes during their parse.
    if (!opts.classpath.empty()) {
        for (const auto& cp : opts.classpath) compiler->addClasspath(cp);
        try {
            compiler->ingestClasspath();
            compiler->linkClasspathModules();
        } catch (cajeta::Exception& e) {
            cajeta::logLine("error",
                std::string("cajeta jit: [") + e.getErrorId()
                + "] classpath ingest failed: " + e.getMessage() + "\n");
            out.errorCode = 1;
            return out;
        } catch (std::exception& e) {
            cajeta::logLine("error",
                std::string("cajeta jit: classpath ingest failed: ")
                + e.what() + "\n");
            out.errorCode = 1;
            return out;
        }
    }

    static thread_local double stdlibHookSeconds;
    stdlibHookSeconds = 0;
    if (auto inner = cajeta::CajetaModule::stdlibImportHook) {
        cajeta::CajetaModule::stdlibImportHook =
            [inner](const std::string& pkg) {
                Clock::time_point s = Clock::now();
                inner(pkg);
                stdlibHookSeconds +=
                    std::chrono::duration<double>(Clock::now() - s).count();
            };
    }

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
            if (!primary) {
                primary = m;
                // Resident reuse MUST be set before the first user compile: stdlib-template
                // instantiation over a user type fires during parse, and one homed in the
                // persistent stdlib module is a cross-module reference no merge can legalize.
                if (residentActive)
                    cajeta::CajetaModule::setReuseEmitModule(primary);
            }
            compiler->compile(m);
        }
    } catch (cajeta::Exception& e) {
        {
            std::ostringstream m; m << "cajeta jit: [" << e.getErrorId() << "] "
                  << e.getMessage() << "\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 1;
        return out;
    }
    if (!primary) {
        {
            std::ostringstream m; m << "cajeta jit: no modules compiled\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 1;
        return out;
    }


    cajeta::CajetaModule::setActiveProfile(
        opts.profile.empty() ? "prod" : opts.profile);
    try {
        cajeta::CajetaModule::validatePlaceholders();
        cajeta::CajetaModule::resolveAdviceMatches();
        cajeta::CajetaModule::resolveDependencyGraph();
    } catch (cajeta::Exception& e) {
        {
            std::ostringstream m; m << "cajeta jit: [" << e.getErrorId() << "] " << e.getMessage() << "\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 1;
        return out;
    } catch (std::exception& e) {
        cajeta::logLine("error",
            std::string("cajeta jit: dependency resolution failed: ")
            + e.what() + "\n");
        out.errorCode = 1;
        return out;
    }
    out.phases.parseStdlibSeconds += stdlibHookSeconds;
    if (opts.debugInfo) {
        auto mods = compiler->getModules();   // ONE copy (getModules-by-value)
        std::vector<cajeta::CajetaModulePtr> modList(mods.begin(), mods.end());
        if (residentStdlib) modList.push_back(residentStdlib);
        assignDbgLocRanges(opts.cacheDir, modList);
    }
    endPhase(out.phases.parseSeconds);
    progress("codegen", "", 0, 0);

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

    try {
        // Resident mode: this session's pure-stdlib-typed instantiations home their
        // methods in the persistent module, which is not in this Compiler's list.
        auto codegenMods = [&]() {
            auto mods = compiler->getModules();
            std::vector<cajeta::CajetaModulePtr> v(mods.begin(), mods.end());
            if (residentStdlib) v.push_back(residentStdlib);
            return v;
        };
        {
            auto ixT0 = std::chrono::steady_clock::now();
            out.symbolIndex->build(codegenMods());
            if (std::getenv("CAJETA_PRIME_TIMING")) {
                auto ixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ixT0).count();
                std::fprintf(stderr,
                    "[jit] symbol index: %zu entries in %lld ms\n",
                    out.symbolIndex->size(), (long long) ixMs);
            }
        }

        size_t prevMethodCount = 0;
        while (true) {
            auto mods = codegenMods();
            size_t methodCount = 0;
            for (auto& m : mods) methodCount += m->getAllMethods().size();
            for (auto& m : mods)
                timeInto(codegenBucket(m), [&] {
                    for (auto& method : m->getAllMethods()) method->getLlvmFunctionType();
                });
            for (auto& m : mods)
                m->completePendingInterfaceVTables();
            for (auto& m : mods)
                timeInto(codegenBucket(m), [&] {
                    for (auto& method : m->getAllMethods()) method->generateCode();
                });
            size_t after = 0;
            for (auto& m : codegenMods()) after += m->getAllMethods().size();
            if (after == methodCount && after == prevMethodCount) break;
            prevMethodCount = after;
        }

        for (auto& m : codegenMods())
            timeInto(codegenBucket(m), [&] {
                for (auto& [name, klass] : m->getStructures())
                    if (klass) klass->generateStaticInitializers();
            });
    } catch (cajeta::Exception& e) {
        if (cajeta::jsonProgressEnabled()) {
            cajeta::emitJsonDiagnostic("error", e.getErrorId(),
                                       e.getMessage(), e.getFile(),
                                       e.getLine(), e.getColumn());
        } else {
            std::ostringstream m;
            m << "cajeta jit: ";
            if (e.hasLocation()) {
                m << e.getFile() << ":" << e.getLine() << ": ";
            }
            m << "[" << e.getErrorId() << "] " << e.getMessage() << "\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 1;
        return out;
    }

    phaseStart = Clock::now();  // close the codegen segment (bucketed above)
    progress("finalize", "", 0, 0);

    // Fill the reflective invoke-adapter bodies and register #ClassObjects now that
    // every method's LLVM function exists, or their thunks stay undefined.
    for (auto& [key, type] : cajeta::CajetaType::getCanonicalMap()) {
        if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(type)) {
            klass->emitReflectInvokeBody();
            klass->emitReflectNewBody();
            klass->finalizeClassObject();
        }
    }

    // Drop-function backfill: a consumer can reference a `__cajeta*_drop` thunk
    // whose lazy synthesis never fired, and LLJIT initialize then fails.
    {
        // getModules() returns by value — bind ONE copy before taking iterators.
        auto jitModules = compiler->getModules();
        std::vector<cajeta::CajetaModulePtr> scanModules(jitModules.begin(),
                                                         jitModules.end());
        if (residentStdlib) scanModules.push_back(residentStdlib);
        cajeta::backfillDropFunctions(scanModules, scanModules);
        cajeta::pinDropFunctionDefinitions(scanModules);
    }
    endPhase(out.phases.finalizeSeconds);
    progress("merge", "", 0, 0);

    // Per-module delivery: no destructive merge. Each module is verified and
    // serialized on its own, and the digests key the content-addressed pools.
    for (auto& m : compiler->getModules()) {
        out.entryName = findEntryMangled(m->getLlvmModule(), opts.entryMethod,
                                         &out.entryTakesArgs);
        if (!out.entryName.empty()) {
            llvm::Function* entryLlvm =
                m->getLlvmModule()->getFunction(out.entryName);
            out.returnsInt32 =
                entryLlvm && entryLlvm->getReturnType()->isIntegerTy(32);
            out.entrySafepointsEmitted = countSafepointCalls(entryLlvm);
            break;
        }
    }
    if (out.entryName.empty()) {
        {
            std::ostringstream m; m << "cajeta jit: could not find static entry `"
                  << opts.entryMethod
                  << "` — expected `main()` or `main(String[] args)`\n";
            cajeta::logLine("error", m.str());
        }
        out.errorCode = 1;
        return out;
    }

    endPhase(out.phases.mergeSeconds);
    progress("jit", "", 0, 0);

    const bool dumpIr = std::getenv("CAJETA_DUMP_IR") != nullptr;
    std::vector<ModuleBC> moduleBCs;
    {
        auto jitModules = compiler->getModules();  // ONE copy (by-value)
        std::vector<cajeta::CajetaModulePtr> mods(jitModules.begin(),
                                                  jitModules.end());
        if (residentStdlib) mods.push_back(residentStdlib);  // delivery too
        moduleBCs.reserve(mods.size());
        // Without XPU kernel registration a JIT'd kernel.launch finds no backend,
        // silently no-ops, and leaves every output buffer zero. Gated on --xpu-backend.
        if (!opts.xpuBackends.empty() && primary) {
            std::vector<cajeta::MethodPtr> kernels;
            for (auto& m : mods) {
                if (!m) continue;
                for (auto& method : m->getAllMethods())
                    if (method && cajeta::xpu::isKernel(*method))
                        kernels.push_back(method);
            }
            if (!kernels.empty()) {
                llvm::Module* pm = primary->getLlvmModule();
                cajeta::xpu::emitBackendManifest(opts.xpuBackends, *pm);
                for (cajeta::xpu::Backend be : opts.xpuBackends) {
                    std::string arch =
                        !opts.xpuArch.empty()              ? opts.xpuArch
                      : be == cajeta::xpu::Backend::Nvptx  ? "sm_89"
                      : be == cajeta::xpu::Backend::Amdgpu ? "gfx1151"
                      : be == cajeta::xpu::Backend::Spirv  ? "vulkan1.3"
                      :                                      "";
                    cajeta::xpu::emitKernelRegistration(be, kernels, *pm, arch, {},
                                                        &out.kernelManifests);
                }
            }
        }

        // Legalize EVERY module before verifying ANY: a use from module B into module A
        // trips A's verifier even though the fix lives in B.
        for (auto& m : mods) {
            legalizeCrossModuleRefs(m->getLlvmModule());
            demoteInstantiationsToWeakODR(m->getLlvmModule());
        }
        for (auto& m : mods) {
            llvm::Module* lm = m->getLlvmModule();
            if (dumpIr) lm->print(llvm::errs(), nullptr);
            std::string verifyErr;
            llvm::raw_string_ostream verifyStream(verifyErr);
            if (llvm::verifyModule(*lm, &verifyStream)) {
                {
                    std::ostringstream m; m << "cajeta jit: IR verify failed ("
                          << lm->getModuleIdentifier() << "): " << verifyErr
                          << "\n";
                    cajeta::logLine("warn", m.str());
                }
                out.errorCode = 1;
                return out;
            }
            Clock::time_point s = Clock::now();
            llvm::SmallVector<char, 0> buf;
            {
                llvm::raw_svector_ostream os(buf);
                llvm::WriteBitcodeToFile(*lm, os);
            }
            out.phases.jitSerializeSeconds +=
                std::chrono::duration<double>(Clock::now() - s).count();
            std::string bytes(buf.data(), buf.size());
            std::string digest = cajeta::buildtool::sha256Hex(bytes);
            moduleBCs.push_back(ModuleBC{std::move(digest), std::move(bytes)});
        }
    }

    if (!buildLLJITFromModules(moduleBCs, opts, objCache.get(), out))
        return out;
    recordPoolCounters(moduleBCs.size());

    out.entryArgsABI = deriveEntryArgsABI(out.jit.get());

    if (opts.debugInfo && out.jit) {
        cajeta::dbg::globalDebugTypeTable().buildFromTypeWorld(
            out.jit->getDataLayout());
    }

    if (!opts.cacheDir.empty()) {
        writeWholeProgramSlot(slot, out, moduleBCs, opts.debugInfo);
    }
    out.objCache = std::move(objCache);
    endPhase(out.phases.jitSeconds);

    return out;
}

// Public shape of the pipeline: run the impl and stamp the wall total on every
// exit path, so an error return still leaves a consistent record.
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
// The JIT'd code calls the installed handler through a plain function pointer, so
// a file-local C function suffices; only one debug session runs in-process.
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

// Forwards a throw at the runtime chokepoint to the active session's
// controller. Always installed: onException() no-ops unless armed.
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

    cajeta::buildtool::setNativePhase(cajeta::buildtool::NativePhase::Execution);

    llvm::orc::LLJIT* jit = built.jit.get();
    if (result) {
        result->kernelManifests = built.kernelManifests;
        result->entrySafepointsEmitted = built.entrySafepointsEmitted;
        result->phases = built.phases;
        result->cacheHit = built.cacheHit;
        result->objectCacheHit = built.objectCacheHit;
        result->moduleObjectsServed = built.moduleObjectsServed;
        result->moduleObjectsCompiled = built.moduleObjectsCompiled;
    }

    auto entrySym = jit->lookup(built.entryName);
    if (!entrySym) {
        {
            std::ostringstream m; m << "cajeta jit-run: entry symbol lookup failed: "
                  << cajeta::jit::toString(entrySym.takeError()) << "\n";
            cajeta::logLine("error", m.str());
        }
        return 1;
    }

    callVoidSymbol(jit, "__cajeta_dbg_reset_safepoint_count");

    installAmbientArgs(jit, opts.programArgs);

    // A parameterized entry is invoked through a correctly-typed pointer.
    void* entryArgs = nullptr;
    if (built.entryTakesArgs) {
        entryArgs = makeEntryArgs(jit, built.entryArgsABI);
        if (!entryArgs) {
            {
                std::ostringstream m; m << "cajeta jit: entry `" << opts.entryMethod
                      << "` takes String[] but the args array could not be "
                         "materialized\n";
                cajeta::logLine("error", m.str());
            }
            return 1;
        }
    }

    int rc = 0;
    void* addr = reinterpret_cast<void*>(entrySym->getValue());
    if (built.returnsInt32) {
        rc = built.entryTakesArgs
                 ? reinterpret_cast<int(*)(void*)>(addr)(entryArgs)
                 : reinterpret_cast<int(*)()>(addr)();
        cajeta::logLine("debug", "[jit-run] entry " + opts.entryMethod
                                 + " returned " + std::to_string(rc) + "\n");
    } else {
        if (built.entryTakesArgs) reinterpret_cast<void(*)(void*)>(addr)(entryArgs);
        else reinterpret_cast<void(*)()>(addr)();
        cajeta::logLine("debug", "[jit-run] entry " + opts.entryMethod
                                 + " completed (void)\n");
    }

    if (result) {
        if (auto cs = jit->lookup("__cajeta_dbg_safepoint_count")) {
            auto f = reinterpret_cast<long(*)()>(cs->getValue());
            result->safepointsExecuted = f ? f() : 0;
        } else {
            cajeta::jit::consumeError(cs.takeError());
        }
    }

    if (!opts.scriptFile.empty()) {
        callVoidSymbol(jit, "__cajeta_session_drop_all");
    }

    callVoidSymbol(jit, "__cajeta_task_shutdown");
    return rc;
}

// --- JitDebugSession ---------------------------------------------------------

struct JitDebugSession::Impl {
    BuiltJit built;
    cajeta::dbg::ResolvedTypeSymbols resolvedTypeSymbols;
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

const cajeta::dbg::ResolvedTypeSymbols&
JitDebugSession::resolvedTypeSymbols() const {
    return impl_->resolvedTypeSymbols;
}

const llvm::DataLayout& JitDebugSession::dataLayout() const {
    return impl_->built.jit->getDataLayout();
}

std::vector<JitDebugSession::FiberSnapshot> JitDebugSession::liveFibers() {
    std::vector<FiberSnapshot> out;
    llvm::orc::LLJIT* jit = impl_->built.jit.get();
    if (!jit) return out;

    auto resolve = [jit](const char* name) -> void* {
        if (auto sym = jit->lookup(name)) {
            return reinterpret_cast<void*>(sym->getValue());
        } else {
            cajeta::jit::consumeError(sym.takeError());
            return nullptr;
        }
    };
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

std::vector<int32_t> matchingLocIds(const Breakpoint& bp) {
    namespace fs = std::filesystem;
    std::vector<int32_t> out;
    const auto& table = cajeta::dbg::globalDbgLocTable();
    // assignedIds, not 0..size(): the ranged allocator leaves the id space sparse.
    for (int32_t id : table.assignedIds()) {
        const auto& loc = table.at(id);
        if (loc.line != bp.line) continue;
        std::string base = fs::path(loc.file).filename().string();
        if (base == bp.file || loc.file == bp.file) out.push_back(id);
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
    JitRunOptions dbgOpts = opts;
    dbgOpts.debugInfo = true;

    auto impl = std::make_unique<JitDebugSession::Impl>();
    impl->built = buildJit(dbgOpts);
    if (impl->built.errorCode != 0 || !impl->built.jit) {
        if (error) *error = "compile/JIT failed (see stderr)";
        return nullptr;
    }

    for (const auto& bp : breakpoints)
        for (int32_t id : matchingLocIds(bp))
            impl->controller.arm(id);
    if (armExceptions) impl->controller.armException();
    if (stopOnEntry) impl->controller.armEntry();

    llvm::orc::LLJIT* jit = impl->built.jit.get();

    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        g_activeController = &impl->controller;
    }
    installHandler(jit, &safepointTrampoline);
    installExceptionHandler(jit, &exceptionTrampoline);
    callVoidSymbol(jit, "__cajeta_dbg_reset_safepoint_count");
    // No throw-site backtrace in debug sessions: the debugger supplies the stack,
    // and backtrace(3) hangs on the session's spawned program thread.
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

    {
        const auto& table = cajeta::dbg::globalDebugTypeTable();
        auto& rs = impl->resolvedTypeSymbols;
        for (const auto& [sym, entry] : table.vtables()) {
            if (auto a = jit->lookup(sym)) {
                rs.vtableByAddr[a->getValue()] = entry;
            } else {
                cajeta::jit::consumeError(a.takeError());
            }
        }
        std::set<std::string> seenStatics;
        for (const auto& name : table.names()) {
            const auto* rec = table.find(name);
            if (!rec) continue;
            for (const auto& sf : rec->statics) {
                if (!seenStatics.insert(sf.symbol).second) continue;
                if (auto a = jit->lookup(sf.symbol)) {
                    rs.staticAddrs[sf.symbol] =
                        reinterpret_cast<void*>(a->getValue());
                } else {
                    cajeta::jit::consumeError(a.takeError());
                }
            }
        }
    }

    installAmbientArgs(jit, opts.programArgs);

    // Materialize args BEFORE the program thread starts.
    void* entryArgs = nullptr;
    if (takesArgs) {
        entryArgs = makeEntryArgs(jit, impl->built.entryArgsABI);
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
    if (beforeRun) beforeRun();

    // Capture the PROGRAM thread's identity from the program thread itself — the
    // reset above ran on this setup thread, and a wrong marker un-arms every step.
    void* markFnAddr = nullptr;
    if (auto sym = jit->lookup("__cajeta_dbg_mark_program_thread")) {
        markFnAddr = reinterpret_cast<void*>(sym->getValue());
    } else {
        cajeta::jit::consumeError(sym.takeError());
    }
    raw->thread = std::thread([raw, entryAddr, returnsInt32, takesArgs,
                               entryArgs, markFnAddr]() {
        if (markFnAddr) reinterpret_cast<void (*)()>(markFnAddr)();
        if (returnsInt32) {
            raw->exitCode = takesArgs
                ? reinterpret_cast<int(*)(void*)>(entryAddr)(entryArgs)
                : reinterpret_cast<int(*)()>(entryAddr)();
        } else {
            if (takesArgs) reinterpret_cast<void(*)(void*)>(entryAddr)(entryArgs);
            else reinterpret_cast<void(*)()>(entryAddr)();
            raw->exitCode = 0;
        }
        callVoidSymbol(raw->built.jit.get(), "__cajeta_task_shutdown");
        raw->finished.store(true);
    });

    return std::make_unique<JitDebugSession>(std::move(impl));
}

// Portable setenv: the Windows CRT spells it `_putenv_s`. Both write the CRT
// environment the in-process JIT runtime reads back with getenv().
static void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, /*overwrite=*/1);
#endif
}

int dispatchRun(int argc, const char* argv[]) {
    // argv: cajeta run [--cache-dir=DIR] [--diag-format=...] <file>.cajeta [args...]
    cajeta::emitStreamRecordOnce();
    JitRunOptions opts;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--cache-dir=", 0) == 0) {
            opts.cacheDir = a.substr(std::string("--cache-dir=").size());
        } else if (a == "--diag-format=json") {
            setEnvVar("CAJETA_DIAG_FORMAT", "json");
        } else if (a == "--diag-format=text") {
            setEnvVar("CAJETA_DIAG_FORMAT", "text");
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty()) {
        std::cerr << "usage: cajeta run [--cache-dir=DIR] <file>.cajeta"
                     " [args...]\n";
        cajeta::emitJsonResult("error", "usage");
        return 2;
    }
    opts.scriptFile = positional[0];
    for (size_t i = 1; i < positional.size(); ++i)
        opts.programArgs.push_back(positional[i]);
    opts.entryMethod = "__cajeta_script_entry";

    std::error_code ec;
    std::filesystem::path scriptAbs =
        std::filesystem::absolute(opts.scriptFile, ec);
    if (ec || !std::filesystem::is_regular_file(scriptAbs)) {
        std::cerr << "cajeta run: script not found: " << opts.scriptFile
                  << "\n";
        cajeta::emitJsonResult("error", "script not found");
        return 2;
    }
    opts.sourceRoot = scriptAbs.parent_path().string();

    std::filesystem::path projectRoot;
    for (std::filesystem::path d = scriptAbs.parent_path();;
         d = d.parent_path()) {
        if (std::filesystem::exists(d / "cajeta.json")) {
            projectRoot = d;
            break;
        }
        if (d == d.root_path() || d.empty()) break;
    }
    if (!projectRoot.empty()) {
        auto manifest = cajeta::buildtool::loadManifestFile(
            (projectRoot / "cajeta.json").string());
        if (!manifest) {
            std::cerr << "cajeta run: bad manifest at " << projectRoot
                      << ": " << cajeta::jit::toString(manifest.takeError()) << "\n";
            cajeta::emitJsonResult("error", "bad manifest");
            return 2;
        }
        auto resolved = cajeta::buildtool::resolveProjectDependencies(
            *manifest, projectRoot.string());
        if (!resolved) {
            std::cerr << "cajeta run: dependency resolution failed: "
                      << cajeta::jit::toString(resolved.takeError()) << "\n";
            cajeta::emitJsonResult("error", "dependency resolution failed");
            return 2;
        }
        for (const auto& dep : *resolved) {
            if (!dep.artifactPath.empty())
                opts.classpath.push_back(dep.artifactPath);
        }
    }

    const int code = runJit(opts);
    cajeta::emitJsonResult(code == 0 ? "ok" : "error");
    return code;
}

int dispatchJitRun(int argc, const char* argv[]) {
    // argv: cajeta jit-run [-g|--debug-info] <sourceRoot> <entryMethod> [args...]
    cajeta::emitStreamRecordOnce();
    JitRunOptions opts;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-g" || a == "--debug-info" || a == "--debug-info=on") {
            opts.debugInfo = true;
        } else if (a == "--debug-info=off") {
            opts.debugInfo = false;
        } else if (a.rfind("--cache-dir=", 0) == 0) {
            opts.cacheDir = a.substr(std::string("--cache-dir=").size());
        } else if (a == "--diag-format=json") {
            setEnvVar("CAJETA_DIAG_FORMAT", "json");
        } else if (a == "--diag-format=text") {
            setEnvVar("CAJETA_DIAG_FORMAT", "text");
        } else if (a.rfind("--xpu-backend=", 0) == 0) {
            // An unknown backend name is a hard error: bundling nothing silently makes a
            // kernel launch a no-op.
            std::string list = a.substr(std::string("--xpu-backend=").size());
            size_t pos = 0;
            bool bad = false;
            while (pos <= list.size() && !bad) {
                size_t comma = list.find(',', pos);
                std::string one = list.substr(
                    pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
                if (!one.empty() && one != "none") {
                    if (one == "cpu")
                        opts.xpuBackends.push_back(cajeta::xpu::Backend::Cpu);
                    else if (one == "nvptx")
                        opts.xpuBackends.push_back(cajeta::xpu::Backend::Nvptx);
                    else if (one == "amdgpu")
                        opts.xpuBackends.push_back(cajeta::xpu::Backend::Amdgpu);
                    else if (one == "vulkan" || one == "spirv")
                        opts.xpuBackends.push_back(cajeta::xpu::Backend::Spirv);
                    else bad = true;
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (bad) {
                std::cerr << "cajeta jit-run: unknown --xpu-backend in '"
                          << a << "' (none|nvptx|amdgpu|vulkan|cpu)\n";
                cajeta::emitJsonResult("error", "usage");
                return 2;
            }
        } else if (a.rfind("--xpu-arch=", 0) == 0) {
            opts.xpuArch = a.substr(std::string("--xpu-arch=").size());
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() < 2) {
        std::cerr << "usage: cajeta jit-run [-g] <sourceRoot>"
                     " <package.Class.method> [args...]\n";
        cajeta::emitJsonResult("error", "usage");
        return 2;
    }
    opts.sourceRoot = positional[0];
    opts.entryMethod = positional[1];
    for (size_t i = 2; i < positional.size(); ++i)
        opts.programArgs.push_back(positional[i]);

    opts.onProgress = [](const std::string& phase, const std::string& detail,
                         int current, int total) {
        std::ostringstream m;
        if (phase == "parse" && total > 0)
            m << "[jit] parse " << current << "/" << total
              << " " << detail << "\n";
        else if (total == 0)
            m << "[jit] " << phase << "\n";
        if (!m.str().empty()) cajeta::logLine("debug", m.str());
    };

    // CAJETA_JIT_PHASES=1 dumps the build-phase wall-clock breakdown to stderr.
    if (std::getenv("CAJETA_JIT_PHASES")) {
        JitRunResult result;
        int code = runJit(opts, &result);
        const auto& ph = result.phases;
        std::cerr << "[jit-phases] collect=" << ph.collectSeconds
                  << "s parse=" << ph.parseSeconds
                  << "s (stdlib=" << ph.parseStdlibSeconds
                  << "s) codegen(stdlib)=" << ph.codegenStdlibSeconds
                  << "s codegen(user)=" << ph.codegenUserSeconds
                  << "s finalize=" << ph.finalizeSeconds
                  << "s merge=" << ph.mergeSeconds
                  << "s jit=" << ph.jitSeconds
                  << "s (ser=" << ph.jitSerializeSeconds
                  << "s reparse=" << ph.jitReparseSeconds
                  << "s mat=" << ph.jitMaterializeSeconds
                  << "s) total=" << ph.totalSeconds << "s\n";
        cajeta::emitJsonResult(code == 0 ? "ok" : "error");
        return code;
    }
    const int code = runJit(opts);
    cajeta::emitJsonResult(code == 0 ? "ok" : "error");
    return code;
}

} // namespace cajeta::jit
