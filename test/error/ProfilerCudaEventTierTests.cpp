// cajeta-profiler — device timing on NVIDIA with NO developer tooling installed.
//
// CUPTI ships with the CUDA *Toolkit*, not the driver. A user who can run a
// cajeta kernel at all necessarily has the driver (libcuda / nvcuda.dll) and
// very often has nothing else — so a profiler that needs CUPTI for any device
// measurement is a profiler that reports host submit-to-complete for most of
// the people who run it.
//
// Vulkan already solved this one rung down: when no vendor layer is available
// it brackets the dispatch with core-API query-pool timestamps and resolves at
// TIER_EVENT. CUDA has the same material available — cuEventRecord and
// cuEventElapsedTime are DRIVER entry points, not toolkit ones — and this suite
// is the claim that it uses them.
//
// Every test here runs with CUPTI deliberately made unloadable
// (CAJETA_CUPTI_LIB pointed at a path that does not exist), so what it measures
// is exactly what a machine with only a driver would get. That also makes the
// suite meaningful on WSL2, where CUPTI cannot arm at all.
//
// The load-bearing test is the SECOND one. Reading an event's elapsed time
// requires the event to have completed, and the obvious implementation —
// synchronize in the launch epilogue and read it there — produces beautiful,
// precise, completely wrong traces: every kernel appears to run alone because
// the measurement is what stopped them overlapping. §5.1.3 forbids exactly
// this, and a bracket is far more tempting to implement that way than a vendor
// callback is.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../xpu/XpuDeviceTestUtil.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include "cajeta/xpu/XpuTarget.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kChildMarker = "CAJETA_CUDA_EVENT_CHILD";

// The same saxpy every backend's device suite runs.
const char* kSaxpySource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Saxpy {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 1024;\n"
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
    "        saxpy.launch(s, grid: [4], block: [256])(y, x, 2.0f, n);\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        x.free();\n"
    "        y.free();\n"
    "        float32 sum = 0.0f;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + hy[i]; }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

// Two streams, two long kernels, no sync between the launches.
const char* kTwoStreamSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Spin {\n"
    "    @Kernel\n"
    "    public static void spin(KernelBuffer<float32> y, uint32 n, uint32 iters) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            float32 v = y[i];\n"
    "            for (uint32 k = 0; k < iters; k = k + 1) { v = v * 1.0000001f + 1.0f; }\n"
    "            y[i] = v;\n"
    "        }\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 4096;\n"
    "        uint32 iters = 2000000;\n"
    "        float32[] h = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = 0.0f; }\n"
    "        KernelBuffer<float32> a = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> b = heap KernelBuffer<float32>(0, n);\n"
    "        a.allocate();\n"
    "        b.allocate();\n"
    "        a.upload(h);\n"
    "        b.upload(h);\n"
    "        KernelStream s1 #= KernelStream.create();\n"
    "        KernelStream s2 #= KernelStream.create();\n"
    "        spin.launch(s1, grid: [16], block: [256])(a, n, iters);\n"
    "        spin.launch(s2, grid: [16], block: [256])(b, n, iters);\n"
    "        s1.sync();\n"
    "        s2.sync();\n"
    "        a.download(h);\n"
    "        float32 first = h[0];\n"
    "        a.free();\n"
    "        b.free();\n"
    "        s1.destroy();\n"
    "        s2.destroy();\n"
    "        return first;\n"
    "    }\n"
    "}\n";

std::vector<CajetaGpuEvent> g_caught;

int32_t catchSink(const CajetaGpuEvent* recs, int32_t n, void*) {
    for (int32_t i = 0; i < n; ++i) g_caught.push_back(recs[i]);
    return 0;
}

// Re-run one test in a virgin process with CUPTI made unloadable, so the child
// measures what a driver-only machine measures. The variable has to be set
// before the child starts: the backend loads CUPTI once, on first use.
int runInFreshProcess(const std::string& filter, std::string& out) {
    const std::string exe = cajeta_self_exe().string();
    if (exe.empty()) return -1;
    ::setenv(kChildMarker, "1", 1);
    ::setenv("CAJETA_CUPTI_LIB", "/nonexistent/cajeta-forces-no-cupti", 1);
    const std::string cmd = "\"" + exe + "\" --gtest_filter=" + filter +
                            " --gtest_color=no 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) { ::unsetenv(kChildMarker); return -1; }
    char buf[4096];
    while (::fgets(buf, sizeof(buf), p)) out += buf;
    const int rc = ::pclose(p);
    ::unsetenv(kChildMarker);
    ::unsetenv("CAJETA_CUPTI_LIB");
    return rc;
}

CajetaJit::Options cudaOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    return o;
}

} // namespace


// ── a driver-only machine still gets a device-measured span ───────────────
TEST(ProfilerCudaEventTier, aLaunchIsDeviceTimedWithNoCuptiInstalled) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerCudaEventTier.aLaunchIsDeviceTimedWithNoCuptiInstalled", out);
        if (rc != 0) FAIL() << "the event-tier child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos) GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }

    auto jit = CajetaJit::compile(kSaxpySource, "test.Saxpy", cudaOptions());
    ASSERT_NE(jit, nullptr);

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto cuptiInit   = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_init"));
    auto cuptiState  = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_state"));
    auto eventsOk    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cuda_events_ok"));
    auto eventsReason = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_cuda_events_reason"));
    auto sinkReg     = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(
                           sym("__cajeta_prof_gpu_sink_register"));
    auto sinkUnreg   = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
    auto gpuCollect  = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    auto gpuFlush    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    auto checkDispatch = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
                           sym("__cajeta_prof_check_dispatch"));
    ASSERT_TRUE(sinkReg && sinkUnreg && gpuCollect && gpuFlush);

    // The whole point of the fallback: it is what answers when CUPTI cannot.
    ASSERT_NE(eventsOk, nullptr)
        << "__cajeta_prof_cuda_events_ok is missing — there is no CUDA "
           "event-tier fallback, so a machine with a driver and no CUDA "
           "Toolkit gets host submit-to-complete and nothing better";

    if (!::cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device/driver available";

    // Confirm the instrument: CUPTI must really be absent, or this test would
    // be measuring the device path and calling it the fallback.
    if (cuptiInit) cuptiInit();
    ASSERT_NE(cuptiState, nullptr);
    ASSERT_NE(cuptiState(), CAJETA_CUPTI_READY)
        << "CUPTI bound despite CAJETA_CUPTI_LIB pointing at nothing — this "
           "test cannot tell the fallback from the device path";

    g_caught.clear();
    const int32_t sink = sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_FLOAT_EQ(run(), 4096.0f)
        << "the kernel did not produce the right answer, so whatever was timed "
           "was not this computation";

    gpuCollect(CAJ_GPU_BACKEND_CUDA);
    gpuFlush();
    sinkUnreg(sink);

    ASSERT_TRUE(eventsOk())
        << "the CUDA event bracket did not arm on a machine with a working "
           "driver (" << (eventsReason ? eventsReason() : "no reason") << ")";
    ASSERT_FALSE(g_caught.empty()) << "the launch produced no record at all";

    const CajetaGpuEvent* ev = nullptr;
    for (const auto& e : g_caught) if (e.tier == CAJETA_PROF_TIER_EVENT) { ev = &e; break; }
    ASSERT_NE(ev, nullptr)
        << "no record resolved at EVENT tier — every span came back at host "
           "tier, which is what this fallback exists to stop ("
        << (eventsReason ? eventsReason() : "no reason") << ")";

    const long long span = (long long) (ev->dev_end_ns - ev->dev_start_ns);
    std::printf(" RESULT u12e_event_span_ns=%lld\n", span);
    std::printf(" RESULT u12e_host_window_ns=%lld\n",
                (long long) (ev->host_return_ns - ev->host_launch_ns));
    std::printf(" RESULT u12e_events_published=%d\n", (int) g_caught.size());

    EXPECT_GT(ev->dev_start_ns, 0);
    EXPECT_GT(span, 0) << "a device span that does not advance is not a measurement";
    // A 1024-element saxpy on any CUDA device this targets is microseconds.
    // The bound is loose on purpose — it is here to catch a span built from
    // the wrong clock or the wrong unit (cuEventElapsedTime answers in
    // FLOAT MILLISECONDS, and a missing conversion lands six orders of
    // magnitude out), not to police performance.
    EXPECT_LT(span, 1000000000LL)
        << "a 1024-element saxpy did not take a second; the span's unit or "
           "clock is wrong";

    // The span has to land on the host timeline near the launch that caused
    // it. An unanchored event bracket measures a correct DURATION and places
    // it wherever the device's own epoch happens to fall — which reads as a
    // plausible trace right up until you compare it with a CPU track.
    EXPECT_GT(ev->dev_start_ns, ev->host_launch_ns - 1000000000LL)
        << "the span is placed far before the launch — the bracket is not "
           "anchored to the host clock";
    EXPECT_LT(ev->dev_start_ns, ev->host_launch_ns + 1000000000LL)
        << "the span is placed far after the launch — the bracket is not "
           "anchored to the host clock";

    // The two quantities the causal bracket is judged on, published so a
    // future failure says HOW FAR out the placement was rather than only that
    // it was out — microseconds is anchor uncertainty, milliseconds is a bug.
    std::printf(" RESULT u12e_start_minus_launch_ns=%lld\n",
                (long long) (ev->dev_start_ns - ev->host_launch_ns));
    std::printf(" RESULT u12e_end_minus_resolved_ns=%lld\n",
                (long long) (ev->dev_end_ns - ev->resolved_ns));

    EXPECT_GT(ev->launch_id, 0);
    EXPECT_GT(ev->resolved_ns, 0)
        << "an event-tier record with no resolution stamp";
    if (checkDispatch) {
        EXPECT_EQ(checkDispatch(ev), CAJETA_SPAN_OK)
            << "a healthy event-tier span was flagged (flags="
            << checkDispatch(ev) << ")";
    }
}


// ── the bracket must not buy its numbers with a synchronize ───────────────
//
// This is the test that constrains the IMPLEMENTATION, not just the output.
// cuEventElapsedTime requires both events to have completed, so the cheapest
// way to satisfy the test above is to cuEventSynchronize in the launch
// epilogue — which serializes every launch against its own completion. The
// trace would then show two sequential kernels for a program that ran two
// concurrent ones, and nothing in the trace would say so.
TEST(ProfilerCudaEventTier, theEventBracketDoesNotSerializeTwoStreams) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerCudaEventTier.theEventBracketDoesNotSerializeTwoStreams", out);
        if (rc != 0) FAIL() << "the two-stream child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos) GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }

    auto jit = CajetaJit::compile(kTwoStreamSource, "test.Spin", cudaOptions());
    ASSERT_NE(jit, nullptr);

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto eventsOk  = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cuda_events_ok"));
    auto sinkReg   = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(
                         sym("__cajeta_prof_gpu_sink_register"));
    auto sinkUnreg = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
    auto gpuCollect = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    auto gpuFlush   = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    ASSERT_TRUE(sinkReg && sinkUnreg && gpuCollect && gpuFlush);
    ASSERT_NE(eventsOk, nullptr) << "no CUDA event-tier fallback — see the sibling test";

    if (!::cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device/driver available";

    g_caught.clear();
    const int32_t sink = sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_GT(run(), 0.0f) << "the spin kernel did not run";

    gpuCollect(CAJ_GPU_BACKEND_CUDA);
    gpuFlush();
    sinkUnreg(sink);

    ASSERT_GE(g_caught.size(), 2u) << "two launches produced fewer than two records";
    const CajetaGpuEvent* a = &g_caught[0];
    const CajetaGpuEvent* b = &g_caught[1];
    if (b->host_launch_ns < a->host_launch_ns) { const CajetaGpuEvent* t = a; a = b; b = t; }

    const long long gap  = (long long) (b->host_launch_ns - a->host_launch_ns);
    const long long span = (long long) (a->dev_end_ns - a->dev_start_ns);
    std::printf(" RESULT u12e_two_stream_launch_gap_ns=%lld\n", gap);
    std::printf(" RESULT u12e_two_stream_first_span_ns=%lld\n", span);

    ASSERT_GT(span, 0) << "the first kernel has no device span to compare against";
    EXPECT_LT(gap, span)
        << "the second launch was issued " << gap << " ns after the first, and "
           "the first kernel only ran for " << span << " ns — the event bracket "
           "waited for the device inside the launch path and serialized the "
           "program (§5.1.3)";
}
