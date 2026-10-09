//
// @Wave(width = N) is honoured on the cpu backend (xpu-kernel-adaptor 3.2.4).
//
// A kernel written for a 32-lane wave (`lane = tid % 32`, a whole-wave reduce)
// declares it with @Wave(width = 32). The cpu backend's wave is the host SIMD
// width, 8 under AVX2 and 16 under AVX-512, so without the declaration such a
// kernel either lowered with a 16-lane reduce or did not lower at all, by host.
// With it the work-item loop is pinned to the declared width, the same pin a
// distributed cooperative-matrix kernel takes, and the answer is the host's no
// longer. NVPTX's warp is 32 and nothing else, so it refuses another width by name.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "KernelLoweringProbe.h"

#include "../PortableEnv.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string source(const std::string& waveAnn) {
    return R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.Wave;
public class M {
    @Kernel
)CJ" + waveAnn + R"CJ(
    public static void sumk(KernelBuffer<float32> out) {
        uint32 t = KernelThread.globalIdX();
        uint32 lane = KernelThread.x() % 32;
        float32 s = Wave.reduceSumF32((float32) lane);
        out[t] = s + (float32) Wave.width() * 1000.0f;
    }
    @Kernel
)CJ" + waveAnn + R"CJ(
    public static void fissk(KernelBuffer<float32> out) {
        Shared<float32> part = shared float32[8];
        uint32 tid = KernelThread.x();
        float32 s = Wave.reduceSumF32(1.0f);
        if (tid % 32 == 0) { part[tid / 32] = s; }
        Barrier.workgroup();
        out[KernelThread.globalIdX()] = part[0] + part[1] + part[2] + part[3]
            + part[4] + part[5] + part[6] + part[7];
    }

    @Kernel
    public static void widthk(KernelBuffer<float32> out) {
        out[KernelThread.globalIdX()] = (float32) Wave.width();
    }

    public static float32 run(int32 which) {
        uint32 n = 256;
        float32[] h = heap float32[n];
        KernelBuffer<float32> b = heap KernelBuffer<float32>(n);
        KernelStream s #= KernelStream.current();
        if (which == 0) {
            sumk.launch(s, grid: [1], block: [256])(b);
        } else if (which == 1) {
            fissk.launch(s, grid: [1], block: [256])(b);
        } else {
            widthk.launch(s, grid: [1], block: [256])(b);
        }
        s.sync();
        b.download(h);
        float32 first = h[0];
        for (uint32 i = 1; i < n; i = i + 1) {
            if (h[i] != first) { return -1.0f - (float32) i; }
        }
        return first;
    }
}
)CJ";
}

// The compiler binary this test drives, as DiagFormatJsonTests finds it.
std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) {
        r = envRoot;
    } else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

float runOnCpu(const std::string& src, int which) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1000.0f;
    auto fn = jit->lookup<float (*)(int)>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn(which) : -1000.0f;
}

}  // namespace

// Every lane reads a 32-wide wave and the sum of lanes 0..31, which is 496.
TEST(XpuCpuDeclaredWave, aDeclaredThirtyTwoLaneWaveRunsAtThirtyTwoOnCpu) {
    float r = runOnCpu(source("    @Wave(width = 32)\n"), 0);
    EXPECT_EQ(r, 32.0f * 1000.0f + 496.0f)
        << "width*1000 + sum of lanes; a negative value names the first lane that disagreed";
}

// Eight 32-lane waves in a 256-thread block, across a barrier: 8 x 32.
TEST(XpuCpuDeclaredWave, theDeclaredWidthSurvivesBarrierFission) {
    float r = runOnCpu(source("    @Wave(width = 32)\n"), 1);
    EXPECT_EQ(r, 256.0f);
}

// Undeclared, the wave is the host's width, below 32 on every cpu we build for, so the
// same kernel reads two different sums inside one 32-lane block: the defect the pin removes.
TEST(XpuCpuDeclaredWave, anUndeclaredThirtyTwoLaneKernelIsWrongAtTheHostWidth) {
    float w = runOnCpu(source(""), 2);
    ASSERT_GE(w, 2.0f);
    ASSERT_LT(w, 32.0f);
    float r = runOnCpu(source(""), 0);
    EXPECT_LT(r, 0.0f) << "one 32-lane block agreed at a " << w << "-lane wave: " << r;
}

// NVPTX's warp is 32: it lowers @Wave(width = 32) and refuses 64 by name.
TEST(XpuCpuDeclaredWave, nvptxRefusesAWaveWidthItCannotRun) {
    using namespace cajeta::xpu::probe;
    Lowered ok = lowerForNvptx(source("    @Wave(width = 32)\n"), "sumk");
    EXPECT_TRUE(ok.ok) << ok.why;
    Lowered bad = lowerForNvptx(source("    @Wave(width = 64)\n"), "sumk");
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.why.find("@Wave(width = 64)"), std::string::npos) << bad.why;
}

// A wrapper whose vectorize pipeline does not come back within the deadline
// ENDS THE COMPILE with an error naming the kernel and the lever, rather than
// hanging: iq3xxsF16CoopIdN64Kernel's 32-lane wrapper sat two hours in VPlan
// CSE (xpu-kernel-adaptor 6.4.16, sampled 2026-10-08), and no size the wrapper
// shows beforehand predicts it. The worker owns the module, so the process is
// the unit that stops; the test drives the compiler binary. The stall is
// injected (CAJETA_XPU_FAULT=vectorize-stall) under a 50 ms deadline.
TEST(XpuCpuDeclaredWave, aWrapperPastTheVectorizeDeadlineEndsTheCompileByName) {
    namespace fs = std::filesystem;
    std::string bin = compilerBinary();
    if (!fs::exists(bin)) GTEST_SKIP() << "no compiler binary at " << bin;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_vdeadline_" + std::to_string(rng()));
    fs::create_directories(root / "test");
    fs::create_directories(root / "out");
    { std::ofstream o(root / "test" / "M.cajeta"); o << source("    @Wave(width = 32)\n"); }
    fs::path errFile = root / "stderr.txt";
    std::string cmd = cajeta_env_prefix({{"CAJETA_XPU_CPU_VECTORIZE_DEADLINE_MS", "50"},
                                         {"CAJETA_XPU_FAULT", "vectorize-stall"}})
        + bin + " --emit=ir --xpu-backend=cpu test.M.run " + root.string() + " "
        + (root / "out").string() + " > " CAJETA_PORTABLE_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    std::string err = ss.str();
    EXPECT_NE(rc, 0) << err;
    EXPECT_NE(err.find("[xpu-kernel-hung]"), std::string::npos) << err;
    EXPECT_NE(err.find("CAJETA_XPU_CPU_VECTORIZE_DEADLINE_S"), std::string::npos) << err;
    fs::remove_all(root);
}

// The default deadline admits a small wave kernel: it lowers and answers 32 lanes.
TEST(XpuCpuDeclaredWave, theDefaultVectorizeDeadlineAdmitsASmallWaveKernel) {
    testing::internal::CaptureStderr();
    float r = runOnCpu(source("    @Wave(width = 32)\n"), 0);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("[xpu-kernel-hung]"), std::string::npos) << err;
    EXPECT_EQ(r, 32.0f * 1000.0f + 496.0f);
}

// The deadline's prescription must WORK: a kernel held with
// @Unlowered(backend = "cpu", tracked = ..., hold = true) never reaches the
// vectorizer, so under the injected stall and a 50 ms deadline the compile
// ends normally with the kernel reported as a tracked skip. Found 2026-10-08:
// the first evaluation run held iq3xxsF16CoopIdN64Kernel with a plain
// @Unlowered, which is a gate declaration and not a hold, and the deadline
// fired on the same kernel again.
TEST(XpuCpuDeclaredWave, aHeldKernelNeverReachesTheVectorizer) {
    namespace fs = std::filesystem;
    std::string bin = compilerBinary();
    if (!fs::exists(bin)) GTEST_SKIP() << "no compiler binary at " << bin;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_vhold_" + std::to_string(rng()));
    fs::create_directories(root / "test");
    fs::create_directories(root / "out");
    const std::string hold =
        "    @Unlowered(backend = \"cpu\", tracked = \"xpu-kernel-adaptor 6.4.16\", hold = true)\n";
    // Every kernel in the file is held: the injected stall catches any that is not.
    std::string src = source("    @Wave(width = 32)\n" + hold);
    const std::string widthk = "    public static void widthk(";
    ASSERT_NE(src.find(widthk), std::string::npos);
    src.insert(src.find(widthk), hold);
    { std::ofstream o(root / "test" / "M.cajeta"); o << src; }
    fs::path errFile = root / "stderr.txt";
    std::string cmd = cajeta_env_prefix({{"CAJETA_XPU_CPU_VECTORIZE_DEADLINE_MS", "50"},
                                         {"CAJETA_XPU_FAULT", "vectorize-stall"}})
        + bin + " --emit=ir --xpu-backend=cpu test.M.run " + root.string() + " "
        + (root / "out").string() + " > " CAJETA_PORTABLE_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    std::string err = ss.str();
    EXPECT_EQ(rc, 0) << err;
    EXPECT_EQ(err.find("[xpu-kernel-hung]"), std::string::npos) << err;
    EXPECT_NE(err.find("[xpu-kernel-skipped] sumk: no cpu device code"), std::string::npos) << err;
    EXPECT_NE(err.find("held before lowering"), std::string::npos) << err;
    EXPECT_NE(err.find("[tracked: xpu-kernel-adaptor 6.4.16]"), std::string::npos) << err;
    fs::remove_all(root);
}
