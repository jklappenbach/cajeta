// int64-scan-verbs Units 1 and 2: the int64 wave verbs (spec 2). One kernel body computes
// every verb over values whose low halves carry, negative values, values above 2^32 and values
// near the int64 minimum. On cpu it runs at declared waves 8, 16 and 32, on each device at 32.
// Each lane's results equal a host model of the verb at that width (use cases 2.2.1 to 2.2.3),
// the cpu widths compose and every backend equals the same model (2.2.4), and every recorded
// launch replays through the reference interpreter bit for bit.
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/reference/Conformance.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

const uint32_t kN = 128;     // two workgroups of 64
const uint32_t kVerbs = 8;
const std::vector<uint32_t> kCpuWidths = {8, 16, 32};
// A device builds the wave its hardware has, and a kernel declaring another width is refused
// at build (checkWaveWidthBuilt), so the devices run the 32-lane kernel.
const std::vector<uint32_t> kDeviceWidths = {32};

std::string kernelName(uint32_t w) { return "i64w" + std::to_string(w); }

// One kernel per width over the same body, a host `run` that launches each, and an accessor
// per width for its outputs.
std::string source(const std::vector<uint32_t>& widths) {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Wave;\n"
        "public class V {\n";
    for (uint32_t w : widths) s += "    static int64[] got" + std::to_string(w) + ";\n";
    for (uint32_t w : widths)
        s += "    @Kernel\n"
             "    @Wave(width = " + std::to_string(w) + ")\n"
             "    public static void " + kernelName(w) +
             "(KernelBuffer<int64> out, KernelBuffer<int64> in) {\n"
             "        uint32 g = KernelThread.globalIdX();\n"
             "        int64 v = in[g];\n"
             "        uint32 l = Wave.laneId();\n"
             "        uint32 w = Wave.width();\n"
             "        out[g * 8] = Wave.reduceSumI64(v);\n"
             "        out[g * 8 + 1] = Wave.reduceMaxI64(v);\n"
             "        out[g * 8 + 2] = Wave.reduceMinI64(v);\n"
             "        out[g * 8 + 3] = Wave.prefixSumI64(v);\n"
             "        out[g * 8 + 4] = Wave.shuffleSyncI64(v, (l + 3) % w);\n"
             "        out[g * 8 + 5] = Wave.shuffleXorSyncI64(v, 1);\n"
             "        out[g * 8 + 6] = Wave.shuffleUpSyncI64(v, 2);\n"
             "        out[g * 8 + 7] = Wave.shuffleDownSyncI64(v, 2);\n"
             "    }\n";
    s += "    public static int64 input(uint32 i) {\n"
         "        int64 k = (int64) i;\n"
         "        uint32 m = i % 4;\n"
         "        if (m == 0) { return (int64) 4294967295 - k; }\n"            // low half near 2^32: carries
         "        if (m == 1) { return (int64) 0 - k * (int64) 4294967297; }\n" // negative, both halves set
         "        if (m == 2) { return k * (int64) 8589934592 + (int64) 4294967290; }\n" // above 2^32
         "        return (int64) 0 - (int64) 9223372036854775807 + k;\n"        // near the int64 minimum
         "    }\n"
         "    public static int32 run() {\n"
         "        uint32 n = 128;\n"
         "        int64[] h = heap int64[n];\n"
         "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = V.input(i); }\n"
         "        KernelBuffer<int64> in = heap KernelBuffer<int64>(n);\n"
         "        in.upload(h);\n"
         "        KernelStream s #= KernelStream.current();\n"
         "        try {\n";
    for (uint32_t w : widths) {
        std::string W = std::to_string(w);
        s += "        KernelBuffer<int64> o" + W + " = heap KernelBuffer<int64>(n * 8);\n"
             "        " + kernelName(w) + ".launch(s, grid: [2], block: [64])(o" + W + ", in);\n"
             "        s.sync();\n"
             "        got" + W + " = heap int64[n * 8];\n"
             "        o" + W + ".download(got" + W + ");\n";
    }
    // A launch that throws reports its message, so the test fails with the reason instead
    // of the process exiting on an uncaught exception.
    s += "        } catch (Exception e) {\n"
         "            System.stderr.println(\"launch threw: \" + e.getMessage());\n"
         "            return 2;\n"
         "        }\n"
         "        return 1;\n"
         "    }\n";
    for (uint32_t w : widths)
        s += "    public static int64 at" + std::to_string(w) + "(uint32 i) { return got" +
             std::to_string(w) + "[i]; }\n";
    s += "}\n";
    return s;
}

int64_t input(uint32_t i) {
    int64_t k = i;
    switch (i % 4) {
        case 0: return INT64_C(4294967295) - k;
        case 1: return (int64_t) (0 - (uint64_t) k * UINT64_C(4294967297));
        case 2: return k * INT64_C(8589934592) + INT64_C(4294967290);
        default: return INT64_MIN + 1 + k;
    }
}

// The host model of each verb at wave width W over the lanes of g's wave.
std::vector<int64_t> model(uint32_t W) {
    std::vector<int64_t> v(kN), out(kN * kVerbs);
    for (uint32_t i = 0; i < kN; ++i) v[i] = input(i);
    for (uint32_t g = 0; g < kN; ++g) {
        uint32_t l = g % W, base = g - l;
        uint64_t sum = 0, pre = 0;
        int64_t mx = INT64_MIN, mn = INT64_MAX;
        for (uint32_t j = 0; j < W; ++j) {
            int64_t x = v[base + j];
            sum += (uint64_t) x;
            if (j < l) pre += (uint64_t) x;
            mx = x > mx ? x : mx;
            mn = x < mn ? x : mn;
        }
        int64_t* o = &out[g * kVerbs];
        o[0] = (int64_t) sum;
        o[1] = mx;
        o[2] = mn;
        o[3] = (int64_t) pre;
        o[4] = v[base + (l + 3) % W];
        o[5] = v[base + (l ^ 1)];
        o[6] = l >= 2 ? v[base + l - 2] : v[g];
        o[7] = l + 2 < W ? v[base + l + 2] : v[g];
    }
    return out;
}

const char* kVerbName[] = {"reduceSumI64", "reduceMaxI64", "reduceMinI64", "prefixSumI64",
                           "shuffleSyncI64", "shuffleXorSyncI64", "shuffleUpSyncI64",
                           "shuffleDownSyncI64"};

struct Outputs {
    std::map<uint32_t, std::vector<int64_t>> byWidth;
    std::string stderrText;
    bool ran = false;
};

Outputs runOn(cajeta::xpu::Backend be, const fs::path& dir,
              const std::vector<uint32_t>& widths) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(source(widths), "test.V", opts);
    EXPECT_NE(jit, nullptr) << "the int64 wave verbs do not compile";
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::map<uint32_t, int64_t (*)(uint32_t)> at;
    for (uint32_t w : widths) at[w] = jit->lookup<int64_t (*)(uint32_t)>("at" + std::to_string(w));
    bool all = fn != nullptr;
    for (auto& [w, f] : at) all = all && f != nullptr;
    EXPECT_TRUE(all);
    if (!all) return o;
    if (!dir.empty()) setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    testing::internal::CaptureStderr();
    int32_t r = fn();
    o.stderrText = testing::internal::GetCapturedStderr();
    if (!dir.empty()) unsetenv("CAJETA_XPU_RECORD");
    EXPECT_EQ(r, 1) << o.stderrText;
    if (r != 1) return o;
    for (auto& [w, f] : at)
        for (uint32_t k = 0; k < kN * kVerbs; ++k) o.byWidth[w].push_back(f(k));
    o.ran = r == 1;
    return o;
}

// 2.2.1 to 2.2.3: each lane's result at each width is the host model's.
void matchesTheModel(const Outputs& o, const char* backend, const std::vector<uint32_t>& widths) {
    ASSERT_TRUE(o.ran);
    EXPECT_EQ(o.stderrText.find("[xpu-kernel-skipped]"), std::string::npos)
        << backend << " did not run every kernel:\n" << o.stderrText;
    for (uint32_t W : widths) {
        std::vector<int64_t> want = model(W);
        const std::vector<int64_t>& got = o.byWidth.at(W);
        ASSERT_EQ(got.size(), want.size());
        for (uint32_t k = 0; k < want.size(); ++k)
            ASSERT_EQ(got[k], want[k]) << kVerbName[k % kVerbs] << " at wave " << W
                << ", lane " << (k / kVerbs) % W << " (g " << k / kVerbs << ") on " << backend;
    }
}

// Every launch replays through the reference bit for bit, and every kernel was recorded.
void everyKernelReplays(const fs::path& dir, const char* backend,
                        const std::vector<uint32_t>& widths) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, source(widths));
    std::vector<cajeta::MethodPtr> kernels;
    for (uint32_t w : widths)
        if (auto k = findKernel(module, "test.V", kernelName(w))) kernels.push_back(k);
    EXPECT_EQ(kernels.size(), widths.size());
    ref::CorpusRun run = ref::runCorpus(kernels, dir.string(), "");
    std::map<std::string, std::string> outcome;
    for (auto& r : run.results) {
        for (uint32_t w : widths)
            if (r.kernel.find(kernelName(w)) != std::string::npos) outcome[kernelName(w)] = r.outcome;
        EXPECT_EQ(r.outcome, "pass") << r.kernel << " on " << r.backend << ": " << r.detail;
    }
    for (uint32_t w : widths)
        EXPECT_TRUE(outcome.count(kernelName(w))) << kernelName(w) << " has no recording on " << backend;
}

// Unit 2: a device's outputs are the host model's, and every launch replays through the
// reference. Vulkan is not recorded (its handles are not addresses), so it is held to the
// model only.
void deviceMatches(cajeta::xpu::Backend be, const char* backend, bool record) {
    fs::path dir;
    if (record) {
        dir = fs::temp_directory_path() / ("cajeta-wave-i64-" + std::string(backend));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    Outputs o = runOn(be, dir, kDeviceWidths);
    matchesTheModel(o, backend, kDeviceWidths);
    if (record) {
        everyKernelReplays(dir, backend, kDeviceWidths);
        fs::remove_all(dir);
    }
}

}  // namespace

TEST(XpuWaveI64, everyVerbMatchesTheHostModelAtEachWidthOnCpu) {
    matchesTheModel(runOn(cajeta::xpu::Backend::Cpu, fs::path(), kCpuWidths), "cpu", kCpuWidths);
}

// 2.2.4: the reduce answers do not depend on the width once the wave covers the same
// lanes, so wave 32's sum over lanes 0..31 is wave 16's two halves added, and wave 8's four
// quarters. The model already pins each width; this pins that the widths compose.
TEST(XpuWaveI64, theWidthsComposeOnCpu) {
    Outputs o = runOn(cajeta::xpu::Backend::Cpu, fs::path(), kCpuWidths);
    ASSERT_TRUE(o.ran);
    for (uint32_t g = 0; g < kN; g += 32) {
        uint64_t s16 = (uint64_t) o.byWidth[16][g * kVerbs] + (uint64_t) o.byWidth[16][(g + 16) * kVerbs];
        uint64_t s8 = 0;
        for (uint32_t q = 0; q < 4; ++q) s8 += (uint64_t) o.byWidth[8][(g + 8 * q) * kVerbs];
        EXPECT_EQ((uint64_t) o.byWidth[32][g * kVerbs], s16) << "g " << g;
        EXPECT_EQ((uint64_t) o.byWidth[32][g * kVerbs], s8) << "g " << g;
    }
}

TEST(XpuWaveI64, everyLaunchReplaysThroughTheReferenceOnCpu) {
    fs::path dir = fs::temp_directory_path() / "cajeta-wave-i64-cpu";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Outputs o = runOn(cajeta::xpu::Backend::Cpu, dir, kCpuWidths);
    ASSERT_TRUE(o.ran);
    everyKernelReplays(dir, "cpu", kCpuWidths);
    fs::remove_all(dir);
}

TEST(XpuWaveI64, everyVerbMatchesTheModelAndReplaysOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    deviceMatches(cajeta::xpu::Backend::Nvptx, "nvptx", true);
}

TEST(XpuWaveI64, everyVerbMatchesTheModelAndReplaysOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    deviceMatches(cajeta::xpu::Backend::Amdgpu, "amdgpu", true);
}

TEST(XpuWaveI64, everyVerbMatchesTheModelOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    deviceMatches(cajeta::xpu::Backend::Spirv, "vulkan", false);
}
