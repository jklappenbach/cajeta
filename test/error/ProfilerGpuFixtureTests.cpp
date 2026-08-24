// cajeta-profiler Unit 11 — the GPU trace fixture the viewer is tested against
// (plan 11.1.e, 11.1.f).
//
// The viewer's other fixture, `tour.pftrace`, is a real profiled run of
// samples/tour. It has host and fiber tracks and no GPU work at all, so it
// cannot exercise the two things §8.4 and §8.6 ask for: a flow from a device
// slice back to the line that launched it, and a visible difference between a
// measurement that came from the device and one that did not.
//
// This generator produces that second fixture. It is a GENERATOR rather than a
// hand-written file for the reason 11.1.a gives: a fixture built by the test
// only proves the reader agrees with what the test author believed the writer
// does, and the two beliefs drift together. Every byte here is encoded by
// `cajeta_rt_prof_trace.c` — the same writer a real run uses.
//
// What is synthetic is the EVENTS, not the encoding. Real launches through
// Unit 7's CPU-emulation lane supply real timings, real call sites and real
// flow ids at host tier; copies of those events are then republished at device
// and event tier, and one with integrity flags set, because no lane available
// without a GPU produces a device-tier span and §8.6 is about telling them
// apart. The alternative was to wait for hardware to hand us a degraded span,
// which is to say never to test the degraded path.
//
// Skipped unless CAJETA_PROFILER_FIXTURE_OUT names a path, so a normal test run
// does not write files anybody has to clean up. To regenerate:
//
//   CAJETA_PROFILER_FIXTURE_OUT=ide-plugins/idea/src/test/resources/profiler/gpu.pftrace \
//     ./build/test/cajeta_test --gtest_filter=ProfilerGpuFixture.*
#include "gtest/gtest.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../../runtime/native/cajeta_prof_abi.h"

using cajeta_test::CajetaJit;

namespace {

struct Fix {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*arm)(void)           = nullptr;
    void    (*disarm)(void)        = nullptr;
    int64_t (*shutdown)(void)      = nullptr;
    void    (*shutdownReset)(void) = nullptr;
    int32_t (*capArm)(int32_t)     = nullptr;
    void    (*capDisarm)(void)     = nullptr;
    int32_t (*gpuFlush)(void)      = nullptr;
    int32_t (*sinkRegister)(CajetaGpuSinkFn, void*, int32_t) = nullptr;
    int32_t (*sinkUnregister)(int32_t) = nullptr;
    void    (*publish)(const CajetaGpuEvent*) = nullptr;
    void    (*launch)(const char*, int32_t, int32_t, int32_t, int32_t, int32_t,
                      int32_t, uint32_t, int64_t, int32_t, int32_t,
                      void (*)(void*), void*) = nullptr;
};

Fix& fix() {
    static Fix f = [] {
        Fix x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class G {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.G");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.arm            = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_arm"));
        x.disarm         = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_disarm"));
        x.shutdown       = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_shutdown"));
        x.shutdownReset  = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_shutdown_reset"));
        x.capArm         = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_capture_arm"));
        x.capDisarm      = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_gpu_capture_disarm"));
        x.gpuFlush       = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_gpu_flush"));
        x.sinkRegister   = reinterpret_cast<decltype(x.sinkRegister)>(sym("__cajeta_prof_gpu_sink_register"));
        x.sinkUnregister = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_sink_unregister"));
        x.publish        = reinterpret_cast<void (*)(const CajetaGpuEvent*)>(sym("__cajeta_prof_gpu_publish"));
        x.launch         = reinterpret_cast<decltype(x.launch)>(sym("__cajeta_prof_gpu_launch"));
        return x;
    }();
    return f;
}

// A kernel that takes measurable time, so the device slices in the fixture have
// non-zero spans. A zero-length slice renders as nothing and would make the
// fixture agree with a viewer that dropped every span.
void spin(void* arg) {
    volatile int64_t n = *static_cast<int64_t*>(arg);
    volatile int64_t acc = 0;
    for (int64_t i = 0; i < n; ++i) acc += i;
}

std::vector<CajetaGpuEvent>& captured() {
    static std::vector<CajetaGpuEvent> v;
    return v;
}

int32_t capture_sink(const CajetaGpuEvent* recs, int32_t n, void*) {
    for (int32_t i = 0; i < n; ++i) captured().push_back(recs[i]);
    return 0;
}

const int32_t kBackendCpu = 3;

// Call sites for the fixture's launches.
//
// A launch driven from this C++ test has an EMPTY Cajeta shadow stack, so
// `caj_gpu_call_site` finds nothing and the writer interns no source location —
// the flow arrives at a launch site with nowhere to navigate, which is the one
// thing §8.4 asks for. These stand in for what LineInfoCodegen emits at a
// method prologue: static, program-lifetime, exactly the shape the seam reads.
const CajetaFrameDesc kSaxpySite     = { "gpu.Saxpy",     "apply",     "gpu/Saxpy.cajeta" };
const CajetaFrameDesc kReduceSite    = { "gpu.Reduce",    "sum",       "gpu/Reduce.cajeta" };
const CajetaFrameDesc kTransposeSite = { "gpu.Transpose", "transpose", "gpu/Transpose.cajeta" };

// Distinct lines per launch, so a viewer that links a kernel to the WRONG
// launch site produces a visibly wrong line rather than a plausible one.
const CajetaFrameDesc* siteFor(const char* kernel) {
    if (std::strcmp(kernel, "saxpy") == 0)  return &kSaxpySite;
    if (std::strcmp(kernel, "reduce") == 0) return &kReduceSite;
    return &kTransposeSite;
}

} // namespace

// ── the generator ───────────────────────────────────────────────────────

TEST(ProfilerGpuFixture, writeGpuTrace) {
    const char* out = ::getenv("CAJETA_PROFILER_FIXTURE_OUT");
    if (!out || !*out) {
        GTEST_SKIP() << "set CAJETA_PROFILER_FIXTURE_OUT to regenerate the fixture";
    }
    Fix& f = fix();
    ASSERT_TRUE(f.launch && f.publish && f.sinkRegister && f.shutdown);

    // Pass 1: real launches, captured rather than written, so their call sites
    // and flow ids can be reused by the synthetic tiers below.
    captured().clear();
    f.disarm();
    f.shutdownReset();
    f.capDisarm();
    const int32_t sink = f.sinkRegister(capture_sink, nullptr, CAJETA_GPU_SINK_BATCHED);
    ASSERT_GE(sink, 0);

    int64_t small = 20000, large = 400000;
    f.launch("saxpy",   256, 1, 1, 64, 1, 1, 0, 0, 0, kBackendCpu, spin, &large);
    f.launch("reduce",  128, 1, 1, 64, 1, 1, 0, 0, 0, kBackendCpu, spin, &small);
    f.launch("saxpy",   256, 1, 1, 64, 1, 1, 0, 1, 0, kBackendCpu, spin, &small);
    f.launch("transpose", 64, 1, 1, 32, 1, 1, 2048, 1, 0, kBackendCpu, spin, &large);
    f.gpuFlush();
    f.sinkUnregister(sink);
    ASSERT_EQ(captured().size(), 4u) << "the CPU lane did not deliver every launch";

    // Pass 2: arm for real, replay the captured events, and add the tiers a
    // machine with no GPU cannot otherwise produce.
    ::setenv("CAJETA_PROFILER", "1", 1);
    ::setenv("CAJETA_PROFILER_OUT", out, 1);
    f.disarm();
    f.shutdownReset();
    f.capDisarm();
    ASSERT_EQ(f.arm(), 0) << "profiler did not arm";

    std::vector<CajetaGpuEvent> src = captured();
    for (size_t i = 0; i < src.size(); ++i) {
        src[i].call_site = siteFor(src[i].kernel_name);
        src[i].call_site_line = 40 + static_cast<int32_t>(i) * 7;
    }

    // Host tier, exactly as measured: submit-to-complete, no device involved.
    for (const CajetaGpuEvent& e : src) f.publish(&e);

    // Device tier: the span a vendor dispatch record would have supplied. Held
    // strictly inside the host window, resolution stamped after the span ends
    // — the causal bracket an honest device span sits in, and what Unit 9's
    // integrity check expects (plan 6.7.2.c).
    for (size_t i = 0; i < src.size(); ++i) {
        CajetaGpuEvent d = src[i];
        d.launch_id += 1000;
        d.tier = CAJETA_PROF_TIER_DEVICE;
        const int64_t window = d.host_return_ns - d.host_launch_ns;
        d.dev_start_ns = d.host_launch_ns + window / 8;
        d.dev_end_ns   = d.host_return_ns - window / 8;
        d.resolved_ns  = d.host_return_ns + window;
        f.publish(&d);
    }

    // Event tier: device event bracketing. Coarser than a dispatch record and
    // §8.6 asks that a reader be able to see which one they are looking at.
    {
        CajetaGpuEvent e = src[0];
        e.launch_id += 2000;
        e.kernel_name = "reduce";
        e.call_site = siteFor(e.kernel_name);   // renamed, so re-derive the site
        e.tier = CAJETA_PROF_TIER_EVENT;
        const int64_t window = e.host_return_ns - e.host_launch_ns;
        e.dev_start_ns = e.host_launch_ns + window / 4;
        e.dev_end_ns   = e.host_return_ns - window / 4;
        f.publish(&e);
    }

    // A span the integrity checker will flag. §11.3 says a flagged span still
    // renders and only the annotation says it should not be trusted, so the
    // fixture has to contain one for the viewer to have anything to render
    // differently.
    //
    // OUTSIDE_HOST via a SHEARED CLOCK DOMAIN — §6.5's real failure, the one
    // the causal bracket exists to catch: the span ends after the moment its
    // own record was read, which no real execution can. (Before 6.7 this
    // exemplar was "span after host_return", but that is what every healthy
    // asynchronous dispatch looks like and is no longer a fault.) Not a
    // negative span, because a negative span emits its END before its BEGIN,
    // which leaves a NEIGHBOURING slice unclosed on the same track — the
    // fixture would then be testing the viewer against damage the flag is not
    // about.
    {
        CajetaGpuEvent bad = src[1];
        bad.launch_id += 3000;
        bad.kernel_name = "transpose";
        bad.call_site = siteFor(bad.kernel_name);
        bad.tier = CAJETA_PROF_TIER_DEVICE;
        const int64_t window = bad.host_return_ns - bad.host_launch_ns;
        bad.dev_start_ns = bad.host_return_ns + window;
        bad.dev_end_ns   = bad.dev_start_ns + window / 2;
        bad.resolved_ns  = bad.host_return_ns + window / 4;  // before dev_end
        f.publish(&bad);
    }

    f.gpuFlush();
    const int64_t packets = f.shutdown();
    EXPECT_GT(packets, 0) << "shutdown wrote no packets";

    f.disarm();
    f.capDisarm();
    f.shutdownReset();
    ::unsetenv("CAJETA_PROFILER");
    ::unsetenv("CAJETA_PROFILER_OUT");

    ::testing::Test::RecordProperty("fixture", out);
}
