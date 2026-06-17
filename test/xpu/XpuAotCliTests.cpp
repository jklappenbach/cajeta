//
// CajetaXPU — AOT `--xpu-*` CLI path.
//
// The NVPTX device path used to run only through the JIT test helper. These
// pin the AOT seam: driving `Compiler::compile(entry, sourceRoot, archive)`
// with `xpuBackend=Nvptx` + `xpuEmit=Ptx` writes a per-kernel `.ptx` next to
// the module's IR output, and the default (`xpuBackend=None`) leaves the
// host-only emit untouched. PTX emission needs only the NVPTX TargetMachine
// (no ptxas, no GPU), so these are portable; the cubin variant is gated on
// ptxas being present.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

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

// A minimal SAXPY-class @Kernel — the lowering subset proven by
// XpuNvptxEmitTests. Lives at <root>/src/test/M.cajeta.
const char* kKernelSource =
    "package test;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            y[i] = a * x[i] + y[i];\n"
    "        }\n"
    "    }\n"
    "}\n";

// Lay down a fresh temp source tree + build dir; return {sourceRoot, archiveRoot}.
std::pair<fs::path, fs::path> makeProject() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_xpu_aot_" + std::to_string(rng()));
    auto src = base / "src";
    auto klassDir = src / "test";
    auto build = base / "build";
    fs::create_directories(klassDir);
    fs::create_directories(build);
    std::ofstream(klassDir / "M.cajeta") << kKernelSource;
    return {src, build};
}

// Find the first file with `ext` (e.g. ".ptx") anywhere under `root`.
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

// --xpu-backend=nvptx --xpu-emit=ptx writes a per-kernel .ptx alongside the
// IR output, carrying the device entry. No GPU / ptxas needed.
TEST(XpuAotCliTests, nvptxBackendEmitsPtxArtifact) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Nvptx);
    compiler.setXpuEmit(XpuEmit::Ptx);
    compiler.setXpuArch("sm_89");
    // entryMethod is only consulted for the C main shim (obj/exe); IR emit
    // ignores it, so a dotted placeholder is fine.
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto ptxPath = findArtifact(build, ".ptx");
    ASSERT_FALSE(ptxPath.empty()) << "no .ptx written under " << build;
    std::string ptx = readFile(ptxPath);
    EXPECT_NE(ptx.find(".visible .entry saxpy"), std::string::npos) << ptx;

    fs::remove_all(src.parent_path());
}

// Default backend (None) is host-only: no device artifact is produced even
// though the source contains a @Kernel.
TEST(XpuAotCliTests, defaultBackendEmitsNoDeviceArtifact) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    // Leave xpuBackend at its default (None).
    compiler.compile("test.M.saxpy", src.string(), build.string());

    EXPECT_TRUE(findArtifact(build, ".ptx").empty());
    EXPECT_TRUE(findArtifact(build, ".cubin").empty());

    fs::remove_all(src.parent_path());
}

// --xpu-emit=cubin assembles through ptxas. Gated on ptxas being present so
// the suite stays green on boxes without the CUDA toolkit.
TEST(XpuAotCliTests, nvptxBackendEmitsCubinWhenPtxasPresent) {
    if (cajeta::xpu::nvidia::findPtxas().empty()) {
        GTEST_SKIP() << "ptxas not found; skipping cubin assembly";
    }
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Nvptx);
    compiler.setXpuEmit(XpuEmit::Cubin);
    compiler.setXpuArch("sm_89");
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto cubinPath = findArtifact(build, ".cubin");
    ASSERT_FALSE(cubinPath.empty()) << "no .cubin written under " << build;
    EXPECT_GT(fs::file_size(cubinPath), 0u);

    fs::remove_all(src.parent_path());
}
