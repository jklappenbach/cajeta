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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
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

    // 8.1.c: the launch id round-tripped as the SDK's external correlation id.
    // Nothing else could have produced this event: resolve publishes only on an
    // exact launch-id match against a parked launch, and drops anything it
    // cannot match rather than guessing.
    EXPECT_GT(device->launch_id, 0);
    EXPECT_STRNE(device->kernel_name ? device->kernel_name : "", "");
}
