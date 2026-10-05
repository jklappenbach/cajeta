// A declaration the backend did not receive fails the build by name
// (xpu-kernel-independence 4.2.1.1, spec §2.3).
//
// @Occupancy and @Wave(width) were once parsed and then ignored (cajeta
// a538aa77: the declaration reached the MIR and nothing else; the nvvm
// annotations were dropped between the IR and the PTX in silence). The
// compiler now checks each declaration against what the backend produced:
// the PTX's entry header for a thread bound, the manifest's wave width for a
// declared width. A test-only lever, CAJETA_XPU_FAULT=drop-occupancy or
// drop-wave-pin, makes the backend lose the declaration again, so there is a
// test that the check fires and a test that it does not.

#include "gtest/gtest.h"

#include "../PortableEnv.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// `bounded` declares a thread bound and `plain` does not; `waved` declares a
// 32-lane wave and reduces across it, `bare` declares nothing.
const char* kProgram =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 256)\n"
    "    public static void bounded(KernelBuffer<float32> y, float32 a) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        y[i] = a * y[i];\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void plain(KernelBuffer<float32> y, float32 a) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        y[i] = a + y[i];\n"
    "    }\n"
    "    @Kernel\n"
    "    @Wave(width = 32)\n"
    "    public static void waved(KernelBuffer<float32> out) {\n"
    "        uint32 lane = KernelThread.x() % 32;\n"
    "        out[KernelThread.globalIdX()] = Wave.reduceSumF32((float32) lane);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void bare(KernelBuffer<float32> out) {\n"
    "        out[KernelThread.globalIdX()] = Wave.reduceSumF32(1.0f);\n"
    "    }\n"
    "    public static void main(String[] args) { }\n"
    "}\n";

struct Built {
    int rc = -1;
    std::string log;
};

Built build(const std::string& backend, const std::string& fault) {
    Built b;
    std::string bin = sourceRoot() + "/build/src/cajeta";
    if (!fs::exists(bin)) return b;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_xpu_decl_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "test");
    fs::create_directories(root / "out");
    std::ofstream(root / "src" / "test" / "M.cajeta") << kProgram;
    std::string prefix = fault.empty() ? "" : cajeta_env_prefix({{"CAJETA_XPU_FAULT", fault}});
    std::string cmd = prefix + "\"" + bin + "\" --emit=obj --xpu-backend=" + backend
        + " test.M.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + (root / "build.log").string() + "\" 2>&1";
    int raw = std::system(cmd.c_str());
#if defined(_WIN32)
    b.rc = raw;
#else
    b.rc = WIFEXITED(raw) ? WEXITSTATUS(raw) : raw;
#endif
    b.log = readFile(root / "build.log");
    std::error_code ec;
    fs::remove_all(root, ec);
    return b;
}

bool haveCompiler() { return fs::exists(sourceRoot() + "/build/src/cajeta"); }

} // namespace

// The thread bound is dropped between the IR and the PTX: the build fails,
// naming the kernel, the backend and the declaration, and not the kernel
// that declared nothing.
TEST(XpuDeclarationCheck, anOccupancyBoundMissingFromThePtxFailsByName) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build("nvptx", "drop-occupancy");
    EXPECT_NE(b.rc, 0) << "a bound the PTX does not carry must fail the build:\n" << b.log;
    EXPECT_NE(b.log.find("bounded"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("@Occupancy"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("nvptx"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find(".maxntid"), std::string::npos) << "the PTX directive it looked for:\n" << b.log;
    EXPECT_EQ(b.log.find("plain"), std::string::npos)
        << "the kernel that declared nothing must not be named:\n" << b.log;
}

// The counterpart: the bound lands and the build passes.
TEST(XpuDeclarationCheck, anOccupancyBoundThatReachesThePtxPasses) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    if (cajeta::xpu::nvidia::findPtxas().empty()) GTEST_SKIP() << "ptxas not found";
    Built b = build("nvptx", "");
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_EQ(b.log.find("@Occupancy"), std::string::npos) << b.log;
}

// The declared width is not pinned on the cpu backend, which then builds the
// kernel at the host's width: the build fails naming the kernel, the
// backend, the declaration and the width it got, and not the kernel that
// declared nothing.
TEST(XpuDeclarationCheck, aWaveWidthNotPinnedOnCpuFailsByName) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build("cpu", "drop-wave-pin");
    EXPECT_NE(b.rc, 0) << "a declared width the backend did not build must fail:\n" << b.log;
    EXPECT_NE(b.log.find("waved"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("@Wave(width = 32)"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("cpu"), std::string::npos) << b.log;
    EXPECT_EQ(b.log.find("bare"), std::string::npos)
        << "the kernel that declared nothing must not be named:\n" << b.log;
}

// The counterpart: the width is pinned and the build passes.
TEST(XpuDeclarationCheck, aWaveWidthThatIsPinnedPasses) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build("cpu", "");
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_EQ(b.log.find("@Wave(width = 32)"), std::string::npos) << b.log;
}
