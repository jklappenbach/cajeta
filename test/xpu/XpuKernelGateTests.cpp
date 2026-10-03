//
// The xpu kernel gate (xpu-kernel-adaptor 4.1.2 / 4.1.3 / 4.2.2 / 4.2.3).
//
// THE RULE, decided by Julian 2026-09-25 ("Unit 4: make it an error"): a
// @Kernel that produces no device code for a backend the build declares
// FAILS THE BUILD, by name, with the backend and the lowering's reason --
// or it names the plan item holding it, `@Unlowered(backend = "cpu",
// tracked = "xpu-kernel-adaptor 4.2.1")`, and is then a countable note. The
// same rule as the device-skip gate (1.6.5) and the kernel census (Unit 6),
// applied where the kernels go missing rather than where the tests do; and
// like both, a held kernel that starts lowering is STALE and fails until
// its declaration is removed, so the inventory cannot outlive its cause.
//
// The lines are the compiler binary's, read off a real build, because the
// verdict is the build's exit code and the JIT harness (which sweeps by
// default) cannot testify to that. `CAJETA_XPU_KERNEL_GATE=warn` demotes
// the gate for a sweep, the §5.5 switch pattern of the ownership checks.
//
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using cajeta_test::CajetaJit;

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
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// A kernel the cpu backend cannot lower (a heap allocation inside the body,
// the construct XpuCpuVectorLadderTests' unlowerable probe uses) and one it
// can, in one file, so 4.1.3 is testable: the gate names the one and not
// the other. `ann` goes on the unlowerable kernel, `goodAnn` on the other.
std::string program(const std::string& ann, const std::string& goodAnn = "") {
    return
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n" + ann +
        "    public static void bad(KernelBuffer<int32> out) {\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        if (g < 1) {\n"
        "            int32[] scratch #= heap int32[4];\n"
        "            scratch[0] = 7;\n"
        "            out[0] = scratch[0];\n"
        "        }\n"
        "    }\n"
        "    @Kernel\n" + goodAnn +
        "    public static void good(KernelBuffer<float32> y, float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { y[i] = a * y[i]; }\n"
        "    }\n"
        "    public static void main(String[] args) { }\n"
        "}\n";
}

struct Built {
    int rc = -1;
    std::string log;
};

Built build(const std::string& source, const std::string& backend,
            const std::string& envPrefix = "") {
    Built b;
    std::string bin = sourceRoot() + "/build/src/cajeta";
    if (!fs::exists(bin)) return b;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path()
                  / ("cajeta_xpu_gate_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "test");
    fs::create_directories(root / "out");
    std::ofstream(root / "src" / "test" / "M.cajeta") << source;
    std::string cmd = envPrefix + "\"" + bin + "\" --emit=obj --xpu-backend=" + backend
        + " test.M.main \"" + (root / "src").string() + "\" \""
        + (root / "out").string() + "\" > \"" + (root / "build.log").string()
        + "\" 2>&1";
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

const char* TRACKED = "    @Unlowered(backend = \"cpu\", tracked = \"xpu-kernel-adaptor 4.2.1\")\n";

}  // namespace

// 4.1.2: the diagnostic names the kernel AND the target; 4.2.2 / 4.2.3: it
// is an error and the build fails. 4.1.3: it does not fire on the rest of
// the file.
TEST(XpuKernelGate, anUnloweredKernelFailsTheBuildByName) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(""), "cpu");
    EXPECT_NE(b.rc, 0) << "a kernel with no cpu device code must fail the build:\n" << b.log;
    EXPECT_NE(b.log.find("cajeta: error: [xpu-kernel-skipped] bad: no cpu device code"),
              std::string::npos) << b.log;
    EXPECT_EQ(b.log.find("[xpu-kernel-skipped] good"), std::string::npos)
        << "the lowerable kernel in the same file must not be named:\n" << b.log;
    // The remedy is in the message: the declaration that holds it.
    EXPECT_NE(b.log.find("@Unlowered("), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("CAJETA_ERROR_XPU_KERNEL_GATE"), std::string::npos) << b.log;
}

// The tracked escape: the kernel names the plan item holding it and the
// build passes with a NOTE that carries the item, the census's countable
// shape (`[tracked: <item>]`, the 1.6.5 spelling).
TEST(XpuKernelGate, aTrackedUnloweredKernelIsANoteNamingTheItem) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(TRACKED), "cpu");
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: note: [xpu-kernel-skipped] bad: no cpu device code"),
              std::string::npos) << b.log;
    EXPECT_NE(b.log.find("[tracked: xpu-kernel-adaptor 4.2.1]"), std::string::npos) << b.log;
    EXPECT_EQ(b.log.find("cajeta: error:"), std::string::npos) << b.log;
}

// STALE: a held kernel that lowers now fails until the declaration goes,
// exactly as a tracked census skip that starts running does.
TEST(XpuKernelGate, aStaleUnloweredDeclarationFailsTheBuild) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(TRACKED, TRACKED), "cpu");
    EXPECT_NE(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: error: [xpu-kernel-skipped] good: STALE"), std::string::npos)
        << "`good` lowers on cpu, so its @Unlowered(backend = \"cpu\") is stale:\n" << b.log;
    // The genuinely held one is still a note beside it.
    EXPECT_NE(b.log.find("cajeta: note: [xpu-kernel-skipped] bad"), std::string::npos) << b.log;
}

// The declaration is per backend: holding a kernel on nvptx says nothing
// about cpu, where it still fails.
TEST(XpuKernelGate, aDeclarationForAnotherBackendDoesNotHold) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(
        "    @Unlowered(backend = \"nvptx\", tracked = \"xpu-kernel-adaptor 4.2.1\")\n"), "cpu");
    EXPECT_NE(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: error: [xpu-kernel-skipped] bad"), std::string::npos) << b.log;
}

// hostWaveBelow = N: the hold is conditional on the cpu host's own wave (8 on
// AVX2, 16 on AVX-512). Found 2026-09-30: the id down-combine kernels
// vectorize at a declared 32 lanes on a 16-wide host and not on an 8-wide
// one, so an unconditional hold was right on one box and STALE on the other.
// Both arms, whatever this host is: a bound above every host holds (a note),
// a bound of 2 holds nowhere (the error), and neither claims the host's width.
TEST(XpuKernelGate, aHoldBelowAHostWaveAboveThisHostHolds) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(
        "    @Unlowered(backend = \"cpu\", tracked = \"xpu-kernel-adaptor 4.2.1.13\", "
        "hostWaveBelow = 1024)\n"), "cpu");
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: note: [xpu-kernel-skipped] bad"), std::string::npos) << b.log;
    EXPECT_EQ(b.log.find("cajeta: error:"), std::string::npos) << b.log;
}

TEST(XpuKernelGate, aHoldBelowAHostWaveThisHostReachesDoesNotHold) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(
        "    @Unlowered(backend = \"cpu\", tracked = \"xpu-kernel-adaptor 4.2.1.13\", "
        "hostWaveBelow = 2)\n"), "cpu");
    EXPECT_NE(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: error: [xpu-kernel-skipped] bad"), std::string::npos) << b.log;
}

// And the stale side: a lowerable kernel held with a bound this host reaches
// is not STALE, because the hold does not apply here.
TEST(XpuKernelGate, aConditionalHoldThisHostReachesIsNotStale) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(TRACKED,
        "    @Unlowered(backend = \"cpu\", tracked = \"xpu-kernel-adaptor 4.2.1.13\", "
        "hostWaveBelow = 2)\n"), "cpu");
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_EQ(b.log.find("STALE"), std::string::npos) << b.log;
}

// Below its bound a conditional hold PERMITS lowering; it does not demand
// that the kernel fail. Found 2026-10-03: on an Intel AVX-512 host tuned to
// prefer 256-bit vectors (GitHub's ubuntu-latest), the host wave reads 8, so
// hostWaveBelow = 16 applied, yet the id down-combine kernels lowered at 32
// lanes using AVX-512's 32 registers. The gate called the hold STALE, and
// cajeta-llm 0.1.0 failed to build there (cajeta-cabra CI 37129095762).
// Reproduced on Phoenix with CAJETA_XPU_CPU_MCPU=icelake-server and
// +prefer-256-bit. A conditional hold is stale nowhere: below N either
// outcome is allowed, and at or above N it does not apply.
TEST(XpuKernelGate, aConditionalHoldBelowItsBoundAllowsTheKernelToLower) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(TRACKED,
        "    @Unlowered(backend = \"cpu\", tracked = \"xpu-kernel-adaptor 4.2.1.13\", "
        "hostWaveBelow = 1024)\n"), "cpu");
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_EQ(b.log.find("STALE"), std::string::npos) << b.log;
}

// Every failing kernel is named before the build fails, so a build with 53
// of them reports 53 rather than the first.
TEST(XpuKernelGate, everyUnloweredKernelIsNamedBeforeTheBuildFails) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    std::string src = program("");
    // A second unlowerable kernel, untracked too.
    src.replace(src.find("    @Kernel\n    public static void good"), 0,
        "    @Kernel\n"
        "    public static void bad2(KernelBuffer<int32> out) {\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        if (g < 1) {\n"
        "            int32[] scratch #= heap int32[4];\n"
        "            scratch[0] = 9;\n"
        "            out[0] = scratch[0];\n"
        "        }\n"
        "    }\n");
    Built b = build(src, "cpu");
    EXPECT_NE(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: error: [xpu-kernel-skipped] bad:"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("cajeta: error: [xpu-kernel-skipped] bad2:"), std::string::npos) << b.log;
    EXPECT_NE(b.log.find("2 kernel(s)"), std::string::npos)
        << "the closing error counts them:\n" << b.log;
}

// The sweep switch: CAJETA_XPU_KERNEL_GATE=warn demotes the error to a
// warning and the build passes. Never the default.
TEST(XpuKernelGate, theSweepSwitchDemotesTheGateToAWarning) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(""), "cpu",
                    cajeta_env_prefix({{"CAJETA_XPU_KERNEL_GATE", "warn"}}));
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: warning: [xpu-kernel-skipped] bad: no cpu device code"),
              std::string::npos) << b.log;
    EXPECT_EQ(b.log.find("cajeta: error:"), std::string::npos) << b.log;
}

// The sweep switch demotes a STALE hold too. The cpu backend's wave width
// follows the host's SIMD width, so a hold written on an AVX2 host is stale on
// an AVX-512 one, and a sweep there must still build.
TEST(XpuKernelGate, theSweepSwitchDemotesAStaleHoldToAWarning) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    Built b = build(program(TRACKED, TRACKED), "cpu",
                    cajeta_env_prefix({{"CAJETA_XPU_KERNEL_GATE", "warn"}}));
    EXPECT_EQ(b.rc, 0) << b.log;
    EXPECT_NE(b.log.find("cajeta: warning: [xpu-kernel-skipped] good: STALE"),
              std::string::npos) << b.log;
    EXPECT_EQ(b.log.find("cajeta: error:"), std::string::npos) << b.log;
}

// The JIT harness sweeps by default (a dozen suites deliberately compile a
// kernel a backend refuses and probe the refused LAUNCH), and gates when a
// test asks, so the in-process failure is testable too.
TEST(XpuKernelGate, theJitHarnessSweepsUnlessAskedToGate) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(program(""), "test.M", o);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_NE(err.find("cajeta: warning: [xpu-kernel-skipped] bad: no cpu device code"),
              std::string::npos) << err;

    o.xpuKernelGateErrors = true;
    testing::internal::CaptureStderr();
    bool threw = false;
    try {
        auto gated = CajetaJit::compile(program(""), "test.M", o);
        (void) gated;
    } catch (cajeta::Exception& e) {
        threw = true;
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_XPU_KERNEL_GATE");
    }
    err = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(threw) << "the gated compile must throw:\n" << err;
    EXPECT_NE(err.find("cajeta: error: [xpu-kernel-skipped] bad"), std::string::npos) << err;

    // And the tracked form passes the gated compile as a note.
    testing::internal::CaptureStderr();
    auto held = CajetaJit::compile(program(TRACKED), "test.M", o);
    err = testing::internal::GetCapturedStderr();
    ASSERT_NE(held, nullptr) << err;
    EXPECT_NE(err.find("[tracked: xpu-kernel-adaptor 4.2.1]"), std::string::npos) << err;
}
