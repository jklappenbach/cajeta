// NVPTX backend — see header.

#include "NvptxBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

/// Target registry init; this may run before any Compiler has done it.
void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();
    });
}

/// NVIDIA's libdevice bitcode, or empty if no install has it.
std::string findLibdevice() {
    auto has = [](const std::string& p) { return llvm::sys::fs::exists(p); };
    auto probe = [&](const std::string& root) -> std::string {
        std::string p = root + "/nvvm/libdevice/libdevice.10.bc";
        return has(p) ? p : std::string{};
    };
    if (const char* cp = std::getenv("CUDA_PATH")) {
        if (std::string p = probe(cp); !p.empty()) return p;
    }
    if (std::string p = probe("/usr/local/cuda"); !p.empty()) return p;
    // Compared NUMERICALLY: "cuda-9.0" sorts above "cuda-13.3" as text.
    std::error_code ec;
    std::string best;
    long bestMajor = -1, bestMinor = -1;
    for (llvm::sys::fs::directory_iterator it("/usr/local", ec), end;
         it != end && !ec; it.increment(ec)) {
        llvm::StringRef d = llvm::StringRef(it->path());
        size_t at = d.rfind("/cuda-");
        if (at == llvm::StringRef::npos) continue;
        llvm::StringRef ver = d.substr(at + 6);          // "13.3"
        long major = 0, minor = 0;
        auto [majStr, rest] = ver.split('.');
        if (majStr.getAsInteger(10, major)) continue;    // non-numeric -> skip
        if (!rest.empty()) rest.getAsInteger(10, minor); // absent minor -> 0
        if (probe(d.str()).empty()) continue;
        if (major > bestMajor || (major == bestMajor && minor > bestMinor)) {
            bestMajor = major; bestMinor = minor; best = d.str();
        }
    }
    return best.empty() ? std::string{} : probe(best);
}

/// True if `m` references any DECLARATION whose name starts with `prefix`.
bool referencesDeviceLib(llvm::Module& m, const char* prefix) {
    for (llvm::Function& fn : m)
        if (fn.isDeclaration() && fn.getName().starts_with(prefix))
            return true;
    return false;
}

/// Links libdevice into `m` ONLY when a __nv_* declaration is outstanding: most
/// kernels must not pay to parse ~500 KB of bitcode. Needed symbols only.
void linkCudaDeviceLibsIfNeeded(llvm::Module& m) {
    if (!referencesDeviceLib(m, "__nv_")) return;

    std::string path = findLibdevice();
    if (path.empty()) {
        llvm::errs() << "cajeta.xpu.nvidia: libdevice.10.bc not found (set "
                        "CUDA_PATH); device transcendentals (Math.exp/cos/...) "
                        "cannot be assembled — ptxas will report an unresolved "
                        "__nv_* extern and the kernel is skipped\n";
        return;
    }
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> lib =
        llvm::parseIRFile(path, err, m.getContext());
    if (!lib) {
        llvm::errs() << "cajeta.xpu.nvidia: cannot parse " << path << ": "
                     << err.getMessage() << "\n";
        return;
    }
    // Aligning libdevice's older datalayout and triple avoids a benign mismatch.
    lib->setDataLayout(m.getDataLayout());
    lib->setTargetTriple(m.getTargetTriple());
    if (llvm::Linker::linkModules(m, std::move(lib),
                                  llvm::Linker::Flags::LinkOnlyNeeded)) {
        llvm::errs() << "cajeta.xpu.nvidia: failed to link " << path << "\n";
        return;
    }

    // NVVMReflect folds libdevice's guards using these flags; without them the
    // reflect calls survive as externs. 0 preserves IEEE denormals.
    if (!m.getModuleFlag("nvvm-reflect-ftz"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-ftz", (uint32_t) 0);
    if (!m.getModuleFlag("nvvm-reflect-prec-sqrt"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-prec-sqrt", (uint32_t) 1);
    if (!m.getModuleFlag("nvvm-reflect-prec-div"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-prec-div", (uint32_t) 1);
    if (!m.getModuleFlag("nvvm-reflect-approx-func"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-approx-func", (uint32_t) 0);
}

/// Runs the IR pipeline before PTX emission, libdevice linked first so the
/// merged bodies optimize with the kernel. Nothing else optimizes device IR.
void optimizeDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    linkCudaDeviceLibsIfNeeded(m);
    llvm::PassBuilder pb(&tm);
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    // Default O3; CAJETA_XPU_DEVICE_OPT=0|1|2|3 overrides.
    llvm::ModulePassManager mpm;
    int lvl = 3;
    if (const char* e = std::getenv("CAJETA_XPU_DEVICE_OPT")) lvl = std::atoi(e);
    if (lvl <= 0) {
        // The inliner is NOT optional here: lowerDeviceFn passes a @Device
        // helper the buffer BASE by value and relies on inlining to splice it
        // in. Left out-of-line the call reads a bogus base and faults at launch.
        mpm.addPass(llvm::AlwaysInlinerPass());
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());  // mem2reg
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    } else {
        llvm::OptimizationLevel ol = lvl == 1 ? llvm::OptimizationLevel::O1
                                   : lvl == 2 ? llvm::OptimizationLevel::O2
                                              : llvm::OptimizationLevel::O3;
        mpm = pb.buildPerModuleDefaultPipeline(ol);
    }
    mpm.run(m, mam);
}

} // namespace

std::unique_ptr<llvm::TargetMachine>
createNvptxTargetMachine(const std::string& arch) {
    ensureTargetsInitialized();

    llvm::Triple triple(kNvptxTriple);
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << "cajeta.xpu.nvidia: nvptx64 target not available: "
                     << error << "\n";
        return nullptr;
    }

    llvm::TargetOptions opt;
    // PTX is position-independent and ptxas does final placement.
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/arch, /*Features=*/"", opt, /*RM=*/std::nullopt);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kNvptxTriple));
    m.setDataLayout(tm.createDataLayout());
}

std::string emitPtx(llvm::Module& deviceModule, llvm::TargetMachine& tm) {
    // addPassesToEmitFile needs a raw_pwrite_stream; PTX is textual, so AssemblyFile.
    optimizeDeviceModule(deviceModule, tm);

    llvm::SmallString<0> buf;
    llvm::raw_svector_ostream os(buf);

    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, os, /*DwoOut=*/nullptr,
                               llvm::CodeGenFileType::AssemblyFile)) {
        llvm::errs() << "cajeta.xpu.nvidia: NVPTX TargetMachine cannot emit "
                        "assembly\n";
        return {};
    }
    pm.run(deviceModule);
    return std::string(buf.begin(), buf.end());
}

std::string findPtxas() {
    if (const char* cudaPath = std::getenv("CUDA_PATH")) {
        for (const char* exe : {"/bin/ptxas.exe", "/bin/ptxas"}) {
            std::string p = std::string(cudaPath) + exe;
            if (llvm::sys::fs::exists(p)) return p;
        }
    }
    if (auto found = llvm::sys::findProgramByName("ptxas")) return *found;
    return {};
}

std::vector<PtxasKernelStats> parsePtxasVerbose(const std::string& text) {
    std::vector<PtxasKernelStats> out;
    // The unsigned integer immediately before `suffix` on `line`; 0 if absent.
    auto numBefore = [](const std::string& line, const char* suffix) -> unsigned {
        size_t p = line.find(suffix);
        if (p == std::string::npos) return 0;
        size_t e = p;
        while (e > 0 && line[e - 1] == ' ') --e;
        size_t s = e;
        while (s > 0 && std::isdigit((unsigned char) line[s - 1])) --s;
        if (s == e) return 0;
        return (unsigned) std::strtoul(line.substr(s, e - s).c_str(), nullptr, 10);
    };
    std::istringstream ss(text);
    std::string line;
    PtxasKernelStats* cur = nullptr;
    while (std::getline(ss, line)) {
        if (size_t p = line.find("Function properties for "); p != std::string::npos) {
            std::string name = line.substr(p + std::strlen("Function properties for "));
            while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == ':'))
                name.pop_back();
            out.push_back(PtxasKernelStats{});
            cur = &out.back();
            cur->name = name;
            continue;
        }
        if (!cur) continue;
        if (line.find("bytes stack frame") != std::string::npos) {
            cur->stackBytes = numBefore(line, " bytes stack frame");
            cur->spillStoreBytes = numBefore(line, " bytes spill stores");
            cur->spillLoadBytes = numBefore(line, " bytes spill loads");
        }
        if (line.find("Used ") != std::string::npos
                && line.find(" registers") != std::string::npos) {
            cur->registers = numBefore(line, " registers");
            cur->smemBytes = numBefore(line, " bytes smem");
        }
    }
    return out;
}

std::vector<uint8_t> assembleCubin(const std::string& ptx,
                                   const std::string& arch,
                                   std::string* verboseLog) {
    std::string ptxas = findPtxas();
    if (ptxas.empty()) {
        llvm::errs() << "cajeta.xpu.nvidia: ptxas not found (set CUDA_PATH or "
                        "put ptxas on PATH)\n";
        return {};
    }

    llvm::SmallString<128> ptxPath, cubinPath;
    // Constructed BEFORE the temp files, so a later failure still removes them.
    struct Cleanup {
        llvm::SmallString<128> &a, &b;
        ~Cleanup() { llvm::sys::fs::remove(a); llvm::sys::fs::remove(b); }
    } cleanup{ptxPath, cubinPath};
    if (llvm::sys::fs::createTemporaryFile("cajeta_xpu", "ptx", ptxPath) ||
        llvm::sys::fs::createTemporaryFile("cajeta_xpu", "cubin", cubinPath)) {
        llvm::errs() << "cajeta.xpu.nvidia: could not create temp files\n";
        return {};
    }

    {
        std::error_code ec;
        llvm::raw_fd_ostream out(ptxPath, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            llvm::errs() << "cajeta.xpu.nvidia: could not write PTX: "
                         << ec.message() << "\n";
            return {};
        }
        out << ptx;
    }

    // ExecuteAndWait passes argv directly, so paths with spaces are safe.
    std::string archArg = "-arch=" + arch;
    std::string oFlag = "-o";
    std::string vFlag = "-v";
    llvm::SmallVector<llvm::StringRef, 8> args = {
        ptxas, archArg, ptxPath.str(), oFlag, cubinPath.str()};
    // `-v` prints the per-kernel resource report on stderr; the cubin is unaffected.
    llvm::SmallString<128> logPath;
    std::optional<llvm::StringRef> redirects[3] = {std::nullopt, std::nullopt, std::nullopt};
    bool capture = false;
    if (verboseLog) {
        verboseLog->clear();
        if (!llvm::sys::fs::createTemporaryFile("cajeta_xpu", "ptxas.log", logPath)) {
            args.push_back(vFlag);
            redirects[2] = llvm::StringRef(logPath);
            capture = true;
        }
    }
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(
        ptxas, args, /*Env=*/std::nullopt,
        /*Redirects=*/capture ? llvm::ArrayRef<std::optional<llvm::StringRef>>(redirects)
                              : llvm::ArrayRef<std::optional<llvm::StringRef>>(),
        /*SecondsToWait=*/0, /*MemoryLimit=*/0, &errMsg);
    if (capture) {
        if (auto log = llvm::MemoryBuffer::getFile(logPath, /*IsText=*/true))
            *verboseLog = (*log)->getBuffer().str();
        llvm::sys::fs::remove(logPath);
    }
    if (rc != 0) {
        llvm::errs() << "cajeta.xpu.nvidia: ptxas failed (rc=" << rc << ") "
                     << errMsg << "\n";
        if (capture && verboseLog && !verboseLog->empty())
            llvm::errs() << *verboseLog << "\n";
        return {};
    }

    auto buf = llvm::MemoryBuffer::getFile(cubinPath, /*IsText=*/false);
    if (!buf) {
        llvm::errs() << "cajeta.xpu.nvidia: could not read cubin: "
                     << buf.getError().message() << "\n";
        return {};
    }
    llvm::StringRef bytes = (*buf)->getBuffer();
    return std::vector<uint8_t>(bytes.bytes_begin(), bytes.bytes_end());
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
