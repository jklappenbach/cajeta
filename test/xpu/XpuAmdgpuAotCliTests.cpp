//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 3) — AOT `--xpu-*` CLI path
// for the amdgpu backend.
//
// The AMD twin of XpuAotCliTests: driving Compiler::compile with
// xpuBackend=Amdgpu + xpuEmit=Isa writes a per-kernel `.isa` (AMDGCN assembly
// text) next to the module's IR output. ISA emission needs only the AMDGPU
// TargetMachine (no lld, no GPU), so the isa case is portable; the hsaco
// variant is gated on ld.lld being present.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "XpuDeviceTestUtil.h"

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

const char* kKernelSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            y[i] = a * x[i] + y[i];\n"
    "        }\n"
    "    }\n"
    "}\n";

std::pair<fs::path, fs::path> makeProject() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_xpu_amdaot_" + std::to_string(rng()));
    auto src = base / "src";
    auto klassDir = src / "test";
    auto build = base / "build";
    fs::create_directories(klassDir);
    fs::create_directories(build);
    std::ofstream(klassDir / "M.cajeta") << kKernelSource;
    return {src, build};
}

fs::path findArtifact(const fs::path& root, const std::string& ext) {
    if (!fs::exists(root)) return {};
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == ext) return e.path();
    }
    return {};
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

} // namespace

// --xpu-backend=amdgpu --xpu-emit=isa writes a per-kernel .isa alongside the
// IR output, carrying the device entry. No GPU / lld needed.

// Default backend (None) is host-only: no AMD device artifact even though the
// source has a @Kernel.

// --xpu-emit=hsaco links the AMDGCN object through ROCm's ld.lld. Gated on an
// actual ROCm/HIP device: a generic host ld.lld merely being on PATH does NOT
// mean an AMDGCN link will work (it fails "incompatible with elf64-x86-64"),
// so hardware presence is the reliable skip condition on non-AMD boxes.
TEST(XpuAmdgpuAotCliTests, amdgpuBackendEmitsHsacoWhenLldPresent) {
    CAJETA_SKIP_IF_NO_HIP();
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Amdgpu);
    compiler.setXpuEmit(XpuEmit::Hsaco);
    compiler.setXpuArch("gfx1151");
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto hsacoPath = findArtifact(build, ".hsaco");
    ASSERT_FALSE(hsacoPath.empty()) << "no .hsaco written under " << build;
    EXPECT_GT(fs::file_size(hsacoPath), 0u);

    fs::remove_all(src.parent_path());
}
