//
// CPU backend — see header. Host TargetMachine + native object emit.
//

#include "CpuBackend.h"

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

std::unique_ptr<llvm::TargetMachine> createCpuTargetMachine() {
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
    // Target the HOST CPU + its native features (AVX2/AVX-512/FMA/...), so
    // TargetTransformInfo lets LoopVectorize cost-model real SIMD for the CPU
    // backend's per-block kernel wrapper (Inc 5B) and codegen emits matching
    // vector instructions. Tradeoff: AOT objects are tuned to this machine —
    // a `--cpu-arch` baseline knob for portable binaries is a later refinement.
    std::string cpu = std::string(llvm::sys::getHostCPUName());
    std::string features;
    {
        llvm::SubtargetFeatures sf;
        for (const auto& f : llvm::sys::getHostCPUFeatures())
            sf.AddFeature(f.first(), f.second);
        features = sf.getString();
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

} // namespace cpu
} // namespace xpu
} // namespace cajeta
