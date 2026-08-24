// cajeta-profiler Unit 13 — Vulkan timestamp timing on a real device
// (plan 13.1.a/c/d/e/g/i, 13.3.a/b; spec §5.5, §6.5).
//
// The pure half (ProfilerVulkanTests) proves the arithmetic and the policy;
// this suite proves the Vulkan API mechanics against a live driver: the
// bracket records, the query pair reads back by availability, the two-slot
// pool survives reuse, the wait shows up as an explicit span, and the whole
// path degrades to the host window rather than failing when any piece is
// missing.
//
// Two lanes share the tests: the default ICD (RADV on the reference gfx1151 —
// 13.3.a's device) and lavapipe (13.1.i's CI lane), selected by re-exec'ing
// this binary with VK_ICD_FILENAMES pointing at the lavapipe ICD. A fresh
// process per lane is not optional: the Vulkan instance is created once per
// runtime instance and the ICD environment is read at that moment.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kChildMarker = "CAJETA_VK_PROF_CHILD";
const char* kLavapipeIcd = "/usr/share/vulkan/icd.d/lvp_icd.json";

const char* kSaxpySource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class VkSaxpy {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 4096;\n"
    "        float32[] hx = heap float32[n];\n"
    "        float32[] hy = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            hx[i] = 1.0f;\n"
    "            hy[i] = 2.0f;\n"
    "        }\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        x.allocate();\n"
    "        y.allocate();\n"
    "        x.upload(hx);\n"
    "        y.upload(hy);\n"
    "        KernelStream s #= KernelStream.current();\n"
    // Four launches through one two-slot query pool: §5.5.5's reset-before-
    // reuse is exercised by construction, and a slot reported available with
    // the PREVIOUS use's value would produce a span the assertions below
    // reject (a duplicate, or one wildly out of order).
    "        for (int32 k = 0; k < 4; k = k + 1) {\n"
    "            saxpy.launch(s, grid: [16], block: [256])(y, x, 2.0f, n);\n"
    "        }\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        x.free();\n"
    "        y.free();\n"
    "        float32 sum = 0.0f;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + hy[i]; }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

std::vector<CajetaGpuEvent> g_caught;

int32_t catchSink(const CajetaGpuEvent* recs, int32_t n, void*) {
    for (int32_t i = 0; i < n; ++i) g_caught.push_back(recs[i]);
    return 0;
}

int runInFreshProcess(const std::string& filter, const char* icd,
                      std::string& out) {
    const std::string exe = cajeta_self_exe().string();
    if (exe.empty()) return -1;
    ::setenv(kChildMarker, "1", 1);
    if (icd) ::setenv("VK_ICD_FILENAMES", icd, 1);
    const std::string cmd = "\"" + exe + "\" --gtest_filter=" + filter +
                            " --gtest_color=no 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) { ::unsetenv(kChildMarker); if (icd) ::unsetenv("VK_ICD_FILENAMES"); return -1; }
    char buf[4096];
    while (::fgets(buf, sizeof(buf), p)) out += buf;
    const int rc = ::pclose(p);
    ::unsetenv(kChildMarker);
    if (icd) ::unsetenv("VK_ICD_FILENAMES");
    return rc;
}

// The body both lanes run. Everything is driven through ONE jit instance —
// the runtime statics (Vulkan instance, profiler state) are per-instance.
void runBracketBody() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    // Static for the same reason the amdgpu fixture generator is: process-
    // level driver state must not outlive the module holding the code it
    // points into.
    static std::unique_ptr<CajetaJit> jit =
        CajetaJit::compile(kSaxpySource, "test.VkSaxpy", o);
    ASSERT_NE(jit, nullptr);

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto sinkReg = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(
        sym("__cajeta_prof_gpu_sink_register"));
    auto sinkUnreg = reinterpret_cast<int32_t (*)(int32_t)>(
        sym("__cajeta_prof_gpu_sink_unregister"));
    auto gpuFlush = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    auto gpuCollect = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    auto timingOk = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_vk_timing_ok"));
    auto vkSpans = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_vk_spans"));
    auto vkUnavailable = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_vk_unavailable"));
    auto validBits = reinterpret_cast<uint32_t (*)(void)>(sym("__cajeta_prof_vk_valid_bits"));
    auto checkDispatch = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        sym("__cajeta_prof_check_dispatch"));
    auto clockConfidence = reinterpret_cast<int32_t (*)(int32_t)>(
        sym("__cajeta_prof_clock_confidence"));
    ASSERT_TRUE(sinkReg && gpuFlush && timingOk && checkDispatch);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);

    g_caught.clear();
    const int32_t sink = sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    const float sum = run();
    gpuCollect(CAJ_GPU_BACKEND_VULKAN);
    gpuFlush();
    sinkUnreg(sink);

    if (sum == 0.0f && g_caught.empty()) {
        GTEST_SKIP() << "no Vulkan device dispatched (run returned 0 with no records)";
    }
    ASSERT_FLOAT_EQ(sum, 40960.0f)   // y=2 +4×(2·1)=10, ×4096
        << "the kernel did not produce the right answer, so whatever was "
           "timed was not this computation";

    // The dispatcher may have fallen back to the CPU rung (no Vulkan device
    // at all) — that is not this suite's subject.
    if (!timingOk()) {
        GTEST_SKIP() << "Vulkan timing unavailable here (no device, or a "
                        "queue family without valid timestamp bits)";
    }

    // 13.1.c/d — the brackets resolved: EVENT-tier spans, one per launch,
    // each with a real duration, its resolution stamp, and a clean pass
    // through the causal-bracket integrity check.
    std::vector<const CajetaGpuEvent*> spans, waits;
    for (const auto& e : g_caught) {
        if (e.kernel_name && std::string(e.kernel_name) == "host blocked on GPU")
            waits.push_back(&e);
        else if (e.tier == CAJETA_PROF_TIER_EVENT)
            spans.push_back(&e);
    }
    std::printf(" RESULT u13_event_spans=%d u13_waits=%d u13_unavailable=%lld\n",
                (int) spans.size(), (int) waits.size(),
                (long long) vkUnavailable());
    ASSERT_EQ(spans.size(), 4u)
        << "four launches through the two-slot pool did not produce four "
           "event-tier spans — a reuse or availability failure (§5.5.4/§5.5.5)";
    EXPECT_EQ(vkSpans(), 4);

    for (const CajetaGpuEvent* e : spans) {
        EXPECT_GT(e->dev_end_ns, e->dev_start_ns)
            << "a device bracket that does not advance is not a measurement";
        EXPECT_GT(e->resolved_ns, 0);
        EXPECT_EQ(checkDispatch(e), CAJETA_SPAN_OK)
            << "a healthy bracket was flagged (flags=" << checkDispatch(e) << ")";
        EXPECT_GT(e->launch_id, 0);
    }
    // Distinct, ordered spans: a stale slot value (§5.5.5) would duplicate or
    // disorder them.
    for (size_t i = 1; i < spans.size(); ++i) {
        EXPECT_GT(spans[i]->dev_start_ns, spans[i - 1]->dev_start_ns)
            << "brackets out of order — a reused slot carried a stale value";
    }

    // §5.5.8 / §14.13 — every submit's blocked interval is an explicit span.
    EXPECT_GE(waits.size(), 4u)
        << "the host blocked on vkQueueWaitIdle without saying so";
    for (const CajetaGpuEvent* w : waits) {
        EXPECT_EQ(w->tier, CAJETA_PROF_TIER_HOST);
        EXPECT_GE(w->dev_end_ns, w->dev_start_ns);
    }

    std::printf(" RESULT u13_valid_bits=%u u13_clock_confidence=%d\n",
                validBits(), clockConfidence(CAJ_GPU_BACKEND_VULKAN));
    EXPECT_GT(validBits(), 0u);

    // 13.1.a — device creation adopted the timing facilities the device
    // offers, and 13.1.g/§6.5 — when calibrated timestamps exist, the
    // explicitly-requested CLOCK_MONOTONIC calibration actually converged
    // (confidence 0 would mean the samples were all rejected, or the domain
    // was never calibrated at all).
    auto hasHqr = reinterpret_cast<int32_t (*)(void)>(
        sym("__cajeta_xpu_vk_has_host_query_reset"));
    auto hasS2 = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_xpu_vk_has_sync2"));
    auto hasCal = reinterpret_cast<int32_t (*)(void)>(
        sym("__cajeta_xpu_vk_has_calibrated_ts"));
    ASSERT_TRUE(hasHqr && hasS2 && hasCal);
    std::printf(" RESULT u13_host_query_reset=%d u13_sync2=%d u13_calibrated_ts=%d\n",
                hasHqr(), hasS2(), hasCal());
    if (hasCal()) {
        EXPECT_GT(clockConfidence(CAJ_GPU_BACKEND_VULKAN), 0)
            << "calibrated timestamps are enabled but the CLOCK_MONOTONIC "
               "calibration never converged";
    }
}

} // namespace

// 13.3.a — the reference device (RADV on gfx1151), via the default ICD.
TEST(ProfilerVulkanDevice, bracketsResolveAtEventTierOnTheDefaultDevice) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerVulkanDevice.bracketsResolveAtEventTierOnTheDefaultDevice",
            nullptr, out);
        if (rc != 0) FAIL() << "the Vulkan device child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos)
            GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }
    runBracketBody();
}

// 13.1.i / 13.3.b — the same mechanics on lavapipe, the CI lane. What it can
// prove is the PLUMBING: query pools, availability, reuse, the wait span.
// What it cannot prove is clock correlation — a software rasterizer's device
// clock IS the host clock — which is exactly the documented caveat.
TEST(ProfilerVulkanDevice, theSamePlumbingRunsOnLavapipe) {
    if (::getenv(kChildMarker) == nullptr) {
        FILE* f = ::fopen(kLavapipeIcd, "rb");
        if (!f) GTEST_SKIP() << "no lavapipe ICD at " << kLavapipeIcd;
        ::fclose(f);
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerVulkanDevice.theSamePlumbingRunsOnLavapipe",
            kLavapipeIcd, out);
        if (rc != 0) FAIL() << "the lavapipe child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos)
            GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }
    runBracketBody();
}
