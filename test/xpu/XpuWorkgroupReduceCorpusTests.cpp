// workgroup-reduce Unit 3: Workgroup.reduce on every backend replays through the reference at
// 0 ulp from a recording, and every device matches the cpu bit for bit at wave width 32.
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include "cajeta/xpu/reference/Conformance.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

const char* kKernels[] = {"wgF", "wgI", "wgLoop", "wgL", "wgL1"};
const uint32_t kN = 768;
const uint32_t kWrittenI = 2 * 96 * 3, kWrittenL = 2 * 128;
// int64-scan-verbs Unit 3: wgL reduces int64 over three waves per workgroup, wgL1 over one.
const uint32_t kWrittenL64 = 2 * 96 * 3, kWrittenL1 = 2 * 32 * 2;

const char* kSource = R"CJ(
package test;
import cajeta.xpu.GroupOp;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
import cajeta.xpu.Workgroup;
import cajeta.xpu.XpuLaunchException;
public class W {
    static float32[] gotF;
    static int32[] gotI;
    static float32[] gotL;
    static int64[] gotL64;
    static int64[] gotL1;
    @Kernel
    @Wave(width = 32)
    public static void wgF(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 f = in[g];
        out[g * 3] = Workgroup.reduce(GroupOp.Add, f);
        out[g * 3 + 1] = Workgroup.reduce(GroupOp.Max, f);
        out[g * 3 + 2] = Workgroup.reduce(GroupOp.Min, f);
    }
    @Kernel
    @Wave(width = 32)
    public static void wgI(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        uint32 g = KernelThread.globalIdX();
        int32 v = in[g];
        out[g * 3] = Workgroup.reduce(GroupOp.Add, v);
        out[g * 3 + 1] = Workgroup.reduce(GroupOp.Max, v);
        out[g * 3 + 2] = Workgroup.reduce(GroupOp.Min, v);
    }
    @Kernel
    @Wave(width = 32)
    public static void wgLoop(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 acc = 0.0f;
        int32 k = 0;
        while (k < 3) {
            acc = acc + Workgroup.reduce(GroupOp.Add, in[g] * (float32) (k + 1));
            k = k + 1;
        }
        out[g] = acc;
    }
    @Kernel
    @Wave(width = 32)
    public static void wgL(KernelBuffer<int64> out, KernelBuffer<int64> in) {
        uint32 g = KernelThread.globalIdX();
        int64 v = in[g];
        out[g * 3] = Workgroup.reduce(GroupOp.Add, v);
        out[g * 3 + 1] = Workgroup.reduce(GroupOp.Max, v);
        out[g * 3 + 2] = Workgroup.reduce(GroupOp.Min, v);
    }
    // One wave per workgroup: the workgroup reduce is the wave reduce, so both differences are 0.
    @Kernel
    @Wave(width = 32)
    public static void wgL1(KernelBuffer<int64> out, KernelBuffer<int64> in) {
        uint32 g = KernelThread.globalIdX();
        int64 v = in[g];
        out[g * 2] = Workgroup.reduce(GroupOp.Add, v) - Wave.reduceSumI64(v);
        out[g * 2 + 1] = Workgroup.reduce(GroupOp.Max, v) - Wave.reduceMaxI64(v);
    }
    public static int64 input64(uint32 i) {
        int64 k = (int64) i;
        uint32 m = i % 4;
        if (m == 0) { return (int64) 4294967295 - k; }
        if (m == 1) { return (int64) 0 - k * (int64) 4294967297; }
        if (m == 2) { return k * (int64) 8589934592 + (int64) 4294967290; }
        return (int64) 0 - (int64) 9223372036854775807 + k;
    }
    public static int32 run() {
        uint32 n = 768;
        float32[] hf = heap float32[n];
        int32[] hi = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            hf[i] = (float32) i * 0.3701f - 19.3f;
            hi[i] = (int32) ((i * 7919) % 2001) - 1000;
        }
        KernelBuffer<float32> inF = heap KernelBuffer<float32>(n);
        KernelBuffer<int32> inI = heap KernelBuffer<int32>(n);
        KernelBuffer<float32> outF = heap KernelBuffer<float32>(n * 3);
        KernelBuffer<int32> outI = heap KernelBuffer<int32>(n * 3);
        KernelBuffer<float32> outL = heap KernelBuffer<float32>(n);
        inF.upload(hf);
        inI.upload(hi);
        KernelStream s #= KernelStream.current();
        wgF.launch(s, grid: [3], block: [256])(outF, inF);
        s.sync();
        wgI.launch(s, grid: [2], block: [96])(outI, inI);
        s.sync();
        wgLoop.launch(s, grid: [2], block: [128])(outL, inF);
        s.sync();
        int64[] hl = heap int64[n];
        for (uint32 i = 0; i < n; i = i + 1) { hl[i] = W.input64(i); }
        KernelBuffer<int64> inL = heap KernelBuffer<int64>(n);
        KernelBuffer<int64> outL64 = heap KernelBuffer<int64>(n * 3);
        KernelBuffer<int64> outL1 = heap KernelBuffer<int64>(n * 3);
        inL.upload(hl);
        wgL.launch(s, grid: [2], block: [96])(outL64, inL);
        s.sync();
        wgL1.launch(s, grid: [2], block: [32])(outL1, inL);
        s.sync();
        gotL64 = heap int64[n * 3];
        gotL1 = heap int64[n * 3];
        outL64.download(gotL64);
        outL1.download(gotL1);
        gotF = heap float32[n * 3];
        gotI = heap int32[n * 3];
        gotL = heap float32[n];
        outF.download(gotF);
        outI.download(gotI);
        outL.download(gotL);
        return 1;
    }
    public static int32 refusal() {
        KernelBuffer<float32> in = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> out = heap KernelBuffer<float32>(768);
        KernelStream s #= KernelStream.current();
        try {
            wgF.launch(s, grid: [1], block: [256])(out, in);
            s.sync();
        } catch (XpuLaunchException e) {
            if (e.message.contains("@Wave(width)")) { return 3; }
            return 2;
        }
        return 1;
    }
    public static float32 f(uint32 i) { return gotF[i]; }
    public static int32 iv(uint32 i) { return gotI[i]; }
    public static float32 l(uint32 i) { return gotL[i]; }
    public static int64 l64(uint32 i) { return gotL64[i]; }
    public static int64 l1(uint32 i) { return gotL1[i]; }
}
)CJ";

struct Outputs {
    std::vector<float> f, l;
    std::vector<int32_t> i;
    std::vector<int64_t> l64, l1;
    std::string stderrText;
};

// Runs the program on `be`, recording its launches into `dir` when it is not empty.
Outputs runOn(cajeta::xpu::Backend be, const fs::path& dir) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.W", opts);
    EXPECT_NE(jit, nullptr);
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto fAt = jit->lookup<float (*)(uint32_t)>("f");
    auto iAt = jit->lookup<int32_t (*)(uint32_t)>("iv");
    auto lAt = jit->lookup<float (*)(uint32_t)>("l");
    auto l64At = jit->lookup<int64_t (*)(uint32_t)>("l64");
    auto l1At = jit->lookup<int64_t (*)(uint32_t)>("l1");
    EXPECT_TRUE(fn && fAt && iAt && lAt && l64At && l1At);
    if (!(fn && fAt && iAt && lAt && l64At && l1At)) return o;
    if (!dir.empty()) setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    testing::internal::CaptureStderr();
    int32_t r = fn();
    o.stderrText = testing::internal::GetCapturedStderr();
    if (!dir.empty()) unsetenv("CAJETA_XPU_RECORD");
    EXPECT_EQ(r, 1);
    for (uint32_t k = 0; k < kN * 3; ++k) {
        o.f.push_back(fAt(k));
        o.i.push_back(iAt(k));
    }
    for (uint32_t k = 0; k < kN; ++k) o.l.push_back(lAt(k));
    if (r == 1)
        for (uint32_t k = 0; k < kN * 3; ++k) {
            o.l64.push_back(l64At(k));
            o.l1.push_back(l1At(k));
        }
    return o;
}

// 3.2.1: a device's outputs are the cpu outputs bit for bit, at the same wave width.
void sameAsCpu(const Outputs& dev, const char* backend) {
    EXPECT_EQ(dev.stderrText.find("[xpu-kernel-skipped]"), std::string::npos)
        << backend << " did not run every kernel on the device:\n" << dev.stderrText;
    Outputs cpu = runOn(cajeta::xpu::Backend::Cpu, fs::path());
    ASSERT_EQ(dev.f.size(), cpu.f.size());
    ASSERT_EQ(dev.l.size(), cpu.l.size());
    auto bits = [](float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; };
    for (size_t k = 0; k < cpu.f.size(); ++k)
        ASSERT_EQ(bits(dev.f[k]), bits(cpu.f[k])) << "wgF[" << k << "] on " << backend
            << ": " << dev.f[k] << " vs cpu " << cpu.f[k];
    for (size_t k = 0; k < kWrittenI; ++k)
        ASSERT_EQ(dev.i[k], cpu.i[k]) << "wgI[" << k << "] on " << backend;
    for (size_t k = 0; k < kWrittenL; ++k)
        ASSERT_EQ(bits(dev.l[k]), bits(cpu.l[k])) << "wgLoop[" << k << "] on " << backend
            << ": " << dev.l[k] << " vs cpu " << cpu.l[k];
    ASSERT_EQ(dev.l64.size(), cpu.l64.size());
    for (size_t k = 0; k < kWrittenL64; ++k)
        ASSERT_EQ(dev.l64[k], cpu.l64[k]) << "wgL[" << k << "] on " << backend;
    for (size_t k = 0; k < kWrittenL1; ++k)
        ASSERT_EQ(dev.l1[k], cpu.l1[k]) << "wgL1[" << k << "] on " << backend;
}

// 3.1.1, 3.1.2: every launch replays bit for bit, and every kernel was recorded.
Outputs everyKernelPassesOn(cajeta::xpu::Backend be, const char* backend) {
    fs::path dir = fs::temp_directory_path() / ("cajeta-wgreduce-corpus-" + std::string(backend));
    fs::remove_all(dir);
    fs::create_directories(dir);
    Outputs out = runOn(be, dir);
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    std::vector<cajeta::MethodPtr> kernels;
    for (const char* n : kKernels)
        if (auto k = findKernel(module, "test.W", n)) kernels.push_back(k);
    EXPECT_EQ(kernels.size(), 5u);
    ref::CorpusRun run = ref::runCorpus(kernels, dir.string(), "");
    std::map<std::string, std::string> outcome;
    for (auto& r : run.results) {
        for (const char* n : kKernels)
            if (r.kernel.find(n) != std::string::npos) outcome[n] = r.outcome + " " + r.detail;
        EXPECT_EQ(r.outcome, "pass") << r.kernel << " on " << r.backend << ": " << r.detail;
    }
    for (const char* n : kKernels)
        EXPECT_TRUE(outcome.count(n)) << n << " has no recording on " << backend;
    fs::remove_all(dir);
    return out;
}

}  // namespace

TEST(XpuWorkgroupReduceCorpus, everyKernelMatchesTheReferenceOnCpu) {
    everyKernelPassesOn(cajeta::xpu::Backend::Cpu, "cpu");
}

int64_t input64(uint32_t i) {
    int64_t k = i;
    switch (i % 4) {
        case 0: return INT64_C(4294967295) - k;
        case 1: return (int64_t) (0 - (uint64_t) k * UINT64_C(4294967297));
        case 2: return k * INT64_C(8589934592) + INT64_C(4294967290);
        default: return INT64_MIN + 1 + k;
    }
}

// int64-scan-verbs 3.2.1 and 3.2.2 on cpu: the int64 reduce over a workgroup of three waves is
// the exact wrapped sum and the signed max and min over its 96 lanes, and over a one-wave
// workgroup it equals the wave reduce.
TEST(XpuWorkgroupReduceCorpus, anInt64ReduceIsExactAndOneWaveIsTheWaveReduceOnCpu) {
    Outputs o = runOn(cajeta::xpu::Backend::Cpu, fs::path());
    ASSERT_EQ(o.l64.size(), kN * 3);
    for (uint32_t g = 0; g < 2 * 96; ++g) {
        uint32_t base = g - g % 96;
        uint64_t sum = 0;
        int64_t mx = INT64_MIN, mn = INT64_MAX;
        for (uint32_t j = 0; j < 96; ++j) {
            int64_t x = input64(base + j);
            sum += (uint64_t) x;
            mx = std::max(mx, x);
            mn = std::min(mn, x);
        }
        ASSERT_EQ(o.l64[g * 3], (int64_t) sum) << "Add at g " << g;
        ASSERT_EQ(o.l64[g * 3 + 1], mx) << "Max at g " << g;
        ASSERT_EQ(o.l64[g * 3 + 2], mn) << "Min at g " << g;
    }
    for (uint32_t k = 0; k < kWrittenL1; ++k) ASSERT_EQ(o.l1[k], 0) << "wgL1[" << k << "]";
}

TEST(XpuWorkgroupReduceCorpus, everyKernelMatchesTheReferenceOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    sameAsCpu(everyKernelPassesOn(cajeta::xpu::Backend::Amdgpu, "amdgpu"), "amdgpu");
}

TEST(XpuWorkgroupReduceCorpus, everyKernelMatchesTheReferenceOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    sameAsCpu(everyKernelPassesOn(cajeta::xpu::Backend::Nvptx, "nvptx"), "nvptx");
}

// The recorder skips Vulkan (its handles are not addresses), so Vulkan is held to the cpu
// outputs, which the reference replays at 0 ulp.
TEST(XpuWorkgroupReduceCorpus, everyKernelMatchesTheCpuOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    sameAsCpu(runOn(cajeta::xpu::Backend::Spirv, fs::path()), "vulkan");
}

// A kernel whose declared wave width the Vulkan device cannot run, and that has no virtual
// variant, is refused by name, never run at another width.
TEST(XpuWorkgroupReduceCorpus, aWaveTheVulkanDeviceCannotRunIsRefusedByName) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    setenv("CAJETA_XPU_VK_VIRTUAL", "0", 1);
    auto jit = CajetaJit::compile(kSource, "test.W", o);
    unsetenv("CAJETA_XPU_VK_VIRTUAL");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("refusal");
    ASSERT_NE(fn, nullptr);
    testing::internal::CaptureStderr();
    int32_t r = fn();
    std::string err = testing::internal::GetCapturedStderr();
    if (cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(32)) {
        EXPECT_EQ(r, 1) << err;
        EXPECT_EQ(err.find("[xpu-launch-refused]"), std::string::npos) << err;
    } else {
        EXPECT_EQ(r, 3) << err;
        EXPECT_NE(err.find("declares @Wave(width = 32)"), std::string::npos) << err;
    }
}
