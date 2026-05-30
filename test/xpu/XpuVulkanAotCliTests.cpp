//
// CajetaXPU Vulkan bring-up (Increment 3) — AOT `--xpu-*` CLI path for the
// vulkan backend.
//
// The Vulkan twin of XpuAmdgpuAotCliTests: driving Compiler::compile with
// xpuBackend=Vulkan + xpuEmit=Spvasm writes a per-kernel `.spvasm` (SPIR-V
// disassembly) next to the module's IR output; xpuEmit=Spirv writes the `.spv`
// binary. SPIR-V emission needs only the in-tree SPIR-V TargetMachine — no
// external assembler, no GPU — so both cases are fully portable. When spirv-val
// is installed, the emitted .spv is validated as a Vulkan 1.3 module.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"

#include "llvm/Support/Program.h"

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
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
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

std::pair<fs::path, fs::path> makeProject() {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path() / ("cajeta_xpu_vkaot_" + std::to_string(rng()));
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

// --xpu-backend=vulkan --xpu-emit=spvasm writes a per-kernel .spvasm alongside
// the IR output, carrying the GLCompute entry. No GPU needed.
TEST(XpuVulkanAotCliTests, vulkanBackendEmitsSpvasmArtifact) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Vulkan);
    compiler.setXpuEmit(XpuEmit::Spvasm);
    compiler.setXpuArch("vulkan1.3");
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto spvasmPath = findArtifact(build, ".spvasm");
    ASSERT_FALSE(spvasmPath.empty()) << "no .spvasm written under " << build;
    std::string text = readFile(spvasmPath);
    EXPECT_NE(text.find("OpEntryPoint GLCompute"), std::string::npos) << text;
    EXPECT_NE(text.find("LocalSize"), std::string::npos) << text;

    fs::remove_all(src.parent_path());
}

// --xpu-emit=spirv writes the Khronos SPIR-V binary; when spirv-val is present,
// it must be a valid Vulkan 1.3 module.
TEST(XpuVulkanAotCliTests, vulkanBackendEmitsSpirvBinary) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Vulkan);
    compiler.setXpuEmit(XpuEmit::Spirv);
    compiler.setXpuArch("vulkan1.3");
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto spvPath = findArtifact(build, ".spv");
    ASSERT_FALSE(spvPath.empty()) << "no .spv written under " << build;
    std::string bytes = readFile(spvPath);
    ASSERT_GE(bytes.size(), 4u);
    // SPIR-V magic 0x07230203 (little-endian).
    EXPECT_EQ((unsigned char) bytes[0], 0x03u);
    EXPECT_EQ((unsigned char) bytes[1], 0x02u);
    EXPECT_EQ((unsigned char) bytes[2], 0x23u);
    EXPECT_EQ((unsigned char) bytes[3], 0x07u);

    if (auto tool = llvm::sys::findProgramByName("spirv-val")) {
        llvm::StringRef env = "--target-env";
        llvm::StringRef ver = "vulkan1.3";
        llvm::StringRef file = spvPath.c_str();
        llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
        int rc = llvm::sys::ExecuteAndWait(*tool, args);
        EXPECT_EQ(rc, 0) << "spirv-val rejected the AOT-emitted .spv";
    }

    fs::remove_all(src.parent_path());
}

// Default backend (None) is host-only: no Vulkan artifact even with a @Kernel.
TEST(XpuVulkanAotCliTests, defaultBackendEmitsNoSpirvArtifact) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.compile("test.M.saxpy", src.string(), build.string());

    EXPECT_TRUE(findArtifact(build, ".spv").empty());
    EXPECT_TRUE(findArtifact(build, ".spvasm").empty());

    fs::remove_all(src.parent_path());
}
