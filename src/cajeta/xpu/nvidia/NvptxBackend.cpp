//
// NVPTX backend — see header.
//

#include "NvptxBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/MC/TargetRegistry.h"
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

// Promote the kernel's entry-block allocas (loop counters, accumulators,
// reassigned locals — see NvptxKernelLowering's mutable-slot model) into SSA
// registers before PTX emission. addPassesToEmitFile runs ONLY the codegen
// pipeline, no IR optimization, so without this the slots stay as .local
// load/store traffic. mem2reg alone is correct and cheap; running it is
// purely a quality improvement (alloca PTX is valid for ptxas either way).
void optimizeDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
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

    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::PromotePass());  // mem2reg
    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
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
