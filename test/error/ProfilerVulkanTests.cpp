// cajeta-profiler Unit 13 — the Vulkan backend's pure half (plan 13.1.b/f/h,
// spec §5.5.2, §5.5.6, §5.5.7, §11.4).
//
// Everything here runs with no Vulkan library and no device, because each of
// these behaviours is arithmetic or policy, and the failures they guard are
// exactly the ones a device test cannot produce on demand: a queue family
// whose timestamps are silently meaningless, a timestamp wrap at an odd bit
// width, a timestamp register reset by a low-power transition. The Vulkan API
// mechanics (query pools, availability, host reset) are covered by the
// device-gated suite.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include <cstdint>
#include <cstring>
#include <vector>
using cajeta_test::CajetaJit;

namespace {

// VkQueueFlagBits values, spelled as the raw bits the picker takes — the pure
// layer deliberately has no Vulkan types (it must build on machines with no
// Vulkan SDK header at all).
constexpr uint32_t Q_GRAPHICS = 0x1;
constexpr uint32_t Q_COMPUTE  = 0x2;
constexpr uint32_t Q_TRANSFER = 0x4;
constexpr uint32_t Q_SPARSE   = 0x8;
constexpr uint32_t Q_VDECODE  = 0x20;
constexpr uint32_t Q_VENCODE  = 0x40;

struct Vk {
    std::unique_ptr<CajetaJit> jit;
    int32_t  (*pick)(const uint32_t*, const uint32_t*, int32_t, int32_t*) = nullptr;
    uint64_t (*delta)(uint64_t, uint64_t, uint32_t) = nullptr;
    int32_t  (*noteSpan)(uint64_t, uint64_t) = nullptr;
    void     (*noteReset)(void) = nullptr;
    int32_t  (*configure)(uint32_t, double, int32_t) = nullptr;
    int32_t  (*timingOk)(void) = nullptr;
    double   (*clockPeriod)(int32_t) = nullptr;
    int32_t  (*clockReset)(int32_t) = nullptr;
    const char* (*backendName)(int32_t) = nullptr;
    void     (*publish)(const CajetaGpuEvent*) = nullptr;
    int32_t  (*sinkRegister)(CajetaGpuSinkFn, void*, int32_t) = nullptr;
    int32_t  (*sinkUnregister)(int32_t) = nullptr;
    int32_t  (*gpuFlush)(void) = nullptr;
    int32_t  (*noteWait)(int64_t, int64_t, int64_t) = nullptr;
    int32_t  (*resolveTier)(int64_t, int64_t, int64_t, int32_t) = nullptr;
};

Vk& vk() {
    static Vk v = [] {
        Vk x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class V {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.V");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.pick = reinterpret_cast<decltype(x.pick)>(sym("__cajeta_xpu_vk_pick_queue_family"));
        x.delta = reinterpret_cast<decltype(x.delta)>(sym("__cajeta_prof_vk_delta_ticks"));
        x.noteSpan = reinterpret_cast<decltype(x.noteSpan)>(sym("__cajeta_prof_vk_note_span_ticks"));
        x.noteReset = reinterpret_cast<decltype(x.noteReset)>(sym("__cajeta_prof_vk_span_tracking_reset"));
        x.configure = reinterpret_cast<decltype(x.configure)>(sym("__cajeta_prof_vk_configure"));
        x.timingOk = reinterpret_cast<decltype(x.timingOk)>(sym("__cajeta_prof_vk_timing_ok"));
        x.clockPeriod = reinterpret_cast<decltype(x.clockPeriod)>(sym("__cajeta_prof_clock_period"));
        x.clockReset = reinterpret_cast<decltype(x.clockReset)>(sym("__cajeta_prof_clock_reset"));
        x.backendName = reinterpret_cast<decltype(x.backendName)>(sym("__cajeta_prof_gpu_backend_name"));
        x.publish = reinterpret_cast<decltype(x.publish)>(sym("__cajeta_prof_gpu_publish"));
        x.sinkRegister = reinterpret_cast<decltype(x.sinkRegister)>(sym("__cajeta_prof_gpu_sink_register"));
        x.sinkUnregister = reinterpret_cast<decltype(x.sinkUnregister)>(sym("__cajeta_prof_gpu_sink_unregister"));
        x.gpuFlush = reinterpret_cast<decltype(x.gpuFlush)>(sym("__cajeta_prof_gpu_flush"));
        x.noteWait = reinterpret_cast<decltype(x.noteWait)>(sym("__cajeta_prof_vk_note_wait"));
        x.resolveTier = reinterpret_cast<decltype(x.resolveTier)>(sym("__cajeta_prof_gpu_resolve_dispatch_tier"));
        return x;
    }();
    return v;
}

std::vector<CajetaGpuEvent>& caught() {
    static std::vector<CajetaGpuEvent> v;
    return v;
}
int32_t catchSink(const CajetaGpuEvent* recs, int32_t n, void*) {
    for (int32_t i = 0; i < n; ++i) caught().push_back(recs[i]);
    return 0;
}

} // namespace

TEST(ProfilerVulkan, entryPointsResolve) {
    auto& v = vk();
    ASSERT_NE(v.pick, nullptr)      << "__cajeta_xpu_vk_pick_queue_family unresolved";
    ASSERT_NE(v.delta, nullptr)     << "__cajeta_prof_vk_delta_ticks unresolved";
    ASSERT_NE(v.noteSpan, nullptr)  << "__cajeta_prof_vk_note_span_ticks unresolved";
    ASSERT_NE(v.configure, nullptr) << "__cajeta_prof_vk_configure unresolved";
    ASSERT_NE(v.noteWait, nullptr)  << "__cajeta_prof_vk_note_wait unresolved";
    ASSERT_NE(v.resolveTier, nullptr)
        << "__cajeta_prof_gpu_resolve_dispatch_tier unresolved";
}

// --- 13.1.b / §5.5.2: the queue family whose timestamps mean something ------

// The measured shape of the reference device (RADV STRIX_HALO, 2026-08-24):
// five families, timestamps valid only on the two compute-capable ones. Three
// of five report zero valid bits, where a timestamp write is legal, returns a
// value, and means nothing.
TEST(ProfilerVulkan, pickPrefersAComputeFamilyWithValidTimestamps) {
    auto& v = vk();
    ASSERT_NE(v.pick, nullptr);
    const uint32_t flags[5] = { Q_GRAPHICS | Q_COMPUTE | Q_TRANSFER | Q_SPARSE,
                                Q_COMPUTE | Q_TRANSFER | Q_SPARSE,
                                Q_VDECODE, Q_VENCODE, Q_SPARSE };
    const uint32_t bits[5]  = { 64, 64, 0, 0, 0 };
    int32_t timingOk = -1;
    EXPECT_EQ(v.pick(flags, bits, 5, &timingOk), 0);
    EXPECT_EQ(timingOk, 1);

    // A device whose FIRST compute family has zero valid bits: the old
    // first-hit selection dispatched there and every timestamp was silently
    // meaningless. The capable family must win.
    const uint32_t flags2[2] = { Q_COMPUTE, Q_COMPUTE | Q_TRANSFER };
    const uint32_t bits2[2]  = { 0, 48 };
    timingOk = -1;
    EXPECT_EQ(v.pick(flags2, bits2, 2, &timingOk), 1);
    EXPECT_EQ(timingOk, 1);
}

TEST(ProfilerVulkan, pickRefusesTimingWhenNoComputeFamilyCanTime) {
    auto& v = vk();
    ASSERT_NE(v.pick, nullptr);
    // Dispatch still works — a compute family exists — but timing is REFUSED
    // rather than delivered from a family whose timestamps mean nothing
    // (§5.5.2). Refusal here is what keeps the host tier honest downstream.
    const uint32_t flags[2] = { Q_COMPUTE, Q_COMPUTE };
    const uint32_t bits[2]  = { 0, 0 };
    int32_t timingOk = -1;
    EXPECT_EQ(v.pick(flags, bits, 2, &timingOk), 0);
    EXPECT_EQ(timingOk, 0);
}

TEST(ProfilerVulkan, pickFindsNoFamilyWithoutCompute) {
    auto& v = vk();
    ASSERT_NE(v.pick, nullptr);
    const uint32_t flags[2] = { Q_GRAPHICS, Q_VDECODE };
    const uint32_t bits[2]  = { 64, 64 };
    int32_t timingOk = -1;
    EXPECT_EQ(v.pick(flags, bits, 2, &timingOk), -1);
    EXPECT_EQ(timingOk, 0);
}

// --- 13.1.f / §5.5.6: wrap arithmetic at the device's valid-bit width -------

TEST(ProfilerVulkan, deltaTicksHandlesWrapAt36Bits) {
    auto& v = vk();
    ASSERT_NE(v.delta, nullptr);
    const uint64_t top36 = (1ULL << 36);
    EXPECT_EQ(v.delta(1000, 5000, 36), 4000u);
    // End wrapped past the 36-bit boundary: the true delta is small.
    EXPECT_EQ(v.delta(top36 - 100, 50, 36), 150u);
    // High garbage bits above the valid width are masked, not trusted: some
    // drivers leave stale bits above timestampValidBits.
    EXPECT_EQ(v.delta((0xDEADULL << 40) | 1000, (0xBEEFULL << 40) | 5000, 36),
              4000u);
}

TEST(ProfilerVulkan, deltaTicksAt64BitsHasNoUndefinedBehavior) {
    auto& v = vk();
    ASSERT_NE(v.delta, nullptr);
    // 1ULL << 64 is undefined behavior; the 64-bit path must not compute the
    // mask that way. Wrap at 2^64 is plain unsigned subtraction.
    EXPECT_EQ(v.delta(1000, 5000, 64), 4000u);
    EXPECT_EQ(v.delta(~0ULL - 99, 100, 64), 200u);
}

TEST(ProfilerVulkan, deltaTicksWithZeroValidBitsIsZero) {
    auto& v = vk();
    ASSERT_NE(v.delta, nullptr);
    // A zero-valid-bits family never reaches the delta in production (the
    // picker refuses timing) — but arithmetic on meaningless ticks must not
    // manufacture a duration if it is ever asked.
    EXPECT_EQ(v.delta(1000, 5000, 0), 0u);
}

// --- 13.1.h / §5.5.7: the low-power timestamp reset -------------------------

TEST(ProfilerVulkan, aTimestampRegisterResetIsFlaggedNotRenderedAsFact) {
    auto& v = vk();
    ASSERT_NE(v.noteSpan, nullptr);
    ASSERT_NE(v.noteReset, nullptr);
    v.noteReset();

    // Two well-ordered spans: clean.
    EXPECT_EQ(v.noteSpan(10000, 20000), CAJETA_SPAN_OK);
    EXPECT_EQ(v.noteSpan(21000, 30000), CAJETA_SPAN_OK);
    // The register reset on a low-power transition: the next span STARTS
    // before the previous one ended — on an AMD APU it starts near zero. The
    // span itself is internally consistent; only history reveals it.
    EXPECT_TRUE(v.noteSpan(500, 900) & CAJETA_SPAN_NONMONOTONIC);
    // After the reset is flagged, tracking re-bases: the device's counter
    // genuinely restarted, and flagging every span after it forever would
    // bury the one real event under noise.
    EXPECT_EQ(v.noteSpan(1000, 2000), CAJETA_SPAN_OK);
}

// --- §11.4: the advertised period is validated, not trusted -----------------

TEST(ProfilerVulkan, configureAcceptsTheMeasuredPeriodAndRejectsNonsense) {
    auto& v = vk();
    ASSERT_NE(v.configure, nullptr);
    ASSERT_NE(v.timingOk, nullptr);

    // The reference device's advertised period (10.019 ns/tick, RADV).
    EXPECT_EQ(v.configure(64, 10.019, 1), 1);
    EXPECT_EQ(v.timingOk(), 1);
    EXPECT_NEAR(v.clockPeriod(CAJ_GPU_BACKEND_VULKAN), 10.019, 1e-9);

    // Zero is what a driver reports when it does not know (§11.4). Timing
    // degrades rather than dividing by it downstream.
    EXPECT_EQ(v.configure(64, 0.0, 1), 0);
    EXPECT_EQ(v.timingOk(), 0);

    // Zero valid bits refuses timing regardless of the period.
    EXPECT_EQ(v.configure(0, 10.019, 1), 0);
    EXPECT_EQ(v.timingOk(), 0);

    // Leave the module configured for any later test in this process.
    EXPECT_EQ(v.configure(64, 10.019, 1), 1);
}

// --- §5.5.8 / §14.13: the host-blocked stall is an explicit span ------------

TEST(ProfilerVulkan, aHostBlockedWaitIsPublishedAsAnExplicitSpan) {
    auto& v = vk();
    ASSERT_NE(v.noteWait, nullptr);
    ASSERT_NE(v.sinkRegister, nullptr);

    caught().clear();
    const int32_t sink = v.sinkRegister(catchSink, nullptr, CAJETA_GPU_SINK_PER_RECORD);
    ASSERT_GE(sink, 0);
    EXPECT_EQ(v.noteWait(0x51, 1000000, 4000000), 1);
    v.gpuFlush();
    v.sinkUnregister(sink);

    ASSERT_EQ(caught().size(), 1u) << "the wait produced no record";
    const CajetaGpuEvent& e = caught()[0];
    ASSERT_NE(e.kernel_name, nullptr);
    EXPECT_STREQ(e.kernel_name, "host blocked on GPU");
    // A host-measured wall interval is host-tier truth, nothing more.
    EXPECT_EQ(e.tier, CAJETA_PROF_TIER_HOST);
    EXPECT_EQ(e.dev_start_ns, 1000000);
    EXPECT_EQ(e.dev_end_ns, 4000000);
    EXPECT_EQ(e.queue, 0x51);
    EXPECT_EQ(e.backend, CAJ_GPU_BACKEND_VULKAN);
    // It spans the blocked interval and is exempt from the §9.1.a bracket the
    // way every host-tier record is.
    EXPECT_GT(e.launch_id, 0) << "the wait span needs its own id to be a slice";
}

TEST(ProfilerVulkan, anUnarmedWaitPublishesNothing) {
    auto& v = vk();
    ASSERT_NE(v.noteWait, nullptr);
    // No sinks: the note is a no-op, not a queued surprise for the next test.
    EXPECT_EQ(v.noteWait(0x51, 1000000, 4000000), 0);
}

// --- the seam speaks EVENT tier for a resolved Vulkan bracket ---------------

TEST(ProfilerVulkan, backendNameReflectsTimingConfiguration) {
    auto& v = vk();
    ASSERT_NE(v.backendName, nullptr);
    // With the module configured (the test above leaves it so), backend 2
    // resolves to the vulkan lane; without Vulkan timing it is the host lane.
    EXPECT_STREQ(v.backendName(CAJ_GPU_BACKEND_VULKAN), "vulkan");
    ASSERT_EQ(v.configure(0, 10.019, 1), 0);   // refuse timing
    EXPECT_STREQ(v.backendName(CAJ_GPU_BACKEND_VULKAN), "cpu");
    ASSERT_EQ(v.configure(64, 10.019, 1), 1);  // restore
}
