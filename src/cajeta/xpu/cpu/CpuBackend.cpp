// CPU backend — see header. Host TargetMachine + native object emit.

#include "CpuBackend.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <string>

namespace cajeta {
namespace xpu {
namespace cpu {

namespace {

void ensureNativeTargetInitialized() {
    static bool done = []() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        return true;
    }();
    (void) done;
}

} // namespace

bool cpuLevelTarget(const std::string& level, std::string* cpu, std::string* features) {
    if (level == "v3")      { *cpu = "x86-64-v3"; *features = ""; return true; }
    if (level == "v3-vnni") { *cpu = "x86-64-v3"; *features = "+avxvnni"; return true; }
    if (level == "v4")      { *cpu = "x86-64-v4"; *features = "+avx512vnni"; return true; }
    if (level == "v4-amx")  { *cpu = "x86-64-v4"; *features = "+avx512vnni,+amx-tile,+amx-int8"; return true; }
    return false;
}

int cpuLevelIndex(const std::string& level) {
    static const char* const names[4] = {"v3", "v3-vnni", "v4", "v4-amx"};
    for (int i = 0; i < 4; ++i) if (level == names[i]) return i;
    return -1;
}

std::unique_ptr<llvm::TargetMachine> createCpuTargetMachine(const std::string& level) {
    ensureNativeTargetInitialized();
    llvm::Triple triple(llvm::sys::getProcessTriple());
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        llvm::errs() << "cajeta.xpu.cpu: host target not available: " << error
                     << "\n";
        return nullptr;
    }
    // Target the HOST CPU and its native features, so TargetTransformInfo lets
    // LoopVectorize cost-model real SIMD; AOT objects are then tuned to this machine.
    std::string cpu = std::string(llvm::sys::getHostCPUName());
    std::string features;
    {
        llvm::SubtargetFeatures sf;
        for (const auto& f : llvm::sys::getHostCPUFeatures())
            sf.AddFeature(f.first(), f.second);
        features = sf.getString();
    }
    // Debug overrides: reproduce another host's cost-model decisions on this box.
    if (const char* mcpu = std::getenv("CAJETA_XPU_CPU_MCPU")) cpu = mcpu;
    if (const char* mattr = std::getenv("CAJETA_XPU_CPU_MATTR")) features = mattr;
    // A named level is exact: its cpu and features, nothing of the host's.
    if (!level.empty() && level != "host" && !cpuLevelTarget(level, &cpu, &features)) {
        llvm::errs() << "cajeta.xpu.cpu: unknown ISA level '" << level << "'\n";
        return nullptr;
    }
    llvm::TargetOptions opt;
    llvm::TargetMachine* tm = target->createTargetMachine(
        triple, cpu, features, opt, /*RM=*/llvm::Reloc::PIC_);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

void configureHostModule(llvm::Module& m, llvm::TargetMachine& tm) {
    m.setTargetTriple(llvm::Triple(llvm::sys::getProcessTriple()));
    m.setDataLayout(tm.createDataLayout());
}

std::vector<uint8_t> emitObject(llvm::Module& m, llvm::TargetMachine& tm) {
    llvm::SmallVector<char, 0> buf;
    llvm::raw_svector_ostream os(buf);
    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, os, /*DwoOut=*/nullptr,
                               llvm::CodeGenFileType::ObjectFile)) {
        llvm::errs() << "cajeta.xpu.cpu: host TargetMachine cannot emit an "
                        "object file\n";
        return {};
    }
    pm.run(m);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

std::string kernelTuneCpu() {
    const char* env = std::getenv("CAJETA_XPU_CPU_TUNE");
    if (env == nullptr || env[0] == 0) return "generic";
    std::string tune = env;
    if (tune == "host" || tune == "native") return std::string();
    return tune;
}

void tuneKernelWrapper(llvm::Function& wrapper) {
    std::string tune = kernelTuneCpu();
    if (tune.empty()) return;
    wrapper.addFnAttr("tune-cpu", tune);
}

} // namespace cpu
} // namespace xpu
} // namespace cajeta
