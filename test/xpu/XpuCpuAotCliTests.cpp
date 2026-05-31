//
// CajetaXPU CPU backend (Increment 2) — AOT `--xpu-*` path: enum + registration
// + multi-target list + native object emit.
//
// Unlike the GPU backends, the CPU kernel is host code linked into the module,
// so registration emits a `__cajeta_xpu_cpu.<name>` function + a global ctor
// calling `__cajeta_xpu_register_cpu_kernel`. The multi-target case
// (vulkan,cpu) bundles BOTH a SPIR-V registration and a CPU registration into
// one module — the foundation for runtime fall-to-CPU. `--xpu-emit=obj` drops a
// native relocatable object. All GPU-free.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"

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
    auto base = fs::temp_directory_path() / ("cajeta_xpu_cpuaot_" + std::to_string(rng()));
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
    for (auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ext) return e.path();
    return {};
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

} // namespace

// --xpu-backend=cpu links the kernel into the module as host code and emits a
// CPU registration ctor — visible in the .ll output.
TEST(XpuCpuAotCliTests, cpuBackendEmitsHostKernelAndRegistration) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Cpu);
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto llPath = findArtifact(build, ".ll");
    ASSERT_FALSE(llPath.empty()) << "no .ll written under " << build;
    std::string ir = readFile(llPath);
    // The host kernel function (decorated) + the CPU registration call + a ctor.
    EXPECT_NE(ir.find("__cajeta_xpu_cpu.saxpy"), std::string::npos) << ir;
    EXPECT_NE(ir.find("__cajeta_xpu_register_cpu_kernel"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.global_ctors"), std::string::npos) << ir;
    // No device-blob registration on the pure-CPU path.
    EXPECT_EQ(ir.find("__cajeta_xpu_register_module"), std::string::npos) << ir;

    fs::remove_all(src.parent_path());
}

// --xpu-backend=vulkan,cpu bundles BOTH registrations into one module: the
// SPIR-V device blob AND the CPU host kernel — the basis for runtime fallback.
TEST(XpuCpuAotCliTests, multiTargetBundlesVulkanAndCpu) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Vulkan);   // clears, sets vulkan
    compiler.addXpuBackend(XpuBackend::Cpu);       // + cpu
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto llPath = findArtifact(build, ".ll");
    ASSERT_FALSE(llPath.empty()) << "no .ll written under " << build;
    std::string ir = readFile(llPath);
    // Vulkan: device-blob registration.
    EXPECT_NE(ir.find("__cajeta_xpu_register_module"), std::string::npos) << ir;
    EXPECT_NE(ir.find("xpu.spirv.saxpy"), std::string::npos) << ir;
    // CPU: host kernel + pointer registration.
    EXPECT_NE(ir.find("__cajeta_xpu_register_cpu_kernel"), std::string::npos) << ir;
    EXPECT_NE(ir.find("__cajeta_xpu_cpu.saxpy"), std::string::npos) << ir;

    fs::remove_all(src.parent_path());
}

// --xpu-emit=obj drops a native relocatable object for the kernel.
TEST(XpuCpuAotCliTests, cpuBackendEmitsObjectArtifact) {
    auto [src, build] = makeProject();

    Compiler compiler;
    compiler.setEmitMode(EmitMode::IR);
    compiler.setXpuBackend(XpuBackend::Cpu);
    compiler.setXpuEmit(XpuEmit::Object);
    compiler.compile("test.M.saxpy", src.string(), build.string());

    auto objPath = findArtifact(build, ".o");
    ASSERT_FALSE(objPath.empty()) << "no .o written under " << build;
    std::string bytes = readFile(objPath);
    ASSERT_GE(bytes.size(), 4u);
    // ELF magic 0x7f 'E' 'L' 'F' (host is Linux here).
    EXPECT_EQ((unsigned char) bytes[0], 0x7Fu);
    EXPECT_EQ(bytes[1], 'E');
    EXPECT_EQ(bytes[2], 'L');
    EXPECT_EQ(bytes[3], 'F');

    fs::remove_all(src.parent_path());
}
