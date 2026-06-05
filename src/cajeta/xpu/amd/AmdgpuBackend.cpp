//
// AMDGPU backend — see header.
//

#include "AmdgpuBackend.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
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
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>
#include <mutex>

namespace cajeta {
namespace xpu {
namespace amd {

namespace {

// Process-global LLVM target registry init; same rationale as NvptxBackend
// (this may run without a Compiler having initialized the registry).
void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllAsmParsers();
    });
}

// Locate the ROCm device-bitcode directory (ockl/oclc). Honors ROCM_PATH, else
// the canonical /opt/rocm install. Empty if neither has the bitcode.
std::string findRocmBitcodeDir() {
    auto has = [](const std::string& d) {
        return llvm::sys::fs::exists(d + "/ockl.bc");
    };
    if (const char* rp = std::getenv("ROCM_PATH")) {
        std::string d = std::string(rp) + "/amdgcn/bitcode";
        if (has(d)) return d;
    }
    std::string d = "/opt/rocm/amdgcn/bitcode";
    if (has(d)) return d;
    return {};
}

// Link one ROCm bitcode file into `m` (same context), pulling ONLY the symbols
// needed to resolve `m`'s outstanding references (LinkOnlyNeeded) — so a single
// device-library function (and its transitive deps) is merged in, not the whole
// library. Returns false (and logs) on parse/link failure.
bool linkRocmBitcode(llvm::Module& m, const std::string& path) {
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> lib =
        llvm::parseIRFile(path, err, m.getContext());
    if (!lib) {
        llvm::errs() << "cajeta.xpu.amd: cannot parse " << path << ": "
                     << err.getMessage() << "\n";
        return false;
    }
    // The device libs carry their own (amdgcn) datalayout/triple; align them to
    // the destination's so the linker doesn't reject a benign mismatch.
    lib->setDataLayout(m.getDataLayout());
    lib->setTargetTriple(m.getTargetTriple());
    if (llvm::Linker::linkModules(m, std::move(lib),
                                  llvm::Linker::Flags::LinkOnlyNeeded)) {
        llvm::errs() << "cajeta.xpu.amd: failed to link " << path << "\n";
        return false;
    }
    return true;
}

// Item 8 Stage C (hybrid): a kernel that samples a Texture2D references the
// ROCm device-library function __ockl_image_sample_2D (emitted by AmdgpuTarget).
// Link it — and the gfx ISA-version control global it reads — ONLY for such
// kernels; every other AMD kernel is untouched (no device-lib dependency). If
// the bitcode isn't installed, the declaration is left unresolved: codegen then
// fails and the kernel falls back to the host stub, exactly like XPU-N01.
// True if `m` references any declaration whose name starts with `prefix`.
static bool referencesDeviceLib(llvm::Module& m, const char* prefix) {
    for (llvm::Function& fn : m)
        if (fn.isDeclaration() && fn.getName().starts_with(prefix))
            return true;
    return false;
}

void linkAmdDeviceLibsIfNeeded(llvm::Module& m, llvm::TargetMachine& tm) {
    bool needsOckl = m.getFunction("__ockl_image_sample_2D") &&
                     m.getFunction("__ockl_image_sample_2D")->isDeclaration();
    bool needsOcml = referencesDeviceLib(m, "__ocml_");   // B2 transcendentals
    if (!needsOckl && !needsOcml) return;

    std::string dir = findRocmBitcodeDir();
    if (dir.empty()) {
        llvm::errs() << "cajeta.xpu.amd: ROCm device bitcode not found "
                        "(set ROCM_PATH); device math / texture sampling needs "
                        "ocml.bc / ockl.bc\n";
        return;
    }
    if (needsOckl && !linkRocmBitcode(m, dir + "/ockl.bc")) return;
    if (needsOcml) linkRocmBitcode(m, dir + "/ocml.bc");   // __ocml_<fn>_f32 defs

    // Both ockl (image sample) and ocml (transcendentals) read the oclc control
    // globals: the ISA version plus the math-option flags. Link the standard set
    // (each only if present) so no `__oclc_*` constant is left undefined.
    std::string gfx = tm.getTargetCPU().str();          // e.g. "gfx1151"
    std::string isa = gfx.rfind("gfx", 0) == 0 ? gfx.substr(3) : gfx;
    linkRocmBitcode(m, dir + "/oclc_isa_version_" + isa + ".bc");
    if (needsOcml) {
        for (const char* ctrl : {"oclc_finite_only_off",
                                 "oclc_unsafe_math_off",
                                 "oclc_correctly_rounded_sqrt_on",
                                 "oclc_daz_opt_off",
                                 "oclc_wavefrontsize64_off"}) {
            std::string p = dir + "/" + ctrl + ".bc";
            if (llvm::sys::fs::exists(p)) linkRocmBitcode(m, p);
        }
    }
}

// Promote entry-block allocas (loop counters, accumulators, reassigned
// locals) to SSA registers before ISA emission. On AMDGPU these allocas live
// in the private address space (5); mem2reg removes most of them, which both
// improves codegen and sidesteps scratch traffic. addPassesToEmitFile runs
// only the codegen pipeline (no IR optimization), so this must run first.
void optimizeDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    // Pull in ROCm image-sample device code first (texture kernels only), so the
    // merged functions go through mem2reg + codegen with the kernel.
    linkAmdDeviceLibsIfNeeded(m, tm);
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

// Run the codegen pipeline to a chosen file type (Assembly or Object) into an
// in-memory buffer. Returns false (and logs) if the TargetMachine can't emit
// that file type.
bool emitToBuffer(llvm::Module& m, llvm::TargetMachine& tm,
                  llvm::CodeGenFileType type, llvm::SmallVectorImpl<char>& out) {
    optimizeDeviceModule(m, tm);
    llvm::raw_svector_ostream os(out);
    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, os, /*DwoOut=*/nullptr, type)) {
        llvm::errs() << "cajeta.xpu.amd: AMDGPU TargetMachine cannot emit "
                     << (type == llvm::CodeGenFileType::AssemblyFile
                             ? "assembly" : "object") << "\n";
        return false;
    }
    pm.run(m);
    return true;
}

} // namespace

std::unique_ptr<llvm::TargetMachine>
createAmdgpuTargetMachine(const std::string& arch) {
    ensureTargetsInitialized();

    llvm::Triple triple(kAmdgpuTriple);
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << "cajeta.xpu.amd: amdgcn target not available: "
                     << error << "\n";
        return nullptr;
    }

    llvm::TargetOptions opt;
    // AMDGPU code objects are position-independent; PIC is the supported reloc
    // model (the default/static models are rejected by the AMDGPU backend).
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/arch, /*Features=*/"", opt,
        /*RM=*/llvm::Reloc::PIC_);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kAmdgpuTriple));
    m.setDataLayout(tm.createDataLayout());
}

std::string emitIsa(llvm::Module& deviceModule, llvm::TargetMachine& tm) {
    llvm::SmallString<0> buf;
    if (!emitToBuffer(deviceModule, tm, llvm::CodeGenFileType::AssemblyFile,
                      buf)) {
        return {};
    }
    return std::string(buf.begin(), buf.end());
}

std::string findLld() {
    // 1. $ROCM_PATH/llvm/bin, then the conventional /opt/rocm location.
    auto tryDir = [](const std::string& dir) -> std::string {
        for (const char* exe : {"/ld.lld", "/ld.lld.exe"}) {
            std::string p = dir + exe;
            if (llvm::sys::fs::exists(p)) return p;
        }
        return {};
    };
    if (const char* rocm = std::getenv("ROCM_PATH")) {
        if (auto p = tryDir(std::string(rocm) + "/llvm/bin"); !p.empty())
            return p;
    }
    if (auto p = tryDir("/opt/rocm/llvm/bin"); !p.empty()) return p;
    // 2. PATH.
    if (auto found = llvm::sys::findProgramByName("ld.lld")) return *found;
    return {};
}

std::vector<uint8_t> assembleHsaco(llvm::Module& deviceModule,
                                   llvm::TargetMachine& tm,
                                   const std::string& /*arch*/) {
    std::string lld = findLld();
    if (lld.empty()) {
        llvm::errs() << "cajeta.xpu.amd: ld.lld not found (set ROCM_PATH or "
                        "put ld.lld on PATH)\n";
        return {};
    }

    // 1. Emit the relocatable AMDGCN ELF object to a temp file.
    llvm::SmallString<0> objBuf;
    if (!emitToBuffer(deviceModule, tm, llvm::CodeGenFileType::ObjectFile,
                      objBuf)) {
        return {};
    }

    llvm::SmallString<128> objPath, hsacoPath;
    if (llvm::sys::fs::createTemporaryFile("cajeta_xpu", "o", objPath) ||
        llvm::sys::fs::createTemporaryFile("cajeta_xpu", "hsaco", hsacoPath)) {
        llvm::errs() << "cajeta.xpu.amd: could not create temp files\n";
        return {};
    }
    struct Cleanup {
        llvm::SmallString<128> a, b;
        ~Cleanup() { llvm::sys::fs::remove(a); llvm::sys::fs::remove(b); }
    } cleanup{objPath, hsacoPath};

    {
        std::error_code ec;
        llvm::raw_fd_ostream out(objPath, ec, llvm::sys::fs::OF_None);
        if (ec) {
            llvm::errs() << "cajeta.xpu.amd: could not write object: "
                         << ec.message() << "\n";
            return {};
        }
        out.write(objBuf.data(), objBuf.size());
    }

    // 2. ld.lld -shared <obj> -o <hsaco>. -shared makes the code object an
    // ET_DYN ELF, which hipModuleLoad accepts. ExecuteAndWait passes argv
    // directly (no shell), so spaces in paths are safe.
    std::string sharedFlag = "-shared";
    std::string oFlag = "-o";
    llvm::SmallVector<llvm::StringRef, 8> args = {
        lld, sharedFlag, objPath.str(), oFlag, hsacoPath.str()};
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(lld, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc != 0) {
        llvm::errs() << "cajeta.xpu.amd: ld.lld failed (rc=" << rc << ") "
                     << errMsg << "\n";
        return {};
    }

    auto buf = llvm::MemoryBuffer::getFile(hsacoPath, /*IsText=*/false);
    if (!buf) {
        llvm::errs() << "cajeta.xpu.amd: could not read hsaco: "
                     << buf.getError().message() << "\n";
        return {};
    }
    llvm::StringRef bytes = (*buf)->getBuffer();
    return std::vector<uint8_t>(bytes.bytes_begin(), bytes.bytes_end());
}

std::vector<std::string> splitArchList(const std::string& arch) {
    std::vector<std::string> out;
    for (size_t s = 0; s <= arch.size();) {
        size_t c = arch.find(',', s);
        std::string a = arch.substr(s, c == std::string::npos ? c : c - s);
        while (!a.empty() && a.front() == ' ') a.erase(a.begin());
        while (!a.empty() && a.back() == ' ') a.pop_back();
        if (!a.empty()) out.push_back(a);
        if (c == std::string::npos) break;
        s = c + 1;
    }
    return out;
}

std::vector<uint8_t> assembleHsacoBundle(
        llvm::Module& deviceModule, const std::vector<std::string>& arches) {
    if (arches.empty()) return {};
    if (arches.size() == 1) {
        auto tm = createAmdgpuTargetMachine(arches[0]);
        return tm ? assembleHsaco(deviceModule, *tm, arches[0])
                  : std::vector<uint8_t>{};
    }
    auto bundler = llvm::sys::findProgramByName("clang-offload-bundler");
    if (!bundler) {
        llvm::errs() << "cajeta.xpu.amd: clang-offload-bundler not found "
                        "(needed for multi-arch); set ROCM_PATH or PATH\n";
        return {};
    }

    // Per-arch hsaco temp files + a 1-byte dummy host input (HIP fatbins lead
    // with the host bundle). Clean them all up at return.
    std::vector<std::string> tmpFiles;
    auto cleanup = [&]() { for (auto& f : tmpFiles) llvm::sys::fs::remove(f); };
    auto writeTemp = [&](const char* ext, const uint8_t* data, size_t len,
                         std::string& out) -> bool {
        llvm::SmallString<128> p;
        if (llvm::sys::fs::createTemporaryFile("cajeta_xpu_mar", ext, p))
            return false;
        out = std::string(p);
        tmpFiles.push_back(out);
        std::error_code ec;
        llvm::raw_fd_ostream o(out, ec, llvm::sys::fs::OF_None);
        if (ec) return false;
        o.write(reinterpret_cast<const char*>(data), (size_t) len);
        return true;
    };

    std::string hostFile;
    const uint8_t one = 0;
    if (!writeTemp("o", &one, 1, hostFile)) { cleanup(); return {}; }

    std::string targets = "host-x86_64-unknown-linux-gnu";
    std::vector<std::string> inputFlags = {"-input=" + hostFile};
    for (const std::string& arch : arches) {
        auto tm = createAmdgpuTargetMachine(arch);
        if (!tm) { cleanup(); return {}; }
        auto clone = llvm::CloneModule(deviceModule);   // assembleHsaco mutates
        std::vector<uint8_t> hsaco = assembleHsaco(*clone, *tm, arch);
        if (hsaco.empty()) { cleanup(); return {}; }
        std::string f;
        if (!writeTemp("hsaco", hsaco.data(), hsaco.size(), f)) {
            cleanup(); return {};
        }
        targets += ",hipv4-amdgcn-amd-amdhsa--" + arch;
        inputFlags.push_back("-input=" + f);
    }

    std::string bundleFile;
    if (!writeTemp("hipfb", &one, 0, bundleFile)) { cleanup(); return {}; }

    std::string typeFlag = "-type=o", targetsFlag = "-targets=" + targets,
                outFlag = "-output=" + bundleFile;
    llvm::SmallVector<llvm::StringRef, 16> args = {*bundler, typeFlag, targetsFlag};
    for (auto& in : inputFlags) args.push_back(in);
    args.push_back(outFlag);
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(*bundler, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc != 0) {
        llvm::errs() << "cajeta.xpu.amd: clang-offload-bundler failed (rc=" << rc
                     << ") " << errMsg << "\n";
        cleanup();
        return {};
    }
    auto buf = llvm::MemoryBuffer::getFile(bundleFile, /*IsText=*/false);
    std::vector<uint8_t> bundle;
    if (buf) {
        llvm::StringRef b = (*buf)->getBuffer();
        bundle.assign(b.bytes_begin(), b.bytes_end());
    }
    cleanup();
    return bundle;
}

} // namespace amd
} // namespace xpu
} // namespace cajeta
