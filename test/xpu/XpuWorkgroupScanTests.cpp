// int64-scan-verbs Unit 4: Workgroup.scan (spec 4). An exclusive scan over the workgroup's
// linear order for int32 and int64, GroupOp Add, Max and Min, at a block of three waves, a block
// of one wave and a block that is not a compile-time constant; inside a loop; and as the slot of
// a stream compaction. Each lane's result equals a host model on cpu, every launch replays
// through the reference, and each device equals the reference (nvptx, amdgpu) or the cpu
// (Vulkan, which is not recorded).
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/reference/Conformance.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

const char* kKernels[] = {"scI", "scL", "scLoop", "scC"};

// The three launches of scI and scL: (block, grid, offset into the output, in elements).
struct Launch { uint32_t block, grid, off; };
const Launch kLaunches[] = {{96, 2, 0}, {32, 2, 576}, {64, 2, 768}};
const uint32_t kOutN = 1152;      // 192*3 + 64*3 + 128*3
const uint32_t kLoopN = 256;      // scLoop: grid 2, block 128
const uint32_t kCompactN = 192;   // scC: grid 2, block 96

const char* kSource = R"CJ(
package test;
import cajeta.xpu.GroupOp;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Workgroup;
public class S {
    static int32[] gotI;
    static int64[] gotL;
    static int32[] gotLoop;
    static int32[] gotC;
    @Kernel
    @Wave(width = 32)
    public static void scI(KernelBuffer<int32> out, KernelBuffer<int32> in, uint32 off) {
        uint32 g = KernelThread.globalIdX();
        int32 v = in[g];
        out[off + g * 3] = Workgroup.scan(GroupOp.Add, v);
        out[off + g * 3 + 1] = Workgroup.scan(GroupOp.Max, v);
        out[off + g * 3 + 2] = Workgroup.scan(GroupOp.Min, v);
    }
    @Kernel
    @Wave(width = 32)
    public static void scL(KernelBuffer<int64> out, KernelBuffer<int64> in, uint32 off) {
        uint32 g = KernelThread.globalIdX();
        int64 v = in[g];
        out[off + g * 3] = Workgroup.scan(GroupOp.Add, v);
        out[off + g * 3 + 1] = Workgroup.scan(GroupOp.Max, v);
        out[off + g * 3 + 2] = Workgroup.scan(GroupOp.Min, v);
    }
    // 4.2.4: a scan in a loop reads nothing left by the pass before.
    @Kernel
    @Wave(width = 32)
    public static void scLoop(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        uint32 g = KernelThread.globalIdX();
        int32 acc = 0;
        int32 k = 0;
        while (k < 3) {
            acc = acc + Workgroup.scan(GroupOp.Add, in[g] * (k + 1));
            k = k + 1;
        }
        out[g] = acc;
    }
    // 4.2.2: every third work-item keeps its index; the kept ones land densely, in order.
    @Kernel
    @Wave(width = 32)
    public static void scC(KernelBuffer<int32> out) {
        uint32 g = KernelThread.globalIdX();
        int32 keep = 0;
        if (g % 3 == 0) { keep = 1; }
        int32 slot = Workgroup.scan(GroupOp.Add, keep);
        if (keep == 1) { out[Workgroup.x() * 96 + (uint32) slot] = (int32) g; }
    }
    public static int64 input64(uint32 i) {
        int64 k = (int64) i;
        uint32 m = i % 4;
        if (m == 0) { return (int64) 4294967295 - k; }
        if (m == 1) { return (int64) 0 - k * (int64) 4294967297; }
        if (m == 2) { return k * (int64) 8589934592 + (int64) 4294967290; }
        return (int64) 0 - (int64) 9223372036854775807 + k;
    }
    public static int32 run(uint32 nb) {
        uint32 n = 256;
        int32[] hi = heap int32[n];
        int64[] hl = heap int64[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            hi[i] = (int32) ((i * 7919) % 2001) - 1000;
            hl[i] = S.input64(i);
        }
        KernelBuffer<int32> inI = heap KernelBuffer<int32>(n);
        KernelBuffer<int64> inL = heap KernelBuffer<int64>(n);
        KernelBuffer<int32> outI = heap KernelBuffer<int32>(1152);
        KernelBuffer<int64> outL = heap KernelBuffer<int64>(1152);
        KernelBuffer<int32> outLoop = heap KernelBuffer<int32>(256);
        KernelBuffer<int32> outC = heap KernelBuffer<int32>(192);
        int32[] none = heap int32[192];
        for (uint32 i = 0; i < 192; i = i + 1) { none[i] = -1; }
        inI.upload(hi);
        inL.upload(hl);
        outC.upload(none);
        KernelStream s #= KernelStream.current();
        try {
            scI.launch(s, grid: [2], block: [96])(outI, inI, (uint32) 0);
            s.sync();
            scI.launch(s, grid: [2], block: [32])(outI, inI, (uint32) 576);
            s.sync();
            scI.launch(s, grid: [2], block: [nb])(outI, inI, (uint32) 768);
            s.sync();
            scL.launch(s, grid: [2], block: [96])(outL, inL, (uint32) 0);
            s.sync();
            scL.launch(s, grid: [2], block: [32])(outL, inL, (uint32) 576);
            s.sync();
            scL.launch(s, grid: [2], block: [nb])(outL, inL, (uint32) 768);
            s.sync();
            scLoop.launch(s, grid: [2], block: [128])(outLoop, inI);
            s.sync();
            scC.launch(s, grid: [2], block: [96])(outC);
            s.sync();
        } catch (Exception e) {
            System.stderr.println("launch threw: " + e.getMessage());
            return 2;
        }
        gotI = heap int32[1152];
        gotL = heap int64[1152];
        gotLoop = heap int32[256];
        gotC = heap int32[192];
        outI.download(gotI);
        outL.download(gotL);
        outLoop.download(gotLoop);
        outC.download(gotC);
        return 1;
    }
    public static int32 i(uint32 k) { return gotI[k]; }
    public static int64 l(uint32 k) { return gotL[k]; }
    public static int32 lp(uint32 k) { return gotLoop[k]; }
    public static int32 c(uint32 k) { return gotC[k]; }
}
)CJ";

int32_t inputI(uint32_t i) { return (int32_t) ((i * 7919u) % 2001u) - 1000; }

int64_t inputL(uint32_t i) {
    int64_t k = i;
    switch (i % 4) {
        case 0: return INT64_C(4294967295) - k;
        case 1: return (int64_t) (0 - (uint64_t) k * UINT64_C(4294967297));
        case 2: return k * INT64_C(8589934592) + INT64_C(4294967290);
        default: return INT64_MIN + 1 + k;
    }
}

// The exclusive scan of `op` (0 Add, 1 Max, 2 Min) over the lanes of g's workgroup before g.
template <typename T>
T exclusive(T (*in)(uint32_t), uint32_t g, uint32_t block, int op) {
    T acc = op == 0 ? T(0) : op == 1 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
    for (uint32_t j = g - g % block; j < g; ++j) {
        T x = in(j);
        if (op == 0) acc = (T) ((std::make_unsigned_t<T>) acc + (std::make_unsigned_t<T>) x);
        else acc = op == 1 ? std::max(acc, x) : std::min(acc, x);
    }
    return acc;
}

struct Outputs {
    std::vector<int32_t> i, loop, c;
    std::vector<int64_t> l;
    std::string stderrText;
    bool ran = false;
};

Outputs runOn(cajeta::xpu::Backend be, const fs::path& dir) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.S", opts);
    EXPECT_NE(jit, nullptr);
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)(uint32_t)>("run");
    auto iAt = jit->lookup<int32_t (*)(uint32_t)>("i");
    auto lAt = jit->lookup<int64_t (*)(uint32_t)>("l");
    auto lpAt = jit->lookup<int32_t (*)(uint32_t)>("lp");
    auto cAt = jit->lookup<int32_t (*)(uint32_t)>("c");
    EXPECT_TRUE(fn && iAt && lAt && lpAt && cAt);
    if (!(fn && iAt && lAt && lpAt && cAt)) return o;
    if (!dir.empty()) setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    testing::internal::CaptureStderr();
    int32_t r = fn(64);
    o.stderrText = testing::internal::GetCapturedStderr();
    if (!dir.empty()) unsetenv("CAJETA_XPU_RECORD");
    EXPECT_EQ(r, 1) << o.stderrText;
    if (r != 1) return o;
    for (uint32_t k = 0; k < kOutN; ++k) {
        o.i.push_back(iAt(k));
        o.l.push_back(lAt(k));
    }
    for (uint32_t k = 0; k < kLoopN; ++k) o.loop.push_back(lpAt(k));
    for (uint32_t k = 0; k < kCompactN; ++k) o.c.push_back(cAt(k));
    o.ran = true;
    return o;
}

const char* kOpName[] = {"Add", "Max", "Min"};

// 4.2.1 to 4.2.4 against the host model.
void matchesTheModel(const Outputs& o, const char* backend) {
    ASSERT_TRUE(o.ran);
    EXPECT_EQ(o.stderrText.find("[xpu-kernel-skipped]"), std::string::npos)
        << backend << " did not run every kernel:\n" << o.stderrText;
    for (const Launch& L : kLaunches)
        for (uint32_t g = 0; g < L.block * L.grid; ++g)
            for (int op = 0; op < 3; ++op) {
                uint32_t k = L.off + g * 3 + op;
                ASSERT_EQ(o.i[k], exclusive<int32_t>(inputI, g, L.block, op))
                    << "int32 " << kOpName[op] << " at g " << g << ", block " << L.block
                    << " on " << backend;
                ASSERT_EQ(o.l[k], exclusive<int64_t>(inputL, g, L.block, op))
                    << "int64 " << kOpName[op] << " at g " << g << ", block " << L.block
                    << " on " << backend;
            }
    for (uint32_t g = 0; g < kLoopN; ++g) {
        int32_t e = exclusive<int32_t>(inputI, g, 128, 0);
        ASSERT_EQ(o.loop[g], e + 2 * e + 3 * e) << "scLoop at g " << g << " on " << backend;
    }
    for (uint32_t w = 0; w < 2; ++w) {
        uint32_t kept = 0;
        for (uint32_t g = w * 96; g < w * 96 + 96; ++g)
            if (g % 3 == 0) {
                ASSERT_EQ(o.c[w * 96 + kept], (int32_t) g) << "scC slot " << kept << " of workgroup "
                                                           << w << " on " << backend;
                ++kept;
            }
        for (uint32_t k = kept; k < 96; ++k)
            ASSERT_EQ(o.c[w * 96 + k], -1) << "scC wrote past its kept rows on " << backend;
    }
}

void everyKernelReplays(const fs::path& dir, const char* backend) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    std::vector<cajeta::MethodPtr> kernels;
    for (const char* n : kKernels)
        if (auto k = findKernel(module, "test.S", n)) kernels.push_back(k);
    EXPECT_EQ(kernels.size(), 4u);
    ref::CorpusRun run = ref::runCorpus(kernels, dir.string(), "");
    std::map<std::string, std::string> outcome;
    for (auto& r : run.results) {
        for (const char* n : kKernels)
            if (r.kernel.find(n) != std::string::npos) outcome[n] = r.outcome;
        EXPECT_EQ(r.outcome, "pass") << r.kernel << " on " << r.backend << ": " << r.detail;
    }
    for (const char* n : kKernels)
        EXPECT_TRUE(outcome.count(n)) << n << " has no recording on " << backend;
}

void recordedOn(cajeta::xpu::Backend be, const char* backend) {
    fs::path dir = fs::temp_directory_path() / ("cajeta-wgscan-" + std::string(backend));
    fs::remove_all(dir);
    fs::create_directories(dir);
    Outputs o = runOn(be, dir);
    matchesTheModel(o, backend);
    everyKernelReplays(dir, backend);
    fs::remove_all(dir);
}

}  // namespace

TEST(XpuWorkgroupScan, everyScanMatchesTheModelAndReplaysOnCpu) {
    recordedOn(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuWorkgroupScan, everyScanMatchesTheModelAndReplaysOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    recordedOn(cajeta::xpu::Backend::Nvptx, "nvptx");
}

TEST(XpuWorkgroupScan, everyScanMatchesTheModelAndReplaysOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    recordedOn(cajeta::xpu::Backend::Amdgpu, "amdgpu");
}

TEST(XpuWorkgroupScan, everyScanMatchesTheModelOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    matchesTheModel(runOn(cajeta::xpu::Backend::Spirv, fs::path()), "vulkan");
}
