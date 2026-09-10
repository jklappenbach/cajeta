// AMDGPU backend — see header.

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

#include <cstdlib>
#include <cstring>
#include <sstream>
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Object/ELF.h"

#include <cstdlib>
#include <mutex>

namespace cajeta {
namespace xpu {
namespace amd {

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

/// The ROCm device-bitcode directory under ROCM_PATH or /opt/rocm; every known
/// layout is probed, since its place moved across releases.
std::string findRocmBitcodeDir() {
    auto has = [](const std::string& d) {
        return llvm::sys::fs::exists(d + "/ockl.bc");
    };
    static const char* kSubdirs[] = {
        "/amdgcn/bitcode",           // classic layout
        "/lib/llvm/amdgcn/bitcode",  // ROCm 7.x (bundled LLVM)
        "/lib/rocm-device-libs/amdgcn/bitcode",
    };
    auto probe = [&](const std::string& root) -> std::string {
        for (const char* sub : kSubdirs) {
            std::string d = root + sub;
            if (has(d)) return d;
        }
        return {};
    };
    if (const char* rp = std::getenv("ROCM_PATH")) {
        if (std::string d = probe(rp); !d.empty()) return d;
    }
    if (std::string d = probe("/opt/rocm"); !d.empty()) return d;
    return {};
}

/// Links one ROCm bitcode file into `m`, pulling ONLY still-needed symbols.
bool linkRocmBitcode(llvm::Module& m, const std::string& path) {
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> lib =
        llvm::parseIRFile(path, err, m.getContext());
    if (!lib) {
        llvm::errs() << "cajeta.xpu.amd: cannot parse " << path << ": "
                     << err.getMessage() << "\n";
        return false;
    }
    // Aligning the libs' own datalayout and triple avoids a benign mismatch.
    lib->setDataLayout(m.getDataLayout());
    lib->setTargetTriple(m.getTargetTriple());
    if (llvm::Linker::linkModules(m, std::move(lib),
                                  llvm::Linker::Flags::LinkOnlyNeeded)) {
        llvm::errs() << "cajeta.xpu.amd: failed to link " << path << "\n";
        return false;
    }
    return true;
}

/// True if `m` references any declaration whose name starts with `prefix`.
static bool referencesDeviceLib(llvm::Module& m, const char* prefix) {
    for (llvm::Function& fn : m)
        if (fn.isDeclaration() && fn.getName().starts_with(prefix))
            return true;
    return false;
}

/// Links only the ROCm device libraries this kernel needs; uninstalled bitcode
/// leaves the declaration unresolved and the kernel falls back to the host stub.
void linkAmdDeviceLibsIfNeeded(llvm::Module& m, llvm::TargetMachine& tm) {
    bool needsOckl = referencesDeviceLib(m, "__ockl_image_");
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

    // Both libraries read the oclc control globals; link the standard set.
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

/// Runs the IR pipeline before ISA emission, device libraries linked first.
void optimizeDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
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

    // Default O3; CAJETA_XPU_DEVICE_OPT=0|1|2|3 overrides, 0 being mem2reg only.
    llvm::ModulePassManager mpm;
    int lvl = 3;
    if (const char* e = std::getenv("CAJETA_XPU_DEVICE_OPT")) lvl = std::atoi(e);
    if (lvl <= 0) {
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());
        mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
    } else {
        llvm::OptimizationLevel ol = lvl == 1 ? llvm::OptimizationLevel::O1
                                   : lvl == 2 ? llvm::OptimizationLevel::O2
                                              : llvm::OptimizationLevel::O3;
        mpm = pb.buildPerModuleDefaultPipeline(ol);
    }
    if (const char* dumpDir = std::getenv("CAJETA_XPU_DUMP_BC")) {
        std::error_code ec;
        llvm::raw_fd_ostream pre(std::string(dumpDir) + "/" + m.getName().str() + ".pre.ll", ec);
        if (!ec) m.print(pre, nullptr);
        mpm.run(m, mam);
        llvm::raw_fd_ostream post(std::string(dumpDir) + "/" + m.getName().str() + ".post.ll", ec);
        if (!ec) m.print(post, nullptr);
        return;
    }
    mpm.run(m, mam);
}

/// Runs codegen to Assembly or Object in memory; false if that type cannot be emitted.
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
    // The AMDGPU backend rejects the default and static reloc models outright.
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, /*CPU=*/arch, /*Features=*/"", opt,
        /*RM=*/llvm::Reloc::PIC_);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(kAmdgpuTriple));
    m.setDataLayout(tm.createDataLayout());
    // The kernel lowerer gates per-subtarget features BEFORE codegen on this.
    m.addModuleFlag(llvm::Module::Warning, "cajeta.amdgpu.arch",
                    llvm::MDString::get(m.getContext(), tm.getTargetCPU()));
}

std::string emitIsa(llvm::Module& deviceModule, llvm::TargetMachine& tm) {
    llvm::SmallString<0> buf;
    if (!emitToBuffer(deviceModule, tm, llvm::CodeGenFileType::AssemblyFile,
                      buf)) {
        return {};
    }
    return std::string(buf.begin(), buf.end());
}

/// Per-kernel resource usage from the emitted asm's .amdgpu_metadata YAML; a
/// kernel's `.name:` is the nearest one BEFORE its `.vgpr_count:`.
std::vector<KernelResourceInfo> parseKernelResourceUsage(
        const std::string& isa) {
    auto valAfter = [](const std::string& line, const char* key) -> std::string {
        auto p = line.find(key);
        if (p == std::string::npos) return {};
        p += std::strlen(key);
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        size_t e = p;
        while (e < line.size() && line[e] != ' ' && line[e] != '\t' &&
               line[e] != '\n' && line[e] != '\r') ++e;
        return line.substr(p, e - p);
    };
    std::vector<KernelResourceInfo> out;
    std::istringstream ss(isa);
    std::string line, curName;
    for (std::string l; std::getline(ss, l);) {
        if (l.find(".name:") != std::string::npos) {
            curName = valAfter(l, ".name:");
        } else if (l.find(".vgpr_count:") != std::string::npos) {
            KernelResourceInfo k;
            k.name = curName;
            k.vgpr = std::atoi(valAfter(l, ".vgpr_count:").c_str());
            k.spill = 0;   // emitted as 0 when none; absent -> treat as none
            out.push_back(k);
        } else if (l.find(".vgpr_spill_count:") != std::string::npos &&
                   !out.empty()) {
            out.back().spill = std::atoi(valAfter(l, ".vgpr_spill_count:").c_str());
        }
    }
    return out;
}

/// Pins the flat work-group size to the real launch size, raising the VGPR budget.
void setKernelWorkgroupSize(llvm::Function* fn, unsigned maxThreads) {
    if (!fn || maxThreads == 0) return;
    if (fn->hasFnAttribute("amdgpu-flat-work-group-size")) return;
    std::string range = "1," + std::to_string(maxThreads);
    fn->addFnAttr("amdgpu-flat-work-group-size", range);
}

/// The ld.lld to invoke: $ROCM_PATH/llvm/bin, then /opt/rocm, then PATH.
std::string findLld() {
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

    // -shared makes the code object the ET_DYN ELF hipModuleLoad accepts.
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

std::vector<ArchHsaco> assembleHsacoPerArch(
        llvm::Module& deviceModule, const std::vector<std::string>& arches) {
    std::vector<ArchHsaco> out;
    if (arches.empty()) return out;
    if (arches.size() == 1) {
        auto tm = createAmdgpuTargetMachine(arches[0]);
        if (!tm) return out;
        std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, arches[0]);
        if (hsaco.empty()) return out;
        out.push_back({arches[0], std::move(hsaco)});
        return out;
    }
    for (const std::string& arch : arches) {
        auto tm = createAmdgpuTargetMachine(arch);
        if (!tm) return {};
        auto clone = llvm::CloneModule(deviceModule);   // assembleHsaco mutates
        std::vector<uint8_t> hsaco = assembleHsaco(*clone, *tm, arch);
        if (hsaco.empty()) return {};
        out.push_back({arch, std::move(hsaco)});
    }
    return out;
}

std::vector<uint8_t> bundleHsacos(const std::vector<ArchHsaco>& perArch) {
    if (perArch.empty()) return {};
    if (perArch.size() == 1) return perArch[0].hsaco;
    auto bundler = llvm::sys::findProgramByName("clang-offload-bundler");
    if (!bundler) {
        llvm::errs() << "cajeta.xpu.amd: clang-offload-bundler not found "
                        "(needed for multi-arch); set ROCM_PATH or PATH\n";
        return {};
    }

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
    for (const ArchHsaco& ah : perArch) {
        std::string f;
        if (!writeTemp("hsaco", ah.hsaco.data(), ah.hsaco.size(), f)) {
            cleanup(); return {};
        }
        targets += ",hipv4-amdgcn-amd-amdhsa--" + ah.arch;
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

std::vector<uint8_t> assembleHsacoBundle(
        llvm::Module& deviceModule, const std::vector<std::string>& arches) {
    return bundleHsacos(assembleHsacoPerArch(deviceModule, arches));
}

/// Measured per-kernel resources, read from the code object's msgpack note.
std::vector<AmdCodeObjectFootprint> readCodeObjectFootprint(
        const std::vector<uint8_t>& elfBytes) {
    std::vector<AmdCodeObjectFootprint> out;
    if (elfBytes.empty()) return out;
    llvm::StringRef data(reinterpret_cast<const char*>(elfBytes.data()),
                         elfBytes.size());
    auto elfOrErr = llvm::object::ELF64LEFile::create(data);
    if (!elfOrErr) { llvm::consumeError(elfOrErr.takeError()); return out; }
    const llvm::object::ELF64LEFile& elf = *elfOrErr;
    auto sections = elf.sections();
    if (!sections) { llvm::consumeError(sections.takeError()); return out; }

    auto uintOf = [](const llvm::msgpack::DocNode& n) -> unsigned {
        switch (n.getKind()) {
            case llvm::msgpack::Type::UInt: return (unsigned) n.getUInt();
            case llvm::msgpack::Type::Int:  return n.getInt() < 0 ? 0u : (unsigned) n.getInt();
            default: return 0;
        }
    };

    for (const auto& shdr : *sections) {
        if (shdr.sh_type != llvm::ELF::SHT_NOTE) continue;
        llvm::Error err = llvm::Error::success();
        for (const auto& note : elf.notes(shdr, err)) {
            if (note.getName() != "AMDGPU"
                    || note.getType() != llvm::ELF::NT_AMDGPU_METADATA)
                continue;
            llvm::ArrayRef<uint8_t> desc = note.getDesc(shdr.sh_addralign);
            llvm::msgpack::Document doc;
            if (!doc.readFromBlob(llvm::StringRef(
                        reinterpret_cast<const char*>(desc.data()), desc.size()),
                    /*Multi=*/false))
                continue;
            llvm::msgpack::DocNode& root = doc.getRoot();
            if (!root.isMap()) continue;
            auto& rootMap = root.getMap();
            auto kIt = rootMap.find("amdhsa.kernels");
            if (kIt == rootMap.end() || !kIt->second.isArray()) continue;
            for (llvm::msgpack::DocNode& k : kIt->second.getArray()) {
                if (!k.isMap()) continue;
                auto& km = k.getMap();
                auto str = [&](const char* key) -> std::string {
                    auto it = km.find(key);
                    if (it == km.end() || !it->second.isString()) return {};
                    return it->second.getString().str();
                };
                auto num = [&](const char* key) -> unsigned {
                    auto it = km.find(key);
                    return it == km.end() ? 0u : uintOf(it->second);
                };
                AmdCodeObjectFootprint fp;
                fp.name = str(".name");
                fp.vgpr = num(".vgpr_count");
                fp.sgpr = num(".sgpr_count");
                fp.vgprSpill = num(".vgpr_spill_count");
                fp.sgprSpill = num(".sgpr_spill_count");
                fp.privateSegmentBytes = num(".private_segment_fixed_size");
                fp.groupSegmentBytes = num(".group_segment_fixed_size");
                fp.wavefrontSize = num(".wavefront_size");
                fp.maxFlatWorkgroupSize = num(".max_flat_workgroup_size");
                out.push_back(std::move(fp));
            }
        }
        if (err) llvm::consumeError(std::move(err));
    }
    return out;
}

} // namespace amd
} // namespace xpu
} // namespace cajeta
