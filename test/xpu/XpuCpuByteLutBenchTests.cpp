// XpuCpuByteLutBenchTests — what `Vector<int8,16>.lut4(table)` costs on the CPU
// backend (xpu-kernel-adaptor plan 1.5.5.7.6).
//
// The portable `byteLut16` allocas the 16-byte table and gathers it a lane at a
// time. On NVPTX that alloca was `.local` and cost 16 bytes of scratch per call
// (1.5.5.4 replaced it with prmt.b32). On a host the alloca is an ordinary stack
// slot with real store-forwarding, and x86 `pshufb` is a 16-entry byte LUT, so
// the open question was whether an override is worth anything there. This is
// the measurement: the same three kernels the NVPTX test uses, launched on the
// cpu backend over a buffer that fits in cache (the lookup exposed) and one that
// does not (the memory floor), each timed best-of-N against the no-lookup
// control that moves identical bytes. The ratio lut4 / control is the cost of
// the lookup; a ratio near 1 at the large size says memory hides it, and the
// small-size ratio says what it costs when nothing does.
//
// Correctness is asserted (every byte against the host table lookup); the
// timings are printed and recorded in the plan, not asserted, since a
// threshold would turn a measurement into a flake on a loaded box.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// `n` work-items of 16 bytes each. timeX(n, reps) returns the best single-launch
// wall time in ns for that kernel; check(n) returns 0 when lutFull and lutTwo
// match the host lookup for every byte, else 1 + the first bad index.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.time.Clock;
public class M {
    @Device
    static Vector<int8,16> kv() {
        return stack Vector<int8,16>(0,1,2,3,4,6,8,12,0,-1,-2,-3,-4,-6,-8,-12);
    }
    @Kernel
    public static void lutFull(KernelBuffer<int8> out, KernelBuffer<int8> idx, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            Vector<int8,16> q = idx.vload<16>((int64) i * 16L);
            out.vstore((int64) i * 16L, q.lut4(M.kv()));
        }
    }
    @Kernel
    public static void lutTwo(KernelBuffer<int8> out, KernelBuffer<int8> idx, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            Vector<int8,16> q = idx.vload<16>((int64) i * 16L);
            Vector<int8,16> lo = (q & 15).lut4(M.kv());
            Vector<int8,16> hi = ((q >> 4) & 15).lut4(M.kv());
            out.vstore((int64) i * 16L, lo + hi);
        }
    }
    @Kernel
    public static void noLut(KernelBuffer<int8> out, KernelBuffer<int8> idx, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            Vector<int8,16> q = idx.vload<16>((int64) i * 16L);
            out.vstore((int64) i * 16L, (q & 15) + (q >> 4));
        }
    }
    static #int8[] indices(uint32 n) {
        int8[] h = heap int8[(int64) n * 16L];
        int64 k = 0;
        while (k < (int64) n * 16L) {
            h[k] = (int8) ((k * 37L + 11L) & 255L);
            k = k + 1;
        }
        return #h;
    }
    static int64 best(int32 which, uint32 n, int32 reps) {
        int8[] h #= M.indices(n);
        KernelBuffer<int8> idx = heap KernelBuffer<int8>((uint64) ((int64) n * 16L));
        KernelBuffer<int8> out = heap KernelBuffer<int8>((uint64) ((int64) n * 16L));
        idx.upload(h);
        KernelStream s #= KernelStream.current();
        int64 bestNs = 0L;
        int32 r = 0;
        while (r < reps) {
            int64 t0 = Clock.nanoTime();
            if (which == 0) { noLut.launch(s, grid: [(n + 255) / 256], block: [256])(out, idx, n); }
            if (which == 1) { lutFull.launch(s, grid: [(n + 255) / 256], block: [256])(out, idx, n); }
            if (which == 2) { lutTwo.launch(s, grid: [(n + 255) / 256], block: [256])(out, idx, n); }
            s.sync();
            int64 dt = Clock.nanoTime() - t0;
            if (bestNs == 0L || dt < bestNs) { bestNs = dt; }
            r = r + 1;
        }
        return bestNs;
    }
    public static int64 timeNoLut(uint32 n, int32 reps) { return M.best(0, n, reps); }
    public static int64 timeLutFull(uint32 n, int32 reps) { return M.best(1, n, reps); }
    public static int64 timeLutTwo(uint32 n, int32 reps) { return M.best(2, n, reps); }
    static int8 lookup(int8 v) {
        int32 i = (int32) v & 15;
        if (i == 0) { return (int8) 0; }
        if (i == 1) { return (int8) 1; }
        if (i == 2) { return (int8) 2; }
        if (i == 3) { return (int8) 3; }
        if (i == 4) { return (int8) 4; }
        if (i == 5) { return (int8) 6; }
        if (i == 6) { return (int8) 8; }
        if (i == 7) { return (int8) 12; }
        if (i == 8) { return (int8) 0; }
        if (i == 9) { return (int8) -1; }
        if (i == 10) { return (int8) -2; }
        if (i == 11) { return (int8) -3; }
        if (i == 12) { return (int8) -4; }
        if (i == 13) { return (int8) -6; }
        if (i == 14) { return (int8) -8; }
        return (int8) -12;
    }
    public static int64 check(uint32 n) {
        int8[] h #= M.indices(n);
        KernelBuffer<int8> idx = heap KernelBuffer<int8>((uint64) ((int64) n * 16L));
        KernelBuffer<int8> out = heap KernelBuffer<int8>((uint64) ((int64) n * 16L));
        idx.upload(h);
        KernelStream s #= KernelStream.current();
        lutFull.launch(s, grid: [(n + 255) / 256], block: [256])(out, idx, n);
        s.sync();
        int8[] got = heap int8[(int64) n * 16L];
        out.download(got);
        int64 k = 0;
        while (k < (int64) n * 16L) {
            if (got[k] != M.lookup(h[k])) { return 1L + k; }
            k = k + 1;
        }
        lutTwo.launch(s, grid: [(n + 255) / 256], block: [256])(out, idx, n);
        s.sync();
        out.download(got);
        k = 0;
        while (k < (int64) n * 16L) {
            int8 want = (int8) (M.lookup((int8) ((int32) h[k] & 15)) + M.lookup((int8) (((int32) h[k] >> 4) & 15)));
            if (got[k] != want) { return 1000000000L + k; }
            k = k + 1;
        }
        return 0L;
    }
}
)CJ";

} // namespace

TEST(XpuCpuByteLutBenchTests, lut4IsCorrectOnCpuAndItsCostIsMeasured) {
    auto jit = CajetaJit::compile(kSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto check = jit->lookup<int64_t (*)(uint32_t)>("check");
    auto tNo   = jit->lookup<int64_t (*)(uint32_t, int32_t)>("timeNoLut");
    auto tOne  = jit->lookup<int64_t (*)(uint32_t, int32_t)>("timeLutFull");
    auto tTwo  = jit->lookup<int64_t (*)(uint32_t, int32_t)>("timeLutTwo");
    ASSERT_NE(check, nullptr);
    ASSERT_NE(tNo, nullptr);
    ASSERT_NE(tOne, nullptr);
    ASSERT_NE(tTwo, nullptr);

    // Correctness first: the cpu gather against the host table, every byte.
    EXPECT_EQ(check(1u << 12), 0) << "byte index (minus 1) of the first mismatch";

    // Three sizes, 16 bytes per work-item each way:
    //   small  64 KB   — cache-resident but dominated by the pool's dispatch and
    //                    join (~400 us per launch on this box), so a control only;
    //   mid     2 MB   — cache-resident AND long enough to amortize dispatch:
    //                    the arm where a lookup cost would show;
    //   large  64 MB   — past any cache, the memory floor.
    const int32_t reps = 15;
    auto gbps = [](uint32_t n, int64_t ns) { return 2.0 * 16.0 * n / (double) ns; };
    for (uint32_t n : {1u << 12, 1u << 17, 1u << 22}) {
        int64_t no = tNo(n, reps), one = tOne(n, reps), two = tTwo(n, reps);
        ASSERT_GT(no, 0);
        std::printf("[lut4-cpu] n=%u (%.1f MB each way): noLut %lld ns (%.1f GB/s), "
                    "lut4x1 %lld ns (x%.2f), lut4x2 %lld ns (x%.2f)\n",
                    n, 16.0 * n / 1048576.0, (long long) no, gbps(n, no),
                    (long long) one, (double) one / no, (long long) two, (double) two / no);
    }
}
