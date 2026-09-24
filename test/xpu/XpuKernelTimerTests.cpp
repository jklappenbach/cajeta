//
// cajeta.xpu.KernelTimer — a per-kernel device timer a harness can stand behind
// (plan xpu-kernel-adaptor Unit 5, spec §7).
//
// The problem it answers: a bench harness timing a cooperative kernel on the
// RTX 4090 implied 252 TFLOPS against the part's ~165 dense f16 peak. A number
// like that is not a slow harness; it is a timer measuring something other
// than execution. Until a timer is CALIBRATED (5.1.1), REFUSES what it cannot
// measure (5.1.2) and is gated against the device's peak (5.1.3), no speed
// figure in the plan can be believed.
//
// The timer brackets a stream with two Events and reads the DEVICE's own
// elapsed time between them (cuEventElapsedTime / hipEventElapsedTime), so on
// NVIDIA it needs the driver and nothing else — no CUPTI, no toolkit. It
// labels its tier: DEVICE where the device measured, HOST where the backend is
// synchronous and the host clock brackets execution exactly (CPU), and
// UNAVAILABLE where neither holds (Vulkan today), in which case elapsedNanos
// answers -1 rather than a number.
//
// The calibration kernel spins on KernelThread.clock() for a stated number of
// SM cycles, so its duration is known from the device's own clock rate — the
// timer is checked against the hardware, not against another host timer.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options withBackend(cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    return o;
}

// One source, every backend: a spin kernel of a stated cycle count, timed by
// the device timer and, independently, by the host clock around a sync.
const char* kSpinSource =
    "package test;\n"
    "import cajeta.time.Clock;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.KernelTimer;\n"
    "public class Spin {\n"
    "    @Kernel\n"
    "    public static void spin(KernelBuffer<uint64> out, uint64 cycles, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            uint64 t0 = KernelThread.clock();\n"
    "            uint64 t = t0;\n"
    "            while (t - t0 < cycles) { t = KernelThread.clock(); }\n"
    "            out[i] = t - t0;\n"
    "        }\n"
    "    }\n"
    "    static #KernelBuffer<uint64> scratch(uint32 n) {\n"
    "        KernelBuffer<uint64> out = heap KernelBuffer<uint64>(0, n);\n"
    "        out.allocate();\n"
    "        return #out;\n"
    "    }\n"
    "    /** Device-timed nanoseconds for one spin of `cycles`; -1 = refused. */\n"
    "    public static int64 timed(uint64 cycles) {\n"
    "        uint32 n = 32;\n"
    "        KernelBuffer<uint64> out #= Spin.scratch(n);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        KernelTimer t #= KernelTimer.create();\n"
    "        t.begin(s);\n"
    "        spin.launch(s, grid: [1], block: [32])(out, cycles, n);\n"
    "        t.end(s);\n"
    "        int64 ns = t.elapsedNanos();\n"
    "        s.sync();\n"
    "        t.destroy();\n"
    "        out.free();\n"
    "        return ns;\n"
    "    }\n"
    "    /** The same spin timed by the host clock around launch + sync. */\n"
    "    public static int64 hostTimed(uint64 cycles) {\n"
    "        uint32 n = 32;\n"
    "        KernelBuffer<uint64> out #= Spin.scratch(n);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        s.sync();\n"
    "        int64 t0 = Clock.nanoTime();\n"
    "        spin.launch(s, grid: [1], block: [32])(out, cycles, n);\n"
    "        s.sync();\n"
    "        int64 ns = Clock.nanoTime() - t0;\n"
    "        out.free();\n"
    "        return ns;\n"
    "    }\n"
    "    static int64 lastHostNs;\n"
    "    /** ONE launch, both clocks: the host window encloses the device bracket. */\n"
    "    public static int64 pairTimed(uint64 cycles) {\n"
    "        uint32 n = 32;\n"
    "        KernelBuffer<uint64> out #= Spin.scratch(n);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        KernelTimer t #= KernelTimer.create();\n"
    "        s.sync();\n"
    "        int64 h0 = Clock.nanoTime();\n"
    "        t.begin(s);\n"
    "        spin.launch(s, grid: [1], block: [32])(out, cycles, n);\n"
    "        t.end(s);\n"
    "        s.sync();\n"
    "        Spin.lastHostNs = Clock.nanoTime() - h0;\n"
    "        int64 ns = t.elapsedNanos();\n"
    "        t.destroy();\n"
    "        out.free();\n"
    "        return ns;\n"
    "    }\n"
    "    public static int64 lastHost() { return Spin.lastHostNs; }\n"
    "    /** A timer whose end was never recorded must refuse. */\n"
    "    public static int64 withoutEnd() {\n"
    "        uint32 n = 32;\n"
    "        KernelBuffer<uint64> out #= Spin.scratch(n);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        KernelTimer t #= KernelTimer.create();\n"
    "        t.begin(s);\n"
    "        spin.launch(s, grid: [1], block: [32])(out, (uint64) 1000, n);\n"
    "        s.sync();\n"
    "        int64 ns = t.elapsedNanos();\n"
    "        t.destroy();\n"
    "        out.free();\n"
    "        return ns;\n"
    "    }\n"
    "    /** A fresh timer, nothing recorded, must refuse. */\n"
    "    public static int64 fresh() {\n"
    "        KernelTimer t #= KernelTimer.create();\n"
    "        int64 ns = t.elapsedNanos();\n"
    "        t.destroy();\n"
    "        return ns;\n"
    "    }\n"
    "    public static int32 tier() { return KernelTimer.tier(); }\n"
    "    public static float64 clockScale() { return KernelTimer.clockScale(); }\n"
    "    public static int64 clockKHz() { return (int64) Device.clockKHz(); }\n"
    "    public static int64 memoryClockKHz() { return (int64) Device.memoryClockKHz(); }\n"
    "    public static int64 memoryBusWidthBits() { return (int64) Device.memoryBusWidthBits(); }\n"
    "    public static float64 peakBandwidthGBps() { return Device.peakBandwidthGBps(); }\n"
    "    /** Every arm of the peak gate, as a bit mask, so one JIT answers all. */\n"
    "    public static int32 gate() {\n"
    "        int32 bits = 0;\n"
    "        // 1 GiB in 1 ns against a 1000 GB/s ceiling: impossible.\n"
    "        if (!KernelTimer.withinPeak(1L, 1073741824L, 1000.0, 0.0, 0.0)) { bits = bits | 1; }\n"
    "        // 1 GiB in 10 ms against the same ceiling: 107 GB/s, fine.\n"
    "        if (KernelTimer.withinPeak(10000000L, 1073741824L, 1000.0, 0.0, 0.0)) { bits = bits | 2; }\n"
    "        // 1e12 ops in 1 us = 1e18 ops/s against a 1e14 peak: impossible.\n"
    "        if (!KernelTimer.withinPeak(1000L, 0L, 0.0, 1.0e12, 1.0e14)) { bits = bits | 4; }\n"
    "        // 1e9 ops in 1 ms = 1e12 ops/s against 1e14: fine.\n"
    "        if (KernelTimer.withinPeak(1000000L, 0L, 0.0, 1.0e9, 1.0e14)) { bits = bits | 8; }\n"
    "        // Work with NO ceiling declared is not accepted: a gate that skips is no gate.\n"
    "        if (!KernelTimer.withinPeak(1000000L, 1048576L, 0.0, 0.0, 0.0)) { bits = bits | 16; }\n"
    "        if (!KernelTimer.withinPeak(1000000L, 0L, 0.0, 1.0e9, 0.0)) { bits = bits | 32; }\n"
    "        // A non-positive duration is not a measurement.\n"
    "        if (!KernelTimer.withinPeak(0L, 1048576L, 1000.0, 0.0, 0.0)) { bits = bits | 64; }\n"
    "        // The implied rates read back in the units the ceilings use.\n"
    "        float64 gbps = KernelTimer.impliedGBps(1000000L, 100000000L);\n"
    "        if (gbps > 99.9 && gbps < 100.1) { bits = bits | 128; }\n"
    "        float64 ops = KernelTimer.impliedOpsPerSecond(1000000L, 2.0e9);\n"
    "        if (ops > 1.99e12 && ops < 2.01e12) { bits = bits | 256; }\n"
    "        return bits;\n"
    "    }\n"
    "}\n";

// The GPU's clock is dynamic: the same spin took 20 ms at boost and 67 ms in a
// low power state in one run of this suite. The best of several attempts is
// the one at the highest clock reached, which is the state the derived
// expectation describes.
template <class F>
int64_t bestOf(int n, F&& f) {
    int64_t best = -1;
    for (int i = 0; i < n; ++i) {
        const int64_t v = f();
        if (v > 0 && (best < 0 || v < best)) best = v;
    }
    return best;
}

const int32_t kTierUnavailable = 0;
const int32_t kTierHost        = 1;
const int32_t kTierDevice      = 2;

} // namespace

// ── 5.1.2 the refusal, on the backend every CI leg has ─────────────────────

TEST(XpuKernelTimer, refusesBeforeTheEndIsRecorded) {
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Cpu));
    ASSERT_NE(jit, nullptr);
    auto withoutEnd = jit->lookup<int64_t (*)()>("withoutEnd");
    auto fresh      = jit->lookup<int64_t (*)()>("fresh");
    ASSERT_NE(withoutEnd, nullptr);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh(), -1)
        << "a timer with nothing recorded returned a number it cannot stand behind";
    EXPECT_EQ(withoutEnd(), -1)
        << "a timer whose end was never recorded returned a number";
}

// ── the CPU backend is synchronous: host tier, said so, and it measures ────

TEST(XpuKernelTimer, cpuTierIsHostAndTheSpinIsTimed) {
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Cpu));
    ASSERT_NE(jit, nullptr);
    auto tier  = jit->lookup<int32_t (*)()>("tier");
    auto timed = jit->lookup<int64_t (*)(uint64_t)>("timed");
    ASSERT_NE(tier, nullptr);
    ASSERT_NE(timed, nullptr);
    // A CPU launch has completed when launch() returns, so a host clock
    // bracketing the stream IS the execution time — but it is the host's
    // clock, and the tier must say so rather than borrow the device label.
    EXPECT_EQ(tier(), kTierHost);
    const int64_t ns = timed(2000000ULL);   // ~1 ms of rdtsc ticks at 2 GHz
    std::printf(" RESULT u5_cpu_spin_ns=%lld\n", (long long) ns);
    EXPECT_GT(ns, 0) << "the CPU timer refused or returned a non-advancing span";
}

// ── 5.1.3 the peak gate fires, and does not fire, and never skips ──────────

TEST(XpuKernelTimer, peakGateRejectsWhatNoDeviceCouldDo) {
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Cpu));
    ASSERT_NE(jit, nullptr);
    auto gate = jit->lookup<int32_t (*)()>("gate");
    ASSERT_NE(gate, nullptr);
    const int32_t bits = gate();
    EXPECT_TRUE(bits & 1)   << "1 GiB in 1 ns was accepted against a 1000 GB/s ceiling";
    EXPECT_TRUE(bits & 2)   << "107 GB/s was rejected against a 1000 GB/s ceiling";
    EXPECT_TRUE(bits & 4)   << "1e18 ops/s was accepted against a 1e14 peak";
    EXPECT_TRUE(bits & 8)   << "1e12 ops/s was rejected against a 1e14 peak";
    EXPECT_TRUE(bits & 16)  << "bytes with no bandwidth ceiling were accepted — the gate skipped";
    EXPECT_TRUE(bits & 32)  << "ops with no compute ceiling were accepted — the gate skipped";
    EXPECT_TRUE(bits & 64)  << "a zero-duration measurement was accepted";
    EXPECT_TRUE(bits & 128) << "impliedGBps is not in GB/s";
    EXPECT_TRUE(bits & 256) << "impliedOpsPerSecond is not in ops/s";
}

// ── Vulkan has no event timing wired: refuse, do not guess ─────────────────

TEST(XpuKernelTimer, vulkanRefusesRatherThanGuess) {
    if (!::cajeta::xpu::test::vulkanAvailable())
        GTEST_SKIP() << "no Vulkan device: " << ::cajeta::xpu::test::vulkanAbsenceReason();
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Spirv));
    ASSERT_NE(jit, nullptr);
    auto tier  = jit->lookup<int32_t (*)()>("tier");
    auto timed = jit->lookup<int64_t (*)(uint64_t)>("timed");
    ASSERT_NE(tier, nullptr);
    ASSERT_NE(timed, nullptr);
    const int64_t ns = timed(100000ULL);
    EXPECT_EQ(tier(), kTierUnavailable)
        << "Vulkan events are always-signaled sentinels; a tier above UNAVAILABLE "
           "claims a measurement the backend cannot make";
    EXPECT_EQ(ns, -1) << "the Vulkan timer returned " << ns << " with no device clock behind it";
}

// ── 5.1.1 calibration on NVIDIA: a spin of known cycles, timed by the device ─

TEST(XpuKernelTimer, nvptxReproducesASpinOfKnownCycles) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Nvptx));
    ASSERT_NE(jit, nullptr);
    auto tier     = jit->lookup<int32_t (*)()>("tier");
    auto timed    = jit->lookup<int64_t (*)(uint64_t)>("timed");
    auto clockKHz = jit->lookup<int64_t (*)()>("clockKHz");
    ASSERT_NE(tier, nullptr);
    ASSERT_NE(timed, nullptr);
    ASSERT_NE(clockKHz, nullptr);

    const int64_t khz = clockKHz();
    ASSERT_GT(khz, 0) << "the driver reported no core clock; the known duration cannot be derived";

    // clock64 counts SM cycles, so `cycles` at the reported (maximum) clock is
    // the SHORTEST the kernel can take. ~20 ms so that launch latency — the
    // event bracket includes it, ~0.4 ms on WSL2 — is inside the tolerance.
    const uint64_t cycles = (uint64_t) khz * 20ULL;          // 20 ms at max clock
    const double expectNs = (double) cycles / ((double) khz * 1e3) * 1e9;

    // Warm the module, then run ~200 ms of spin so GPU Boost has settled: the
    // clock ramps over tens of milliseconds, and a 40 ms kernel otherwise
    // sees a higher average clock than a 20 ms one and reads as sub-linear.
    timed(cycles / 20);
    for (int i = 0; i < 5; ++i) timed(cycles * 2);
    // Interleave the arms so a clock drift lands on both alike.
    int64_t one = -1, two = -1;
    for (int i = 0; i < 3; ++i) {
        const int64_t a = timed(cycles);
        const int64_t b = timed(cycles * 2);
        if (a > 0 && (one < 0 || a < one)) one = a;
        if (b > 0 && (two < 0 || b < two)) two = b;
    }
    std::printf(" RESULT u5_clock_khz=%lld\n", (long long) khz);
    std::printf(" RESULT u5_spin_expect_ns=%.0f\n", expectNs);
    std::printf(" RESULT u5_spin_one_ns=%lld\n", (long long) one);
    std::printf(" RESULT u5_spin_two_ns=%lld\n", (long long) two);

    EXPECT_EQ(tier(), kTierDevice) << "the NVIDIA timer did not reach the device tier";
    ASSERT_GT(one, 0) << "the timer refused a kernel that ran";
    ASSERT_GT(two, 0);

    // Faster than the reported clock allows is a unit or clock-domain error,
    // not a fast GPU — with one allowance: GPU Boost runs ABOVE attribute 13.
    // nvidia-smi on the RTX 4090 reports clocks.max.sm 3120 MHz against the
    // attribute's 2535 (23% headroom) and 2805 MHz sampled during this very
    // spin, and no CUDA attribute reports the 3120. So the floor is 25% under
    // the reported clock: a 2x unit error still fails it, and the 4.56% event
    // clock scale is pinned by the sleep test below, not here. Above 3x is a
    // stuck clock or a bracket that swallowed something else; both are the
    // timer's problem to expose.
    // (Before the event clock's scale was corrected this read 2794 MHz, which
    // was the 4.56% scale error, not the silicon.)
    const double impliedMHz = (double) cycles / ((double) one * 1e-9) / 1e6;
    std::printf(" RESULT u5_spin_implied_mhz=%.0f\n", impliedMHz);
    EXPECT_GE((double) one, expectNs * 0.75)
        << "the timer reports a spin of " << cycles << " cycles finishing faster than "
           "the device's maximum clock permits, even allowing 25% of boost headroom";
    EXPECT_LE((double) one, expectNs * 3.0)
        << "the timer reports three times the longest plausible duration";

    // Linearity: doubling the work doubles the time within 15%, which bounds
    // the constant the bracket adds (launch latency) relative to the kernel.
    // The tolerance is the dynamic clock's, not the timer's: 2.001 and 1.850
    // were measured in consecutive runs before the warm-up above was added.
    const double ratio = (double) two / (double) one;
    std::printf(" RESULT u5_spin_ratio=%.4f\n", ratio);
    EXPECT_NEAR(ratio, 2.0, 0.3)
        << "twice the cycles did not take twice the time; the bracket is measuring "
           "something other than the kernel";
}

// ── 5.3.1 two independent timers agree on one kernel ───────────────────────

TEST(XpuKernelTimer, nvptxAgreesWithTheHostClockOnALongKernel) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Nvptx));
    ASSERT_NE(jit, nullptr);
    auto timed     = jit->lookup<int64_t (*)(uint64_t)>("timed");
    auto pairTimed = jit->lookup<int64_t (*)(uint64_t)>("pairTimed");
    auto lastHost  = jit->lookup<int64_t (*)()>("lastHost");
    auto clockKHz  = jit->lookup<int64_t (*)()>("clockKHz");
    ASSERT_NE(timed, nullptr);
    ASSERT_NE(pairTimed, nullptr);
    ASSERT_NE(lastHost, nullptr);
    ASSERT_NE(clockKHz, nullptr);
    const int64_t khz = clockKHz();
    ASSERT_GT(khz, 0);
    const uint64_t cycles = (uint64_t) khz * 20ULL;             // ~20 ms

    timed(cycles / 20);
    for (int i = 0; i < 3; ++i) timed(cycles * 2);              // settle the clock
    // Both clocks on the SAME launch: two launches are not comparable when the
    // GPU's clock moves between them (separate best-of-3 arms disagreed by 5.6%
    // in the wrong direction before this was one launch). Keep the pair whose
    // device time is shortest.
    int64_t dev = -1, host = -1;
    for (int i = 0; i < 3; ++i) {
        const int64_t d = pairTimed(cycles);
        const int64_t h = lastHost();
        if (d > 0 && (dev < 0 || d < dev)) { dev = d; host = h; }
    }
    std::printf(" RESULT u5_agree_device_ns=%lld\n", (long long) dev);
    std::printf(" RESULT u5_agree_host_ns=%lld\n", (long long) host);
    ASSERT_GT(dev, 0);
    ASSERT_GT(host, 0);
    // The host window ENCLOSES the device bracket (it opens before begin and
    // closes after the sync), so it is the longer one, by launch latency and
    // the wake-up: a few percent of 20 ms. Shorter is a causality violation;
    // more than 10% longer is a disagreement nothing here explains.
    EXPECT_GE(host, dev)
        << "the host window that encloses the device bracket reads shorter than it";
    EXPECT_LE((double) host, (double) dev * 1.10)
        << "the two timers disagree by more than launch latency can explain";
}

// ── the peak the gate compares against comes from the bus the driver reports ─

TEST(XpuKernelTimer, nvptxPeakBandwidthComesFromTheBus) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Nvptx));
    ASSERT_NE(jit, nullptr);
    auto memKHz = jit->lookup<int64_t (*)()>("memoryClockKHz");
    auto bus    = jit->lookup<int64_t (*)()>("memoryBusWidthBits");
    auto peak   = jit->lookup<double (*)()>("peakBandwidthGBps");
    auto timed  = jit->lookup<int64_t (*)(uint64_t)>("timed");
    ASSERT_NE(memKHz, nullptr);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(peak, nullptr);
    ASSERT_NE(timed, nullptr);
    timed(1000);                                                  // touch the device
    const int64_t k = memKHz();
    const int64_t b = bus();
    const double  p = peak();
    std::printf(" RESULT u5_mem_clock_khz=%lld\n", (long long) k);
    std::printf(" RESULT u5_mem_bus_bits=%lld\n", (long long) b);
    std::printf(" RESULT u5_peak_gbps=%.1f\n", p);
    ASSERT_GT(k, 0);
    ASSERT_GT(b, 0);
    // Double data rate: 2 transfers per clock across the bus width.
    const double expect = 2.0 * (double) k * 1e3 * ((double) b / 8.0) / 1e9;
    EXPECT_NEAR(p, expect, expect * 0.001)
        << "peakBandwidthGBps is not 2 x memory clock x bus bytes";
    // Sanity on the physical ceiling: a discrete NVIDIA part this targets
    // reports between 100 GB/s and 10 TB/s.
    EXPECT_GT(p, 100.0);
    EXPECT_LT(p, 10000.0);
}

// ── the event clock's scale is measured, corrected, and reported ────────────
//
// Found by the agreement test below: on this WSL2 box the host window that
// ENCLOSES a device bracket read 4% shorter than the bracket, which no
// protocol can produce — only a clock. libcuda alone confirmed it: events
// around a 2.000 s sleep answered 2.091 s. This test brackets a host sleep
// with the runtime's own events, through the same natives KernelTimer uses,
// and requires the corrected figure to match the host clock within 1%.

TEST(XpuKernelTimer, nvptxEventClockScaleIsMeasuredAndCorrected) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Nvptx));
    ASSERT_NE(jit, nullptr);
    auto timed      = jit->lookup<int64_t (*)(uint64_t)>("timed");
    auto clockScale = jit->lookup<double (*)()>("clockScale");
    ASSERT_NE(timed, nullptr);
    ASSERT_NE(clockScale, nullptr);
    ASSERT_GT(timed(1000), 0);                                    // CUDA active, scale measured

    const double scale = clockScale();
    std::printf(" RESULT u5_event_clock_scale=%.4f\n", scale);
    ASSERT_GT(scale, 0.0) << "the timer refused this box's event clock";
    EXPECT_GT(scale, 0.9);
    EXPECT_LT(scale, 1.1);

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto create  = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_xpu_event_create"));
    auto record  = reinterpret_cast<void (*)(void*, int64_t, int64_t)>(sym("__cajeta_xpu_event_record"));
    auto wait    = reinterpret_cast<void (*)(void*, int64_t)>(sym("__cajeta_xpu_event_wait"));
    auto elapsed = reinterpret_cast<int64_t (*)(void*, int64_t, int64_t)>(sym("__cajeta_xpu_event_elapsed_nanos"));
    auto destroy = reinterpret_cast<void (*)(void*, int64_t)>(sym("__cajeta_xpu_event_destroy"));
    ASSERT_TRUE(create && record && wait && elapsed && destroy);

    const int64_t start = create();
    const int64_t end   = create();
    ASSERT_NE(start, 0);
    ASSERT_NE(end, 0);
    record(nullptr, start, 0);
    wait(nullptr, start);
    const auto h0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto h1 = std::chrono::steady_clock::now();
    record(nullptr, end, 0);
    const int64_t dev  = elapsed(nullptr, start, end);
    const int64_t host = std::chrono::duration_cast<std::chrono::nanoseconds>(h1 - h0).count();
    destroy(nullptr, start);
    destroy(nullptr, end);
    std::printf(" RESULT u5_sleep_host_ns=%lld\n", (long long) host);
    std::printf(" RESULT u5_sleep_device_ns=%lld\n", (long long) dev);
    ASSERT_GT(dev, 0);
    // The end record lands after h1 by one submission latency, so the device
    // figure is the longer one by well under 1% of 300 ms; a raw 4.56% scale
    // error would put it 13.7 ms out.
    EXPECT_GE(dev, host);
    EXPECT_LE((double) dev, (double) host * 1.01)
        << "the corrected event clock still disagrees with the host clock by more than 1%";
}

// ── 5.1.2 on the device path: an event that was never recorded is refused ──

TEST(XpuKernelTimer, nvptxRefusesANeverRecordedEvent) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSpinSource, "test.Spin",
                                  withBackend(cajeta::xpu::Backend::Nvptx));
    ASSERT_NE(jit, nullptr);
    auto timed = jit->lookup<int64_t (*)(uint64_t)>("timed");
    ASSERT_NE(timed, nullptr);
    ASSERT_GT(timed(1000), 0);                                    // CUDA is the active backend now

    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto create  = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_xpu_event_create"));
    auto record  = reinterpret_cast<void (*)(void*, int64_t, int64_t)>(sym("__cajeta_xpu_event_record"));
    auto elapsed = reinterpret_cast<int64_t (*)(void*, int64_t, int64_t)>(sym("__cajeta_xpu_event_elapsed_nanos"));
    auto destroy = reinterpret_cast<void (*)(void*, int64_t)>(sym("__cajeta_xpu_event_destroy"));
    ASSERT_NE(create, nullptr);
    ASSERT_NE(record, nullptr);
    ASSERT_NE(elapsed, nullptr)
        << "__cajeta_xpu_event_elapsed_nanos is missing — there is no device timer";
    ASSERT_NE(destroy, nullptr);

    const int64_t start = create();
    const int64_t end   = create();
    ASSERT_NE(start, 0);
    ASSERT_NE(end, 0);
    record(nullptr, end, 0);                                      // end only; start never recorded
    EXPECT_EQ(elapsed(nullptr, start, end), -1)
        << "an elapsed time between an unrecorded event and a recorded one is not a measurement";
    EXPECT_EQ(elapsed(nullptr, 0, end), -1) << "a null event handle produced a number";
    destroy(nullptr, start);
    destroy(nullptr, end);
}
