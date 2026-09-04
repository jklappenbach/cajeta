//
// NVPTX backend — see header.
//

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

#include <cstdlib>
#include <mutex>

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {

// The LLVM target registry is process-global. The Compiler ctor
// already calls InitializeAll*, but NvptxBackend may be exercised
// without a Compiler (standalone tooling / tests), so guard a private
// one-time init. Re-initializing is harmless, but call_once keeps it
// tidy.
void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();
    });
}

// Locate NVIDIA's libdevice bitcode. Honors CUDA_PATH, then the canonical
// /usr/local/cuda symlink, then any versioned sibling — newest last-wins so a
// box with several toolkits installed picks the highest. Empty if none has it.
//
// This is the NVPTX twin of findRocmBitcodeDir(): AMD has resolved __ocml_*
// out of ocml.bc since B2, and NVPTX emitted __nv_* calls with a comment
// promising the same ("linked at cubin time") that nothing ever performed.
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
    // Versioned installs, e.g. /usr/local/cuda-13.3. Compare NUMERICALLY, not
    // lexicographically: "cuda-9.0" sorts above "cuda-13.3" as text, which
    // would select the oldest toolkit on a box that has both.
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

// True if `m` references any *declaration* whose name starts with `prefix`.
bool referencesDeviceLib(llvm::Module& m, const char* prefix) {
    for (llvm::Function& fn : m)
        if (fn.isDeclaration() && fn.getName().starts_with(prefix))
            return true;
    return false;
}

// Link libdevice into `m`, but ONLY when a __nv_* declaration is outstanding:
// the overwhelming majority of kernels call no transcendental and must not pay
// for parsing a ~500 KB bitcode module. LinkOnlyNeeded pulls just the needed
// function and its transitive deps, not the whole library.
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
    // libdevice ships with its own (older) datalayout/triple; align both to the
    // destination so the linker does not reject a benign mismatch. Same
    // treatment linkRocmBitcode gives the ROCm device libs.
    lib->setDataLayout(m.getDataLayout());
    lib->setTargetTriple(m.getTargetTriple());
    if (llvm::Linker::linkModules(m, std::move(lib),
                                  llvm::Linker::Flags::LinkOnlyNeeded)) {
        llvm::errs() << "cajeta.xpu.nvidia: failed to link " << path << "\n";
        return;
    }

    // libdevice bodies are guarded by __nvvm_reflect("__CUDA_FTZ") and friends.
    // NVPTX's codegen pipeline runs NVVMReflect, but it reads these module
    // flags to decide what to fold to; without them the reflect calls can
    // survive as unresolved externs — trading one ptxas failure for another.
    // 0 = IEEE denormals preserved, matching the non-fast-math default the
    // rest of the lowering assumes (@FastMath is applied per-instruction).
    if (!m.getModuleFlag("nvvm-reflect-ftz"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-ftz", (uint32_t) 0);
    if (!m.getModuleFlag("nvvm-reflect-prec-sqrt"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-prec-sqrt", (uint32_t) 1);
    if (!m.getModuleFlag("nvvm-reflect-prec-div"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-prec-div", (uint32_t) 1);
    if (!m.getModuleFlag("nvvm-reflect-approx-func"))
        m.addModuleFlag(llvm::Module::Override, "nvvm-reflect-approx-func", (uint32_t) 0);
}

// Promote the kernel's entry-block allocas (loop counters, accumulators,
// reassigned locals — see NvptxKernelLowering's mutable-slot model) into SSA
// registers before PTX emission. addPassesToEmitFile runs ONLY the codegen
// pipeline, no IR optimization, so without this the slots stay as .local
// load/store traffic. mem2reg alone is correct and cheap; running it is
// purely a quality improvement (alloca PTX is valid for ptxas either way).
void optimizeDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    // Resolve libdevice transcendentals FIRST, so the merged bodies go through
    // mem2reg and codegen with the kernel (the AmdgpuBackend ordering).
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

    // Full IR pipeline before codegen. addPassesToEmitFile runs NO IR passes,
    // so without this every @Kernel ships UNOPTIMIZED — redundant address math
    // spills regalloc, and (the correctness half) the AlwaysInline @Device
    // helpers stay out-of-line. Default O3, matching AmdgpuBackend; override
    // with CAJETA_XPU_DEVICE_OPT=0|1|2|3.
    llvm::ModulePassManager mpm;
    int lvl = 3;
    if (const char* e = std::getenv("CAJETA_XPU_DEVICE_OPT")) lvl = std::atoi(e);
    if (lvl <= 0) {
        // Correctness floor — UNLIKE AmdgpuBackend's lvl-0 (mem2reg only), the
        // inliner is NOT optional on nvptx. lowerDeviceFn stamps every @Device
        // helper `alwaysinline` and passes the kernel's buffer BASE by value,
        // relying on inlining to splice that base into the kernel's real buffer
        // access (KernelLowering lowerDeviceFn, "the base flows straight through
        // inlining"). Left out-of-line, the call reads a bogus base and
        // cuLaunchKernel returns CUDA_ERROR_ILLEGAL_ADDRESS (700). So even the
        // minimal fallback runs AlwaysInliner before mem2reg.
        mpm.addPass(llvm::AlwaysInlinerPass());
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());  // mem2reg
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    } else {
        // buildPerModuleDefaultPipeline(O1/O2/O3) runs the inliner (which honors
        // alwaysinline) plus mem2reg and the rest, so the 700 fix holds here too.
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
    // Default reloc/codegen model is correct for PTX (position-
    // independent, default code model). ptxas does final placement.
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/arch, /*Features=*/"", opt, /*RM=*/std::nullopt);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kNvptxTriple));
    m.setDataLayout(tm.createDataLayout());
}

std::string emitPtx(llvm::Module& deviceModule, llvm::TargetMachine& tm) {
    // addPassesToEmitFile requires a raw_pwrite_stream; raw_svector_ostream
    // qualifies (raw_string_ostream does not). PTX is textual, so
    // AssemblyFile (not ObjectFile); the NVPTX AsmPrinter must be
    // registered (InitializeAllAsmPrinters).
    // Promote alloca slots to registers first (the codegen pipeline below
    // does no IR optimization on its own).
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
    // 1. $CUDA_PATH/bin (set by the CUDA toolkit installer).
    if (const char* cudaPath = std::getenv("CUDA_PATH")) {
        for (const char* exe : {"/bin/ptxas.exe", "/bin/ptxas"}) {
            std::string p = std::string(cudaPath) + exe;
            if (llvm::sys::fs::exists(p)) return p;
        }
    }
    // 2. PATH.
    if (auto found = llvm::sys::findProgramByName("ptxas")) return *found;
    return {};
}

std::vector<uint8_t> assembleCubin(const std::string& ptx,
                                   const std::string& arch) {
    std::string ptxas = findPtxas();
    if (ptxas.empty()) {
        llvm::errs() << "cajeta.xpu.nvidia: ptxas not found (set CUDA_PATH or "
                        "put ptxas on PATH)\n";
        return {};
    }

    // ptxas works on files; round-trip through temporaries. unique paths
    // avoid collisions across concurrent compiles.
    llvm::SmallString<128> ptxPath, cubinPath;
    // Guard by reference and construct BEFORE creating the temp files: if the
    // ptx file is created but the cubin file then fails, the dtor still removes
    // the orphaned ptx (remove on an empty/absent path is a harmless no-op).
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

    // ptxas -arch=sm_89 <in.ptx> -o <out.cubin>. ExecuteAndWait passes argv
    // directly (no shell), so paths with spaces ("Program Files") are safe.
    std::string archArg = "-arch=" + arch;
    std::string oFlag = "-o";
    llvm::SmallVector<llvm::StringRef, 8> args = {
        ptxas, archArg, ptxPath.str(), oFlag, cubinPath.str()};
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(ptxas, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc != 0) {
        llvm::errs() << "cajeta.xpu.nvidia: ptxas failed (rc=" << rc << ") "
                     << errMsg << "\n";
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
