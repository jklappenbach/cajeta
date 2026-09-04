// cajeta-profiler 12.x — device dispatch records on a real NVIDIA GPU.
//
// The NVPTX twin of ProfilerRocmDispatchDeviceTests, and it exists for the same
// reason: everything else in Unit 12 checks the machinery around the
// measurement — loading libcupti, binding entry points, pushing and popping the
// external correlation id, decoding a record's bytes. None of it checks that a
// real kernel launched by a real program comes back with a span the DEVICE
// supplied. Until something does, "CUPTI is ready" means only that a library
// loaded.
//
// The gap is not hypothetical. `caj_cupti_consume_buffer` — the two-pass walk
// that turns activity records into resolved launches — carries the comment
// "reachable only with a bound CUPTI, so it is exercised on the PHOENIX and
// phoenix-wsl lanes rather than here". This suite is that lane.
//
// It runs in a FRESH PROCESS, re-exec'ing the test binary for itself, for the
// same reason the ROCm suite does: CUPTI's activity kinds and its buffer
// callbacks have to be in place before the CUDA context that will produce the
// records exists. Any earlier test in the same binary that touches the GPU
// closes that window, and a suite that measured the degraded path while
// claiming to measure the device one would be worse than no suite.
//
// WSL2 note: the driver there REFUSES cuptiActivityRegisterTimestampCallback
// (CUptiResult 39), so these skip on phoenix-wsl and run on PHOENIX. That is a
// platform fact, not a defect in this code — see the Windows lane.
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

const char* kChildMarker = "CAJETA_CUDA_DISPATCH_CHILD";

// Deliberately the SAME source the ROCm device suite and the CUDA dispatch
// tests use. One program, every backend: if the number differs by device, the
// difference is the backend's, not the benchmark's.
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

// Two streams, two launches, no sync between them. The kernel spins long enough
// that a host which waited for stream 1 could not possibly have issued stream
// 2's launch before stream 1's kernel finished — which is what makes the
// serialization question answerable from the timestamps alone.
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

// The backend's symbols, looked up once. Named as a struct so a missing one
// fails with the name rather than a null dereference three lines later.
struct CuptiSyms {
    int32_t     (*init)(void)            = nullptr;
    int32_t     (*configure)(void)       = nullptr;
    int32_t     (*tracing)(void)         = nullptr;
    const char* (*reason)(void)          = nullptr;
    int32_t     (*state)(void)           = nullptr;
    int64_t     (*records)(void)         = nullptr;
    int64_t     (*rejected)(void)        = nullptr;
    int64_t     (*extRecords)(void)      = nullptr;
    int64_t     (*unmapped)(void)        = nullptr;
    int32_t     (*kindsEnabled)(void)    = nullptr;
    int32_t     (*flush)(void)           = nullptr;
    int64_t     (*pushes)(void)          = nullptr;
    int64_t     (*pops)(void)            = nullptr;
    int32_t     (*sinkReg)(CajetaGpuSinkFn, void*, int32_t) = nullptr;
    int32_t     (*sinkUnreg)(int32_t)    = nullptr;
    int32_t     (*gpuCollect)(int32_t)   = nullptr;
    int32_t     (*gpuFlush)(void)        = nullptr;
    int32_t     (*checkDispatch)(const CajetaGpuEvent*) = nullptr;
};

CuptiSyms bind(CajetaJit* jit) {
    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    CuptiSyms s;
    s.init         = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_init"));
    s.configure    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_configure"));
    s.tracing      = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_tracing"));
    s.reason       = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_cupti_reason"));
    s.state        = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_state"));
    s.records      = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_cupti_records"));
    s.rejected     = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_cupti_rejected"));
    s.extRecords   = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_cupti_ext_records"));
    s.unmapped     = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_cupti_unmapped"));
    s.kindsEnabled = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_kinds_enabled"));
    s.flush        = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_cupti_flush"));
    s.pushes       = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_cupti_pushes"));
    s.pops         = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_cupti_pops"));
    s.sinkReg      = reinterpret_cast<int32_t (*)(CajetaGpuSinkFn, void*, int32_t)>(
                         sym("__cajeta_prof_gpu_sink_register"));
    s.sinkUnreg    = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
    s.gpuCollect   = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_collect"));
    s.gpuFlush     = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
    s.checkDispatch = reinterpret_cast<int32_t (*)(const CajetaGpuEvent*)>(
                         sym("__cajeta_prof_check_dispatch"));
    return s;
}

CajetaJit::Options cudaOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    return o;
}

} // namespace


// ── 12.x.a / 12.x.b — the span came from the device, and it is this launch's ──
TEST(ProfilerCudaDispatchDevice, dispatchRecordsCarryDeviceSpansAndTheLaunchId) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerCudaDispatchDevice.dispatchRecordsCarryDeviceSpansAndTheLaunchId", out);
        if (rc != 0) {
            FAIL() << "the CUDA device-dispatch child failed (exit " << rc << "):\n" << out;
        }
        if (out.find("[  SKIPPED ]") != std::string::npos) {
            GTEST_SKIP() << "child skipped:\n" << out;
        }
        SUCCEED() << out;
        return;
    }

    // ── everything below runs in the fresh child ──
    //
    // Compiling does not touch the CUDA driver — it goes through LLVM to PTX to
    // ptxas — so the arming window is still open afterwards. Arming BEFORE
    // asking whether a device exists is deliberate: that question is itself a
    // driver call, and asking it first would narrow the window for no reason.
    auto jit = CajetaJit::compile(kSaxpySource, "test.Saxpy", cudaOptions());
    ASSERT_NE(jit, nullptr);

    const CuptiSyms s = bind(jit.get());
    ASSERT_TRUE(s.init && s.tracing && s.records && s.kindsEnabled);
    ASSERT_TRUE(s.sinkReg && s.sinkUnreg && s.gpuCollect && s.gpuFlush);
    // The arming step. ROCm spells it __cajeta_prof_rocm_configure and the
    // CUDA backend needs the exact counterpart: registering the activity
    // buffer callbacks and enabling the kinds whose records this test is
    // about. Binding libcupti (init) is NOT arming — `tracing()` requires
    // kinds_enabled > 0, and nothing in the shipping backend ever enables one.
    ASSERT_NE(s.configure, nullptr)
        << "__cajeta_prof_cupti_configure is missing: libcupti binds, the "
           "buffer walk exists, and nothing connects them — no activity "
           "callbacks are registered and no activity kind is ever enabled, so "
           "__cajeta_prof_cupti_tracing() is false in every shipping build and "
           "every CUDA launch drains at host tier";

    s.init();
    const int32_t configured = s.configure();

    // Arming is what this suite is about, so a machine that CAN arm and did
    // not is a failure, not a skip. Only the absence of the hardware or the
    // library is a skip.
    if (!::cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device/driver available";
    if (s.state() != CAJETA_CUPTI_READY) {
        GTEST_SKIP() << "CUPTI did not bind here ("
                     << (s.reason ? s.reason() : "no reason") << ")";
    }
    std::printf(" RESULT u12_kinds_enabled=%d\n", (int) s.kindsEnabled());
    std::printf(" RESULT u12_configure_rc=%d\n", (int) configured);
    std::printf(" RESULT u12_reason=%s\n", s.reason ? s.reason() : "none");
    // Checking the RETURN, not just the side effect. configure() reports 0
    // when an enable was refused while still leaving kinds_enabled > 0 and
    // tracing() true — so a test that asserted only those would pass against a
    // half-armed backend, which is precisely the state that yields kernel
    // records with nothing to attribute them to.
    ASSERT_EQ(configured, 1)
        << "configure() did not fully arm the backend ("
        << (s.reason ? s.reason() : "no reason") << ")";
    ASSERT_GE(s.kindsEnabled(), 3)
        << "CONCURRENT_KERNEL, EXTERNAL_CORRELATION and DRIVER must all be "
           "enabled: a kernel is resolvable only THROUGH a correlation record, "
           "and correlation records ride the driver API stream ("
        << (s.reason ? s.reason() : "no reason") << ")";
    ASSERT_EQ(s.tracing(), 1)
        << "CUPTI is bound with kinds enabled but not tracing ("
        << (s.reason ? s.reason() : "no reason") << ")";

    g_caught.clear();
    const int32_t sink = s.sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_FLOAT_EQ(run(), 4096.0f)
        << "the kernel did not produce the right answer, so whatever was timed "
           "was not this computation";

    // Wait for CUPTI to DELIVER before collecting, and flush — never collect —
    // while waiting. `collect` is flush + drain_pending: it publishes every
    // still-parked launch at host tier and destroys its chance, so a record
    // arriving one flush too late finds nothing to claim. `s.sync()` guarantees
    // the kernel finished, not that its record has been buffered. Bounded at
    // ~500 ms: the contract is that records arrive promptly, so a wait this
    // long failing IS the finding. (The ROCm suite was flaky for exactly this
    // reason before it waited — 2026-08-27.)
    ASSERT_NE(s.flush, nullptr);
    for (int i = 0; i < 100 && s.records() == 0; ++i) {
        s.flush();
        if (s.records() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    s.gpuCollect(CAJ_GPU_BACKEND_CUDA);
    s.gpuFlush();
    s.sinkUnreg(sink);

    ASSERT_FALSE(g_caught.empty()) << "the launch produced no record at all";
    EXPECT_GT(s.records(), 0) << "CUPTI delivered no kernel activity records";

    // The correlation id must have been pushed and popped in balance: an
    // unbalanced pair leaves CUPTI attributing later kernels to this launch.
    EXPECT_GT(s.pushes(), 0) << "no external correlation id was ever pushed";
    EXPECT_EQ(s.pushes(), s.pops())
        << "push/pop of the external correlation id did not balance";

    const CajetaGpuEvent* device = nullptr;
    for (const auto& e : g_caught) if (e.tier == CAJETA_PROF_TIER_DEVICE) { device = &e; break; }
    // The discriminator, printed BEFORE the assertion that may end the test:
    // a kernel is resolvable only THROUGH an external-correlation record, so
    // ext=0 means the mapping records never arrived (a different bug, and a
    // different fix, from ext>0 with unmapped>0, which means they arrived and
    // did not match).
    std::printf(" RESULT u12_ext_correlation_records=%lld\n",
                (long long) (s.extRecords ? s.extRecords() : -1));
    std::printf(" RESULT u12_kernels_unmapped=%lld\n",
                (long long) (s.unmapped ? s.unmapped() : -1));

    ASSERT_NE(device, nullptr)
        << "every record came back at host tier — the activity record never "
           "reached the launch waiting for it; CUPTI records=" << s.records()
        << " rejected=" << (s.rejected ? s.rejected() : -1)
        << " ext_correlation=" << (s.extRecords ? s.extRecords() : -1)
        << " unmapped=" << (s.unmapped ? s.unmapped() : -1);

    // Published so the measurement is visible rather than merely asserted: a
    // test that only says "greater than zero" hides the case where the number
    // is technically non-zero and obviously wrong.
    std::printf(" RESULT u12_device_span_ns=%lld\n",
                (long long) (device->dev_end_ns - device->dev_start_ns));
    std::printf(" RESULT u12_host_window_ns=%lld\n",
                (long long) (device->host_return_ns - device->host_launch_ns));
    std::printf(" RESULT u12_activity_records=%lld\n", (long long) s.records());
    std::printf(" RESULT u12_records_rejected=%lld\n",
                (long long) (s.rejected ? s.rejected() : -1));
    std::printf(" RESULT u12_events_published=%d\n", (int) g_caught.size());

    // The span is the device's, and it is a real one.
    EXPECT_GT(device->dev_start_ns, 0);
    EXPECT_GT(device->dev_end_ns, device->dev_start_ns)
        << "a device span that does not advance is not a measurement";

    // The mapped device span has to land in the host's timeline. With the
    // timestamp callback registered (§6.2) records arrive already in the host
    // domain; if that registration were skipped or refused, raw CUPTI
    // timestamps would put the kernel somewhere else entirely — which is
    // precisely the WSL2 failure mode, and this bound is what would catch it.
    EXPECT_GT(device->dev_start_ns, device->host_launch_ns - 1000000000LL);
    EXPECT_LT(device->dev_end_ns, device->host_return_ns + 1000000000LL);

    // The positive control for the backend's own self-check: records DID
    // arrive, so the device path must still be enabled. Without this, a
    // self-check that fired unconditionally would pass every negative test and
    // silently downgrade every real run.
    ASSERT_NE(s.state, nullptr);
    EXPECT_EQ(s.state(), CAJETA_CUPTI_READY)
        << "the self-check disabled a backend that was delivering records";

    // The launch id round-tripped as CUPTI's external correlation id. Nothing
    // else could have produced this event: resolve publishes only on an exact
    // launch-id match against a parked launch, and drops what it cannot match
    // rather than guessing.
    EXPECT_GT(device->launch_id, 0);

    // Every device-tier record carries its resolution stamp, and no more
    // device-tier records exist than CUPTI delivered records to supply them.
    int32_t deviceTierCount = 0;
    for (const auto& e : g_caught) {
        if (e.tier != CAJETA_PROF_TIER_DEVICE) continue;
        deviceTierCount++;
        EXPECT_GT(e.resolved_ns, 0)
            << "a device-tier record with no resolution stamp — a device claim "
               "no activity record stands behind";
    }
    EXPECT_LE((int64_t) deviceTierCount, s.records())
        << "more device-tier spans than activity records exist to supply them";

    // A healthy asynchronous dispatch must come through the integrity checker
    // CLEAN. On the AMD side every span of a healthy run once wore
    // OUTSIDE_HOST, which taught readers to ignore the one flag that exists to
    // catch a sheared clock domain.
    ASSERT_NE(s.checkDispatch, nullptr);
    EXPECT_EQ(s.checkDispatch(device), CAJETA_SPAN_OK)
        << "a real device span from a healthy run was flagged (flags="
        << s.checkDispatch(device) << ")";
}


// ── 12.x.c — the profiler does not serialize the program (§5.1.3) ────────
//
// The CUDA half of the claim ProfilerRocmDispatchDevice makes for AMD. It
// fails differently from a wrong span and that is why it is separate: a
// profiler that flushes or synchronizes inside end_launch produces perfectly
// ordered, non-overlapping records of a program that in fact ran concurrently,
// and nothing in the trace says so.
//
// There is a second, CUDA-specific trap underneath it. CUPTI's
// CUPTI_ACTIVITY_KIND_KERNEL serializes kernel execution on the device;
// CONCURRENT_KERNEL does not. Enabling the wrong one changes the program being
// measured rather than observing it — the numbers stay internally consistent
// and describe a program the user never ran. This test is what makes that
// policy observable instead of merely commented.
TEST(ProfilerCudaDispatchDevice, twoStreamsAreNotSerializedByBeingMeasured) {
    if (::getenv(kChildMarker) == nullptr) {
        std::string out;
        const int rc = runInFreshProcess(
            "ProfilerCudaDispatchDevice.twoStreamsAreNotSerializedByBeingMeasured", out);
        if (rc != 0) FAIL() << "the two-stream child failed (exit " << rc << "):\n" << out;
        if (out.find("[  SKIPPED ]") != std::string::npos) GTEST_SKIP() << "child skipped:\n" << out;
        SUCCEED() << out;
        return;
    }

    auto jit = CajetaJit::compile(kTwoStreamSource, "test.Spin", cudaOptions());
    ASSERT_NE(jit, nullptr);

    const CuptiSyms s = bind(jit.get());
    ASSERT_TRUE(s.init && s.tracing && s.sinkReg && s.gpuCollect && s.gpuFlush);
    ASSERT_NE(s.configure, nullptr)
        << "__cajeta_prof_cupti_configure is missing — see the sibling test";

    s.init();
    s.configure();

    if (!::cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device/driver available";
    if (s.state() != CAJETA_CUPTI_READY) {
        GTEST_SKIP() << "CUPTI did not bind here ("
                     << (s.reason ? s.reason() : "no reason") << ")";
    }
    ASSERT_EQ(s.tracing(), 1)
        << "CUPTI bound but not tracing (" << (s.reason ? s.reason() : "no reason") << ")";

    g_caught.clear();
    const int32_t sink = s.sinkReg(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);

    auto run = jit->lookup<float (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_GT(run(), 0.0f) << "the spin kernel did not run";

    for (int i = 0; i < 100 && s.records() < 2; ++i) {
        s.flush();
        if (s.records() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    s.gpuCollect(CAJ_GPU_BACKEND_CUDA);
    s.gpuFlush();
    s.sinkUnreg(sink);

    ASSERT_GE(g_caught.size(), 2u) << "two launches produced fewer than two records";
    const CajetaGpuEvent* a = &g_caught[0];
    const CajetaGpuEvent* b = &g_caught[1];
    if (b->host_launch_ns < a->host_launch_ns) { const CajetaGpuEvent* t = a; a = b; b = t; }

    const long long gap  = (long long) (b->host_launch_ns - a->host_launch_ns);
    const long long span = (long long) (a->dev_end_ns - a->dev_start_ns);
    std::printf(" RESULT u12_two_stream_launch_gap_ns=%lld\n", gap);
    std::printf(" RESULT u12_two_stream_first_span_ns=%lld\n", span);

    ASSERT_GT(span, 0) << "the first kernel has no device span to compare against";
    // The host issued the second launch while the first kernel was still
    // running. A profiler that flushed or synchronized inside end_launch would
    // have made this gap at least as large as the kernel it waited for.
    EXPECT_LT(gap, span)
        << "the second launch was issued " << gap << " ns after the first, and "
           "the first kernel only ran for " << span << " ns — the measurement "
           "serialized the program (§5.1.3)";
}
