//
// Deferred launches and replay (xpu-kernel-adaptor Unit 12, the launch floor).
//
// A launch costs the host a driver call, and on the measured box that call is
// 6 to 13 microseconds whatever the kernel does. A decode token is some
// hundreds of launches, so the floor is milliseconds a token, and it is the
// same sequence of kernels every token. `KernelStream.setDeferred(true)` makes
// the stream QUEUE its launches and submit them together at the next point
// their results can be observed: a sync, a host transfer, an event, a free, or
// `flush()`. A queue whose kernel sequence the stream has submitted before is
// replayed as one recorded submission with only the changed arguments patched
// (a CUDA graph on nvptx). Where the backend has no such thing the queue is
// submitted launch by launch, and on a synchronous backend nothing is queued
// at all. The observable results are the same in every case, which is what
// these tests pin, together with the counters that say the replay happened.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options withBackend(cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    return o;
}

const char* kDeferSource =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Defer {\n"
    "    @Kernel\n"
    "    public static void deferAdd(KernelBuffer<float32> y, uint32 n, float32 v) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = y[i] + v; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void deferScale(KernelBuffer<float32> y, uint32 n, float32 f) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = y[i] * f; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void deferCopy(KernelBuffer<float32> dst, KernelBuffer<float32> src, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { dst[i] = src[i]; }\n"
    "    }\n"
    "    /** One round: twelve launches whose result depends on their ORDER and on `seed`. */\n"
    "    static void round(KernelStream s, KernelBuffer<float32> y, KernelBuffer<float32> z,\n"
    "            uint32 n, float32 seed) {\n"
    "        uint32 g = (n + 63) / 64;\n"
    "        int32 k = 0;\n"
    "        while (k < 4) {\n"
    "            deferAdd.launch(s, grid: [g], block: [64])(y, n, seed + (float32) k);\n"
    "            deferScale.launch(s, grid: [g], block: [64])(y, n, 0.5f);\n"
    "            deferCopy.launch(s, grid: [g], block: [64])(z, y, n);\n"
    "            k = k + 1;\n"
    "        }\n"
    "        return;\n"
    "    }\n"
    "    /** The same arithmetic on the host. */\n"
    "    static float32 expect(float32 start, float32 seed) {\n"
    "        float32 v = start;\n"
    "        int32 k = 0;\n"
    "        while (k < 4) { v = (v + seed + (float32) k) * 0.5f; k = k + 1; }\n"
    "        return v;\n"
    "    }\n"
    "    /**\n"
    "     * `rounds` rounds on a stream, deferred or not, a readback after each.\n"
    "     * Returns the number of rounds whose every element matched the host.\n"
    "     * The width changes on the last round, so the grid of a recorded\n"
    "     * sequence changes too.\n"
    "     */\n"
    "    public static int32 run(int32 deferred, int32 rounds) {\n"
    "        uint32 cap = 512;\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, cap);\n"
    "        KernelBuffer<float32> z = heap KernelBuffer<float32>(0, cap);\n"
    "        y.allocate();\n"
    "        z.allocate();\n"
    "        float32[] host #= heap float32[(int32) cap];\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) cap) { host[i] = 1.0f; i = i + 1; }\n"
    "        y.upload(host);\n"
    "        KernelStream s #= KernelStream.create();\n"
    "        s.setDeferred(deferred != 0);\n"
    "        int32 good = 0;\n"
    "        float32 want = 1.0f;\n"
    "        int32 r = 0;\n"
    "        while (r < rounds) {\n"
    "            uint32 n = 256;\n"
    "            if (r == rounds - 1) { n = 512; }\n"
    "            float32 seed = 1.0f + (float32) r;\n"
    "            Defer.round(s, y, z, n, seed);\n"
    "            z.download(host);\n"
    "            float32 w = Defer.expect(want, seed);\n"
    "            boolean ok = true;\n"
    "            i = 0;\n"
    "            while (i < 256) {\n"
    "                if (host[i] != w) { ok = false; }\n"
    "                i = i + 1;\n"
    "            }\n"
    "            if (ok) { good = good + 1; }\n"
    "            want = w;\n"
    "            r = r + 1;\n"
    "        }\n"
    "        s.setDeferred(false);\n"
    "        s.sync();\n"
    "        s.destroy();\n"
    "        y.free();\n"
    "        z.free();\n"
    "        return good;\n"
    "    }\n"
    "    /** A launch queued before `setDeferred(false)` has run by the time it returns. */\n"
    "    public static int32 turningOffFlushes() {\n"
    "        uint32 n = 64;\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        y.allocate();\n"
    "        float32[] host #= heap float32[64];\n"
    "        int32 i = 0;\n"
    "        while (i < 64) { host[i] = 2.0f; i = i + 1; }\n"
    "        y.upload(host);\n"
    "        KernelStream s #= KernelStream.create();\n"
    "        s.setDeferred(true);\n"
    "        deferAdd.launch(s, grid: [1], block: [64])(y, n, 3.0f);\n"
    "        deferScale.launch(s, grid: [1], block: [64])(y, n, 2.0f);\n"
    "        int64 pendingBefore = s.pendingLaunches();\n"
    "        s.setDeferred(false);\n"
    "        int64 pendingAfter = s.pendingLaunches();\n"
    "        s.sync();\n"
    "        y.download(host);\n"
    "        s.destroy();\n"
    "        y.free();\n"
    "        int32 bits = 0;\n"
    "        if (host[0] == 10.0f && host[63] == 10.0f) { bits = bits | 1; }\n"
    "        if (pendingAfter == 0L) { bits = bits | 2; }\n"
    "        if (pendingBefore == 2L) { bits = bits | 4; }\n"
    "        return bits;\n"
    "    }\n"
    "    public static int64 stat(int32 which) { return KernelStream.deferStat(which); }\n"
    "    public static int64 launches() {\n"
    "        return Device.kernelLaunchCount(\"deferAdd\") + Device.kernelLaunchCount(\"deferScale\")\n"
    "            + Device.kernelLaunchCount(\"deferCopy\");\n"
    "    }\n"
    "}\n";

// KernelStream.DEFER_* in the stdlib.
enum { kQueued = 0, kReplayed = 1, kDirect = 2, kGraphsBuilt = 3,
       kGraphLaunches = 4, kPatches = 5, kFlushes = 6 };

struct Defer {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*run)(int32_t, int32_t) = nullptr;
    int32_t (*turningOff)() = nullptr;
    int64_t (*stat)(int32_t) = nullptr;
    int64_t (*launches)() = nullptr;
};

bool load(cajeta::xpu::Backend backend, Defer& d) {
    d.jit = CajetaJit::compile(kDeferSource, "test.Defer", withBackend(backend));
    if (!d.jit) return false;
    d.run        = d.jit->lookup<int32_t (*)(int32_t, int32_t)>("run");
    d.turningOff = d.jit->lookup<int32_t (*)()>("turningOffFlushes");
    d.stat       = d.jit->lookup<int64_t (*)(int32_t)>("stat");
    d.launches   = d.jit->lookup<int64_t (*)()>("launches");
    return d.run && d.turningOff && d.stat && d.launches;
}

} // namespace

// The direct arm is the control: the fixture itself is right before any
// statement about deferral means something.
TEST(XpuDeferredLaunch, nvptxDirectRoundsMatchTheHost) {
    CAJETA_SKIP_IF_NO_CUDA();
    Defer d;
    ASSERT_TRUE(load(cajeta::xpu::Backend::Nvptx, d));
    const int64_t queued = d.stat(kQueued);
    EXPECT_EQ(d.run(0, 6), 6);
    EXPECT_EQ(d.stat(kQueued), queued) << "a stream that was never deferred queued a launch";
}

// Six rounds of one twelve-launch sequence: the first is submitted launch by
// launch (nothing is known about it), the second records it, and from the
// third on it is replayed. Every round's readback must match the host, which
// covers the patched scalars (the seed changes every round) and the patched
// grid (the last round is twice as wide).
TEST(XpuDeferredLaunch, nvptxDeferredRoundsMatchTheHostAndAreReplayed) {
    CAJETA_SKIP_IF_NO_CUDA();
    Defer d;
    ASSERT_TRUE(load(cajeta::xpu::Backend::Nvptx, d));
    const int64_t before = d.launches();
    int64_t s0[7];
    for (int k = 0; k < 7; ++k) s0[k] = d.stat(k);
    auto delta = [&](int k) { return d.stat(k) - s0[k]; };
    EXPECT_EQ(d.run(1, 6), 6) << "a deferred round read back a different result than the host computed";
    std::printf(" RESULT defer queued=%lld replayed=%lld direct=%lld graphs=%lld "
                "graphLaunches=%lld patches=%lld flushes=%lld\n",
                (long long) delta(kQueued), (long long) delta(kReplayed),
                (long long) delta(kDirect), (long long) delta(kGraphsBuilt),
                (long long) delta(kGraphLaunches), (long long) delta(kPatches),
                (long long) delta(kFlushes));
    EXPECT_EQ(delta(kQueued), 72) << "six rounds of twelve launches were not all queued";
    EXPECT_EQ(d.launches() - before, 72) << "the census lost or double-counted a deferred launch";
    EXPECT_EQ(delta(kQueued), delta(kReplayed) + delta(kDirect))
        << "a queued launch was neither replayed nor submitted directly";
    EXPECT_EQ(delta(kGraphsBuilt), 1) << "one sequence should be recorded once";
    EXPECT_GE(delta(kReplayed), 48) << "rounds three to six were not replayed";
    EXPECT_GE(delta(kPatches), 4) << "the changing seed was never patched into the recording";
}

TEST(XpuDeferredLaunch, nvptxTurningDeferralOffSubmitsWhatWasQueued) {
    CAJETA_SKIP_IF_NO_CUDA();
    Defer d;
    ASSERT_TRUE(load(cajeta::xpu::Backend::Nvptx, d));
    const int32_t bits = d.turningOff();
    EXPECT_TRUE(bits & 4) << "two launches on a deferred stream were not pending";
    EXPECT_TRUE(bits & 2) << "launches were still pending after setDeferred(false)";
    EXPECT_TRUE(bits & 1) << "the queued launches did not run, or ran out of order";
}

// A synchronous backend has nothing to defer: the switch is accepted, nothing
// is queued, and the results are the same.
TEST(XpuDeferredLaunch, cpuAcceptsTheSwitchAndQueuesNothing) {
    Defer d;
    ASSERT_TRUE(load(cajeta::xpu::Backend::Cpu, d));
    const int64_t queued = d.stat(kQueued);
    EXPECT_EQ(d.run(1, 4), 4);
    EXPECT_EQ(d.stat(kQueued), queued) << "the cpu backend queued a launch";
    const int32_t bits = d.turningOff();
    EXPECT_TRUE(bits & 1) << "the launches did not run";
    EXPECT_TRUE(bits & 2);
    EXPECT_FALSE(bits & 4) << "the cpu backend reported pending launches";
}
