// vulkan-batch-pipelining Unit 2 (spec 3): under the profiler a batched Vulkan launch is
// bracketed by timestamps inside the batch's own command buffer and resolved at EVENT
// tier after the fence, so a profiled step keeps the unprofiled step's cadence.

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include "../xpu/XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kChildMarker = "CAJETA_VK_BATCH_PROF_CHILD";

// `run(n)`: n dependent launches on the current stream, one sync; returns the sum.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class P {\n"
    "    @Kernel\n"
    "    public static void bump(KernelBuffer<int32> out, int32 a, int32 b, int32 c, int32 d) {\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + a + b + c + d; }\n"
    "    }\n"
    "    public static int64 run(int32 n) {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        out.allocate();\n"
    "        int32[] zero #= heap int32[1];\n"
    "        out.upload(zero);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            bump.launch(s, grid: [1], block: [1])(out, 1, 1, 1, 1);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        s.sync();\n"
    "        int32[] got #= heap int32[1];\n"
    "        out.download(got);\n"
    "        out.free();\n"
    "        return (int64) got[0];\n"
    "    }\n"
    "}\n";

std::vector<CajetaGpuEvent> g_caught;
int32_t catchSink(const CajetaGpuEvent* recs, int32_t n, void*) {
    for (int32_t i = 0; i < n; ++i) g_caught.push_back(recs[i]);
    return 0;
}

struct Harness {
    std::unique_ptr<CajetaJit> jit;
    int64_t (*run)(int32_t) = nullptr;
    int32_t (*sinkReg)(CajetaGpuSinkFn, void*, int32_t) = nullptr;
    int32_t (*sinkUnreg)(int32_t) = nullptr;
    int32_t (*gpuFlush)(void) = nullptr;
    int32_t (*gpuCollect)(int32_t) = nullptr;
    int32_t (*timingOk)(void) = nullptr;
    int64_t (*vkUnavailable)(void) = nullptr;
    int32_t (*checkDispatch)(const CajetaGpuEvent*) = nullptr;
    bool ok() const { return jit && run && sinkReg && sinkUnreg && gpuFlush && gpuCollect && timingOk && checkDispatch; }
};

// One JIT per process: the Vulkan instance and the profiler state are per runtime.
Harness& harness() {
    static Harness h;
    if (h.jit) return h;
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    h.jit = CajetaJit::compile(kSource, "test.P", o);
    if (!h.jit) return h;
    auto sym = [&](const char* n) { return h.jit->lookupRawSymbol(n); };
    h.run = h.jit->lookup<int64_t (*)(int32_t)>("run");
    h.sinkReg = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(sym("__cajeta_prof_gpu_sink_register"));
    h.sinkUnreg = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
    h.gpuFlush = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    h.gpuCollect = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    h.timingOk = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_vk_timing_ok"));
    h.vkUnavailable = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_vk_unavailable"));
    h.checkDispatch = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(sym("__cajeta_prof_check_dispatch"));
    return h;
}

// Run `n` launches with a sink attached; the records land in g_caught; stderr in *err.
int64_t profiledRun(Harness& h, int n, std::string* err) {
    g_caught.clear();
    const int32_t sink = h.sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    testing::internal::CaptureStderr();
    const int64_t sum = h.run(n);
    h.gpuCollect(CAJ_GPU_BACKEND_VULKAN);
    h.gpuFlush();
    *err = testing::internal::GetCapturedStderr();
    h.sinkUnreg(sink);
    return sum;
}

std::vector<const CajetaGpuEvent*> spansOf(int32_t tier) {
    std::vector<const CajetaGpuEvent*> v;
    for (const auto& e : g_caught) {
        if (e.kernel_name && std::string(e.kernel_name) == "host blocked on GPU") continue;
        if (e.tier == tier) v.push_back(&e);
    }
    return v;
}
size_t waits() {
    size_t n = 0;
    for (const auto& e : g_caught)
        if (e.kernel_name && std::string(e.kernel_name) == "host blocked on GPU") n++;
    return n;
}
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// One unprofiled launch first: the device, and with it the timing facilities, come up
// on the first launch, so timingOk() answers only after it.
#define VK_PROF_GATE(h)                                                        \
    CAJETA_SKIP_IF_NO_VULKAN();                                                \
    Harness& h = harness();                                                    \
    ASSERT_TRUE(h.ok());                                                       \
    ASSERT_EQ(h.run(1), 4);                                                    \
    if (!h.timingOk()) GTEST_SKIP() << "Vulkan timestamp timing unavailable here";

int runInFreshProcess(const std::string& filter, const char* envName, const char* envValue,
                      std::string& out) {
    const std::string exe = cajeta_self_exe().string();
    if (exe.empty()) return -1;
    ::setenv(kChildMarker, "1", 1);
    if (envName) ::setenv(envName, envValue, 1);
    const std::string cmd = "\"" + exe + "\" --gtest_filter=" + filter + " --gtest_color=no 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) { ::unsetenv(kChildMarker); if (envName) ::unsetenv(envName); return -1; }
    char buf[4096];
    while (::fgets(buf, sizeof(buf), p)) out += buf;
    const int rc = ::pclose(p);
    ::unsetenv(kChildMarker);
    if (envName) ::unsetenv(envName);
    return rc;
}

} // namespace

// 2.1.1 — every batched launch resolves at EVENT tier with a device span, from inside
// the stream (the in-stream flag), and the batch flushed once.
TEST(ProfilerVulkanBatch, batchedLaunchesResolveAtEventTier) {
    VK_PROF_GATE(h);
    std::string err;
    ASSERT_EQ(profiledRun(h, 50, &err), 200) << err.substr(0, 300);
    auto spans = spansOf(CAJETA_PROF_TIER_EVENT);
    ASSERT_EQ(spans.size(), 50u) << "host-tier left: " << spansOf(CAJETA_PROF_TIER_HOST).size();
    for (const CajetaGpuEvent* e : spans) {
        EXPECT_GT(e->dev_end_ns, e->dev_start_ns);
        EXPECT_TRUE(e->integrity_flags & CAJETA_SPAN_INSTREAM) << "launch " << e->launch_id;
        EXPECT_EQ(h.checkDispatch(e), CAJETA_SPAN_OK) << "launch " << e->launch_id
            << " flags " << h.checkDispatch(e);
    }
    // 50 launches: chunks of 8, 16 and 26 at the flush; the fence is waited once.
    EXPECT_EQ(waits(), 1u);
}

// 2.1.2 — the spans are ordered and sit inside the batch's wall.
TEST(ProfilerVulkanBatch, spansAreMonotonicAndWithinTheBatchWall) {
    VK_PROF_GATE(h);
    ::setenv("CAJETA_XPU_VK_SYNC_LOG", "1", 1);
    std::string err;
    const int64_t sum = profiledRun(h, 50, &err);
    ::unsetenv("CAJETA_XPU_VK_SYNC_LOG");
    ASSERT_EQ(sum, 200);
    auto spans = spansOf(CAJETA_PROF_TIER_EVENT);
    ASSERT_EQ(spans.size(), 50u);
    int64_t firstLaunch = spans[0]->host_launch_ns, lastResolved = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i]->host_launch_ns < firstLaunch) firstLaunch = spans[i]->host_launch_ns;
        if (spans[i]->resolved_ns > lastResolved) lastResolved = spans[i]->resolved_ns;
        if (i > 0) EXPECT_GE(spans[i]->dev_start_ns, spans[i - 1]->dev_start_ns) << "launch " << spans[i]->launch_id;
        if (spans[i]->dev_start_ns < firstLaunch) {
            ADD_FAILURE() << "launch " << spans[i]->launch_id << " starts "
                          << (firstLaunch - spans[i]->dev_start_ns) << " ns before the first launch; "
                          << err.substr(0, 600);
            break;
        }
    }
    EXPECT_LE(spans.back()->dev_end_ns, lastResolved) << "a span after its resolution";
}

// 2.1.3 — the profiled batch submits the same chunks as the unprofiled one.
TEST(ProfilerVulkanBatch, aProfiledStepKeepsTheCadence) {
    VK_PROF_GATE(h);
    ::setenv("CAJETA_XPU_VK_SYNC_LOG", "1", 1);
    std::string err;
    const int64_t sum = profiledRun(h, 44, &err);
    ::unsetenv("CAJETA_XPU_VK_SYNC_LOG");
    ASSERT_EQ(sum, 176);
    EXPECT_TRUE(has(err, "sync-log 44 dispatches")) << err.substr(0, 400);
    EXPECT_TRUE(has(err, "2 chunks, first 8, ramp 2")) << err.substr(0, 400);
    EXPECT_EQ(spansOf(CAJETA_PROF_TIER_EVENT).size(), 44u);
}

// 2.1.4 — more launches than one query pool holds: the chunk submits early, nothing drops.
TEST(ProfilerVulkanBatch, queryPoolExhaustionSubmitsNotDrops) {
    VK_PROF_GATE(h);
    ::setenv("CAJETA_XPU_VK_CHUNK", "1000", 1);
    ::setenv("CAJETA_XPU_VK_SYNC_LOG", "1", 1);
    std::string err;
    const int64_t sum = profiledRun(h, 600, &err);
    ::unsetenv("CAJETA_XPU_VK_CHUNK");
    ::unsetenv("CAJETA_XPU_VK_SYNC_LOG");
    ASSERT_EQ(sum, 2400);
    EXPECT_EQ(spansOf(CAJETA_PROF_TIER_EVENT).size(), 600u)
        << "host-tier left: " << spansOf(CAJETA_PROF_TIER_HOST).size();
    EXPECT_TRUE(has(err, "2 chunks")) << err.substr(0, 400);   // 256, 256, 88 at the flush
}

// 2.1.5 — CAJETA_XPU_VK_SUBMIT=eager keeps the per-launch bracket (the control). A fresh
// process: the submit mode is read once per runtime.
TEST(ProfilerVulkanBatch, eagerModeKeepsTheOldBracket) {
    if (::getenv(kChildMarker) == nullptr) {
        CAJETA_SKIP_IF_NO_VULKAN();
        std::string out;
        const int rc = runInFreshProcess("ProfilerVulkanBatch.eagerModeKeepsTheOldBracket",
                                         "CAJETA_XPU_VK_SUBMIT", "eager", out);
        if (rc != 0) FAIL() << "the eager child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos) GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }
    VK_PROF_GATE(h);
    std::string err;
    ASSERT_EQ(profiledRun(h, 4, &err), 16);
    auto spans = spansOf(CAJETA_PROF_TIER_EVENT);
    ASSERT_EQ(spans.size(), 4u);
    for (const CajetaGpuEvent* e : spans)
        EXPECT_FALSE(e->integrity_flags & CAJETA_SPAN_INSTREAM) << "launch " << e->launch_id;
    EXPECT_GE(waits(), 4u);
}

// 2.1.6 — an unavailable query result publishes the launch at host tier, without a wait.
TEST(ProfilerVulkanBatch, unavailableResultsFallToHostTier) {
    VK_PROF_GATE(h);
    ::setenv("CAJETA_XPU_FAULT", "vk-query-unavailable", 1);
    std::string err;
    const int64_t before = h.vkUnavailable ? h.vkUnavailable() : 0;
    const int64_t sum = profiledRun(h, 10, &err);
    ::unsetenv("CAJETA_XPU_FAULT");
    ASSERT_EQ(sum, 40);
    EXPECT_EQ(spansOf(CAJETA_PROF_TIER_EVENT).size(), 0u);
    EXPECT_EQ(spansOf(CAJETA_PROF_TIER_HOST).size(), 10u);
    if (h.vkUnavailable) EXPECT_GE(h.vkUnavailable() - before, 10);
}
