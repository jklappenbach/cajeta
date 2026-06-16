//
// Cajeta XPU — proof-of-support compute probes, AMD (gfx1151) EMIT gate.
//
// The third rung of the proof-of-support matrix. CPU (bit-exact oracle) and
// Vulkan/RADV (on real metal) run the probes end to end; AMD in-process device
// JIT is blocked by a libamd_comgr symbol collision (see project memory), so we
// can't execute the full run() on AMD here. But ISA emission needs only the
// AMDGPU TargetMachine (no GPU, no lld), so we CAN prove the probe *kernels*
// lower to valid gfx1151 AMDGCN — including that the float/int atomics in the
// numerics and PyTorch probes become real device atomic instructions. This is
// the AMD twin of XpuAmdgpuAotCliTests, run over the SAME probe sources
// (ComputeProbeSources.h) the CPU + Vulkan gates use.
//

#include "gtest/gtest.h"

#include "ComputeProbeSources.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using cajeta::Compiler;
using cajeta::EmitMode;
using cajeta::XpuBackend;
using cajeta::XpuEmit;

namespace {

namespace fs = std::filesystem;

// Write `source` for fully-qualified `fqClass` (e.g. "test.NumProbe") into a
// fresh temp project laid out as <base>/src/<pkg-path>/<Class>.cajeta, and
// return (srcRoot, buildDir) for Compiler::compile.
std::pair<fs::path, fs::path> makeProject(const std::string& source,
                                          const std::string& fqClass) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xpu_probe_amd_" + std::to_string(rng()));
    auto src = base / "src";
    fs::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClass.size(); ++i) {
        if (i == fqClass.size() || fqClass[i] == '.') {
            rel /= fqClass.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = src / rel;
    fs::create_directories(full.parent_path());
    fs::create_directories(base / "build");
    std::ofstream(full) << source;
    return {src, base / "build"};
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

// A module with multiple @Kernel methods emits one .isa per kernel, so pick the
// one carrying `.amdhsa_kernel <kernelName>` (not just the first .isa found).
std::string findKernelIsa(const fs::path& root, const std::string& kernelName) {
    if (!fs::exists(root)) return {};
    const std::string marker = ".amdhsa_kernel " + kernelName;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".isa") continue;
        std::string isa = readFile(e.path());
        if (isa.find(marker) != std::string::npos) return isa;
    }
    return {};
}

// Compile one @Kernel method to gfx1151 .isa and return the ISA text. `entry` is
// "test.Class.method"; asserts a non-empty .isa carrying that kernel + the arch.
std::string emitKernelIsa(const std::string& source, const std::string& fqClass,
                          const std::string& entry,
                          const std::string& kernelName) {
    auto [src, build] = makeProject(source, fqClass);
    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Amdgpu);
    compiler.setXpuEmit(XpuEmit::Isa);
    compiler.setXpuArch("gfx1151");
    compiler.compile(entry, src.string(), build.string());

    std::string isa = findKernelIsa(build, kernelName);
    EXPECT_FALSE(isa.empty())
        << "no .isa carrying '.amdhsa_kernel " << kernelName << "' under " << build;
    EXPECT_NE(isa.find("gfx1151"), std::string::npos) << isa;

    fs::remove_all(src.parent_path());
    return isa;
}

bool hasAtomic(const std::string& isa) {
    return isa.find("atomic") != std::string::npos;   // *_atomic_* mnemonic
}

} // namespace

// Target 1: numerics — the broadcast+reduction kernel lowers to gfx1151 AMDGCN,
// and its float atomicAdd becomes a real device atomic instruction.
TEST(XpuComputeProbeAmdEmitTests, numericsBroadcastReduceEmitsAmdIsa) {
    std::string isa = emitKernelIsa(cajeta_test_probes::kNumericsBroadcastReduce(),
                                    "test.NumProbe",
                                    "test.NumProbe.broadcastReduce",
                                    "broadcastReduce");
    EXPECT_TRUE(hasAtomic(isa)) << "float atomicAdd did not lower to an AMD atomic\n"
                                << isa;
}

// Target 2: PyTorch — the matmul kernel lowers to gfx1151 AMDGCN.
TEST(XpuComputeProbeAmdEmitTests, torchMatmulEmitsAmdIsa) {
    emitKernelIsa(cajeta_test_probes::kTorchMatmulScatter(), "test.TorchProbe",
                  "test.TorchProbe.matmul", "matmul");
}

// Target 2: PyTorch — the atomic scatter-add kernel lowers to gfx1151 AMDGCN,
// with its float atomicAdd as a real device atomic.
TEST(XpuComputeProbeAmdEmitTests, torchScatterAddEmitsAmdIsa) {
    std::string isa = emitKernelIsa(cajeta_test_probes::kTorchMatmulScatter(),
                                    "test.TorchProbe",
                                    "test.TorchProbe.scatterAdd", "scatterAdd");
    EXPECT_TRUE(hasAtomic(isa)) << "scatter-add atomicAdd did not lower to an AMD atomic\n"
                                << isa;
}

// Target 3: Toffee/SPELA — the fused forward+loss+update kernel lowers to gfx1151.
TEST(XpuComputeProbeAmdEmitTests, spelaFusedLayerEmitsAmdIsa) {
    emitKernelIsa(cajeta_test_probes::kSpelaFusedLayer(), "test.SpelaProbe",
                  "test.SpelaProbe.fusedLayer", "fusedLayer");
}
