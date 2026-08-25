// cajeta-profiler Unit 6.6 — GPU capture is armed by a real run (spec §9.1,
// §9.6, §8.3).
//
// Found while trying to produce a GPU trace fixture for the viewer:
// CAJETA_PROFILER=1 armed the SAMPLER only. Nothing registered a GPU sink, so a
// profiled run of a GPU program wrote a CPU-sampled trace with no device track,
// and Units 7 and 8 were reachable only from tests that attached a writer by
// hand. Measured on a real amdgpu run of samples/profile: two tracks,
// cajeta.profiler and cajeta.thread.0, and nothing else.
//
// The events go into the SAME file as the samples. §8.3 wants host, fiber and
// device on one time axis and §8.8 wants that to be one file in Perfetto with
// no export step — and §3.4 already settled the identical question for
// instrumentation.
#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../../runtime/native/cajeta_prof_abi.h"

using cajeta_test::CajetaJit;

namespace {

struct Cap {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*arm)(void)            = nullptr;
    int64_t (*shutdown)(void)       = nullptr;
    void    (*shutdownReset)(void)  = nullptr;
    int32_t (*isArmed)(void)        = nullptr;
    int64_t (*captured)(void)       = nullptr;
    int64_t (*capDropped)(void)     = nullptr;
    int32_t (*capArm)(int32_t)      = nullptr;
    void    (*capDisarm)(void)      = nullptr;
    int32_t (*gpuFlush)(void)       = nullptr;
    void    (*disarm)(void)         = nullptr;
    void    (*launch)(const char*, int32_t, int32_t, int32_t, int32_t, int32_t,
                      int32_t, uint32_t, int64_t, int32_t, int32_t,
                      void (*)(void*), void*) = nullptr;
};

Cap& cap() {
    static Cap c = [] {
        Cap x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class C {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.C");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.arm           = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_arm"));
        x.shutdown      = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_shutdown"));
        x.shutdownReset = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_shutdown_reset"));
        x.isArmed       = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_is_armed"));
        x.captured      = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_gpu_captured"));
        x.capDropped    = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_gpu_capture_dropped"));
        x.capArm        = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_capture_arm"));
        x.capDisarm     = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_gpu_capture_disarm"));
        x.gpuFlush      = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
        x.disarm        = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_disarm"));
        x.launch        = reinterpret_cast<decltype(x.launch)>(sym("__cajeta_prof_gpu_launch"));
        return x;
    }();
    return c;
}

void noKernel(void*) {}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Each test gets its own trace path and its own armed/disarmed cycle.
struct Armed {
    std::string path;
    explicit Armed(const char* tag, const char* ring = nullptr) {
        path = std::string("cajeta-gpu-capture-") + tag + "-" +
               std::to_string(cajeta_getpid()) + ".pftrace";
        ::setenv("CAJETA_PROFILER", "1", 1);
        ::setenv("CAJETA_PROFILER_OUT", path.c_str(), 1);
        if (ring) ::setenv("CAJETA_PROFILER_GPU_RING", ring, 1);
        // Start from a known state. __cajeta_prof_arm() returns -1 immediately
        // when the sampler is already armed, and returns BEFORE it arms GPU
        // capture — so a preceding test that did not shut down would leave this
        // one silently unarmed and passing for the wrong reason.
        cap().disarm();
        cap().shutdownReset();
        cap().capDisarm();
        cap().arm();
    }
    ~Armed() {
        cap().disarm();
        cap().capDisarm();
        cap().shutdownReset();
        ::unsetenv("CAJETA_PROFILER");
        ::unsetenv("CAJETA_PROFILER_OUT");
        ::unsetenv("CAJETA_PROFILER_GPU_RING");
        std::remove(path.c_str());
    }
};

const int32_t kBackendCpu = 3;

} // namespace

// ── 6.6.1.b — arming the profiler arms GPU capture ──────────────────────

TEST(ProfilerGpuCapture, armingTheProfilerAlsoArmsGpuCapture) {
    Cap& c = cap();
    ASSERT_TRUE(c.arm && c.isArmed && c.capDisarm);

    c.capDisarm();
    ::unsetenv("CAJETA_PROFILER");
    EXPECT_EQ(c.isArmed(), 0) << "something was armed before anything armed it";

    Armed run("armed");
    // Unit 8's rocprofiler configure hook gates on exactly this, and it fires
    // at HIP init — so if this is false, the §5.2.3 window is missed on every
    // real run and the whole ROCm backend degrades to host tier silently.
    EXPECT_EQ(c.isArmed(), 1)
        << "CAJETA_PROFILER armed the sampler but not GPU capture (§9.1, §9.6)";
}

// ── 6.6.1.a — captured launches reach the sampler's own trace ───────────

TEST(ProfilerGpuCapture, capturedLaunchesLandInTheSameFileAsTheSamples) {
    Cap& c = cap();
    ASSERT_TRUE(c.launch && c.shutdown && c.captured);

    Armed run("same-file");
    for (int i = 0; i < 4; ++i)
        c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, kBackendCpu, noKernel, nullptr);
    // Batched delivery is asynchronous. The drain path flushes before it emits,
    // so the test does the same rather than racing the delivery thread.
    c.gpuFlush();
    EXPECT_EQ(c.captured(), 4);

    ASSERT_GT(c.shutdown(), 0) << "shutdown wrote no packets";
    const std::string body = slurp(run.path);
    ASSERT_FALSE(body.empty()) << "no trace at " << run.path;

    // Track names are plain strings on the wire. The device/context/queue
    // hierarchy is what §8.3 asks to see beside the host and fiber tracks.
    EXPECT_NE(body.find("cajeta.xpu."), std::string::npos)
        << "the trace carries no device track — a profiled GPU run that looks "
           "exactly like a CPU one is the failure this unit exists for";
    EXPECT_NE(body.find("queue "), std::string::npos);
    // And the samples are still there: one file, both halves.
    EXPECT_NE(body.find("cajeta.thread."), std::string::npos)
        << "the GPU half displaced the host half instead of joining it";
}

// ── 6.6.1.c — a run with no GPU work pays nothing and shows nothing ─────

TEST(ProfilerGpuCapture, aRunWithNoGpuWorkGetsNoDeviceTrack) {
    Cap& c = cap();
    Armed run("no-gpu");
    EXPECT_EQ(c.captured(), 0);

    c.shutdown();
    const std::string body = slurp(run.path);
    // An empty device track on every CPU profile would be worse than useless:
    // it invites the reader to wonder what the GPU was doing.
    EXPECT_EQ(body.find("cajeta.xpu."), std::string::npos)
        << "a run that never touched the GPU grew a device track";
}

// ── bounded, and honest about it ────────────────────────────────────────

TEST(ProfilerGpuCapture, captureIsBoundedAndCountsWhatItDropped) {
    Cap& c = cap();
    Armed run("bounded", "8");   // eight slots

    for (int i = 0; i < 40; ++i)
        c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, kBackendCpu, noKernel, nullptr);

    c.gpuFlush();
    // Bounded, because a profiler that grows without limit changes the program
    // it is measuring — and one that blocks changes it more.
    EXPECT_EQ(c.captured(), 8);
    EXPECT_EQ(c.capDropped(), 32)
        << "drops must be counted; a silent drop is a measurement that lies "
           "about how much it saw";
}

TEST(ProfilerGpuCapture, disarmingStopsCapture) {
    Cap& c = cap();
    {
        Armed run("disarm");
        c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, kBackendCpu, noKernel, nullptr);
        c.gpuFlush();
        ASSERT_EQ(c.captured(), 1);
    }
    // The Armed destructor disarmed. A launch now must not be recorded, or a
    // second profiled region in one process would inherit the first's events.
    c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, kBackendCpu, noKernel, nullptr);
    c.gpuFlush();
    EXPECT_EQ(c.captured(), 0);
}
