// cajeta-profiler 8.1.b / 8.1.c — device dispatch records, on a real GPU.
//
// This is the claim the whole ROCm backend exists to make: that a kernel's span
// on the trace came from the device, not from the host's view of the call. Every
// other test in Unit 8 checks the machinery around it — binding, configuring,
// parking, degrading. This one checks the measurement.
//
// It runs in a FRESH PROCESS, re-exec'ing the test binary for itself. rocprofiler
// intercepts HIP by installing itself while HIP loads (§5.2.3), so it must be
// configured before the first HIP call in the process. Any earlier test in the
// same binary that touches a GPU shuts that window, and there is no reopening it
// — the state would be CAJETA_ROCM_LATE and this test would be measuring the
// degraded path while claiming to measure the device one.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using cajeta_test::CajetaJit;
using cajeta::xpu::amd::HipDriver;

namespace {

const char* kChildMarker = "CAJETA_ROCM_DISPATCH_CHILD";

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

// Two streams, two launches, no sync between them. The kernel spins long
// enough that a host which waited for stream 1 could not possibly have issued
// stream 2's launch before stream 1's kernel finished — which is what makes the
// serialization question answerable from the timestamps.
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

// Re-run this one test in a virgin process and hand back what it printed.
int runInFreshProcess(const std::string& filter, std::string& out) {
    const std::string exe = cajeta_self_exe().string();
    if (exe.empty()) return -1;
    ::setenv(kChildMarker, "1", 1);
    const std::string cmd = "\"" + exe + "\" --gtest_filter=" + filter +
                            " --gtest_color=no 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) { ::unsetenv(kChildMarker); return -1; }
    char buf[4096];
    while (::fgets(buf, sizeof(buf), p)) out += buf;
    const int rc = ::pclose(p);
    ::unsetenv(kChildMarker);
    return rc;
}

} // namespace

TEST(ProfilerRocmDispatchDevice, dispatchRecordsCarryDeviceSpansAndTheLaunchId) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerRocmDispatchDevice.dispatchRecordsCarryDeviceSpansAndTheLaunchId", out);
        if (rc != 0) {
            FAIL() << "the device-dispatch child failed (exit " << rc << "):\n" << out;
        }
        if (out.find("[  SKIPPED ]") != std::string::npos) {
            GTEST_SKIP() << "child skipped:\n" << out;
        }
        SUCCEED() << out;
        return;
    }

    // ── everything below runs in the fresh child ──
    //
    // Compiling does not touch the HIP runtime — it goes through LLVM to HSACO —
    // so the configuration window is still open afterwards. Configuring BEFORE
    // asking HipDriver whether a device exists is deliberate: that question is
    // itself a HIP call, and asking it first would shut the window.
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    o.xpuArch = "gfx1100,gfx1151";
    auto jit = CajetaJit::compile(kSaxpySource, "test.Saxpy", o);
    ASSERT_NE(jit, nullptr);

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto rocmInit    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_init"));
    auto rocmConfig  = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_configure"));
    auto rocmTracing = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_tracing"));
    auto rocmReason  = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_rocm_reason"));
    auto rocmRecords = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_rocm_records"));
    auto sinkReg     = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(
                           sym("__cajeta_prof_gpu_sink_register"));
    auto sinkUnreg   = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
    auto gpuCollect  = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    auto gpuFlush    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    ASSERT_TRUE(rocmInit && rocmConfig && rocmTracing && sinkReg && gpuCollect && gpuFlush);

    rocmInit();
    rocmConfig();
    if (!rocmTracing()) {
        GTEST_SKIP() << "rocprofiler-sdk dispatch tracing not available here ("
                     << (rocmReason ? rocmReason() : "no reason") << ")";
    }
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    const int32_t sink = sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_FLOAT_EQ(run(), 4096.0f) << "the kernel did not produce the right answer, so "
                                       "whatever was timed was not this computation";

    // Wait for the SDK to DELIVER before collecting, and flush — never
    // collect — while waiting. `collect` is flush + drain_pending: it
    // publishes every still-parked launch at host tier and destroys its
    // chance, so a record arriving one flush too late finds nothing to claim.
    // `s.sync()` guarantees the kernel finished, not that its record has been
    // buffered; the gap is small and real, and it made this test flaky (two
    // runs of the same binary on the same tree, 2026-08-27: one NULL device
    // record, one clean 11,331 ns span). Bounded at ~500 ms: the contract is
    // that records arrive promptly, so a wait this long failing IS the
    // finding.
    auto rocmFlush = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_flush"));
    ASSERT_NE(rocmFlush, nullptr);
    for (int i = 0; i < 100 && rocmRecords() == 0; ++i) {
        rocmFlush();
        if (rocmRecords() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    gpuCollect(CAJ_GPU_BACKEND_HIP);
    gpuFlush();
    sinkUnreg(sink);

    ASSERT_FALSE(g_caught.empty()) << "the launch produced no record at all";
    EXPECT_GT(rocmRecords(), 0) << "rocprofiler delivered no kernel-dispatch records";

    const CajetaGpuEvent* device = nullptr;
    for (const auto& e : g_caught) if (e.tier == CAJETA_PROF_TIER_DEVICE) { device = &e; break; }
    ASSERT_NE(device, nullptr)
        << "every record came back at host tier — the dispatch record never "
           "reached the launch waiting for it (§5.2.4)";

    // Published so the measurement is visible rather than merely asserted: a
    // test that only says "greater than zero" hides the case where the number
    // is technically non-zero and obviously wrong.
    std::printf(" RESULT u8_device_span_ns=%lld\n",
                (long long) (device->dev_end_ns - device->dev_start_ns));
    std::printf(" RESULT u8_host_window_ns=%lld\n",
                (long long) (device->host_return_ns - device->host_launch_ns));
    std::printf(" RESULT u8_dispatch_records=%lld\n", (long long) rocmRecords());
    std::printf(" RESULT u8_events_published=%d\n", (int) g_caught.size());

    // 8.1.b: the span is the device's, and it is a real one.
    EXPECT_GT(device->dev_start_ns, 0);
    EXPECT_GT(device->dev_end_ns, device->dev_start_ns)
        << "a device span that does not advance is not a measurement";

    // The mapped device span has to land in the host's timeline. Raw
    // rocprofiler timestamps are CLOCK_BOOTTIME; publishing them unmapped would
    // put the kernel wherever the machine's suspend history happened to put it.
    EXPECT_GT(device->dev_start_ns, device->host_launch_ns - 1000000000LL);
    EXPECT_LT(device->dev_end_ns, device->host_return_ns + 1000000000LL);

    // The positive control for 8.2.d's self-check: records DID arrive, so the
    // device path must still be enabled. The disable path is reproduced in
    // ProfilerRocmTests with no-op thunks; without this, a self-check that
    // fired unconditionally would pass every test there and silently downgrade
    // every real run.
    auto rocmState = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_state"));
    ASSERT_NE(rocmState, nullptr);
    EXPECT_EQ(rocmState(), CAJETA_ROCM_READY)
        << "the self-check disabled a backend that was delivering records";
    EXPECT_EQ(rocmTracing(), 1);

    // 8.1.c: the launch id round-tripped as the SDK's external correlation id.
    // Nothing else could have produced this event: resolve publishes only on an
    // exact launch-id match against a parked launch, and drops anything it
    // cannot match rather than guessing.
    EXPECT_GT(device->launch_id, 0);
    EXPECT_STRNE(device->kernel_name ? device->kernel_name : "", "");

    // 6.7.1.a — a TIER_DEVICE span exists only because a dispatch record
    // supplied it, and the trace's own account must agree: every device-tier
    // record carries its resolution stamp, no more device-tier records exist
    // than records the SDK delivered, and the backend that produced them
    // reports a non-zero record count. The 6.6.3 trace violated the last of
    // these (144 device spans beside rocm_records=0, from the pre-6.6.2.d
    // settle ordering) and nothing pinned it.
    int32_t deviceTierCount = 0;
    for (const auto& e : g_caught) {
        if (e.tier != CAJETA_PROF_TIER_DEVICE) continue;
        deviceTierCount++;
        EXPECT_GT(e.resolved_ns, 0)
            << "a device-tier record with no resolution stamp — a device claim "
               "no dispatch record stands behind";
    }
    EXPECT_LE((int64_t) deviceTierCount, rocmRecords())
        << "more device-tier spans than dispatch records exist to supply them";

    // 6.7.1.c on real hardware — a healthy asynchronous dispatch must come
    // through the integrity checker CLEAN. Before the causal bracket, every
    // span of a healthy gfx1151 run wore OUTSIDE_HOST, which taught readers to
    // ignore the one flag that exists to catch a sheared clock domain.
    auto checkDispatch = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
        sym("__cajeta_prof_check_dispatch"));
    ASSERT_NE(checkDispatch, nullptr);
    EXPECT_EQ(checkDispatch(device), CAJETA_SPAN_OK)
        << "a real device span from a healthy run was flagged (flags="
        << checkDispatch(device) << ")";
}


// ── 8.1.f — the profiler does not serialize the program (§5.1.3) ─────────
//
// The other half of 8.1.f. ProfilerRocmTests checks that overlapping spans
// survive the pending table; this checks that the launch path never waited for
// the device in the first place. Both matter and they fail differently: a
// profiler that flushes inside end_launch produces perfectly ordered,
// non-overlapping records of a program that in fact ran concurrently, and
// nothing in the trace says so.
//
// Fresh process, for the same reason as the test above: the configuration
// window closes at the first HIP call.
TEST(ProfilerRocmDispatchDevice, twoStreamsAreNotSerializedByBeingMeasured) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerRocmDispatchDevice.twoStreamsAreNotSerializedByBeingMeasured", out);
        if (rc != 0) FAIL() << "the two-stream child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos) GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }

    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    o.xpuArch = "gfx1100,gfx1151";
    auto jit = CajetaJit::compile(kTwoStreamSource, "test.Spin", o);
    ASSERT_NE(jit, nullptr);

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto rocmInit    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_init"));
    auto rocmConfig  = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_configure"));
    auto rocmTracing = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_tracing"));
    auto rocmReason  = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_rocm_reason"));
    auto sinkReg     = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(
                           sym("__cajeta_prof_gpu_sink_register"));
    auto sinkUnreg   = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
    auto gpuCollect  = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    auto gpuFlush    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    ASSERT_TRUE(rocmInit && rocmConfig && rocmTracing && sinkReg && gpuCollect && gpuFlush);

    rocmInit();
    rocmConfig();
    if (!rocmTracing()) {
        GTEST_SKIP() << "rocprofiler-sdk dispatch tracing not available here ("
                     << (rocmReason ? rocmReason() : "no reason") << ")";
    }
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    g_caught.clear();
    const int32_t sink = sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_GT(run(), 0.0f) << "the spin kernel did not run";

    gpuCollect(CAJ_GPU_BACKEND_HIP);
    gpuFlush();
    sinkUnreg(sink);

    ASSERT_GE(g_caught.size(), 2u) << "two launches produced fewer than two records";
    const CajetaGpuEvent* a = &g_caught[0];
    const CajetaGpuEvent* b = &g_caught[1];
    if (b->host_launch_ns < a->host_launch_ns) { const CajetaGpuEvent* t = a; a = b; b = t; }

    const long long gap  = (long long) (b->host_launch_ns - a->host_launch_ns);
    const long long span = (long long) (a->dev_end_ns - a->dev_start_ns);
    std::printf(" RESULT u8_two_stream_launch_gap_ns=%lld\n", gap);
    std::printf(" RESULT u8_two_stream_first_span_ns=%lld\n", span);

    ASSERT_GT(span, 0) << "the first kernel has no device span to compare against";
    // The host issued the second launch while the first kernel was still
    // running. A profiler that flushed or synchronized inside end_launch would
    // have made this gap at least as large as the kernel it waited for.
    EXPECT_LT(gap, span)
        << "the second launch was issued " << gap << " ns after the first, and the "
           "first kernel only ran for " << span << " ns — the measurement "
           "serialized the program (§5.1.3)";
}
