//
// xpu-kernel-adaptor 4.2.1.1 / 4.1.1 — a loop whose exit depends on the
// work-item id is scaffold too, with the work-item PREDICATED.
//
// MEASURED CAUSE A. The lane-strided idiom every wave mat-vec in cajeta-llm
// is written in -- `b = lane >> 3; while (b < blocksPerRow) { ...; b += 4 }`
// with the wave reduce AFTER the loop -- has an exiting condition that is
// per work-item, so fission step 4b could not make the loop workgroup-uniform
// scaffold, the loop stayed inside its region, the region's work-item loop
// wrapped it as an OUTER loop, LoopVectorize (innermost loops only) never
// widened the work-item loop, and the reduce after it was left scalar. All 53
// cpu DECLINED rows in cajeta-llm's census sat on this one shape (2026-09-25).
//
// THE TRANSFORM these tests pin. A barrier-free loop with a per-work-item
// exit becomes scaffold with each work-item carrying an `active` flag in a
// context array: the loop runs while ANY work-item is active, every trip runs
// the body region under each work-item's own flag, and an exiting edge clears
// the flag instead of leaving. A work-item that is done simply skips its
// trips; the code after the loop sits in a region of its own, a clean
// counted work-item loop, and widens at the wave width.
//
// Every assertion checks the VALUE per lane, never only the registration: a
// wave reduce run at width 1 answers this work-item's own number.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// -1 refused (the launch raised, or the output was never written), -2 the
// harness, -3 no wave, else the kernel's own verdict.
int runOnCpu(const std::string& src) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

const char* kPreamble = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Workgroup;
import cajeta.xpu.Barrier;
import cajeta.xpu.Shared;
import cajeta.xpu.Wave;
import cajeta.lang.System;
public class M {
    @Kernel
    public static void widthk(KernelBuffer<uint32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = Wave.width();
    }
    public static uint32 probeWidth() {
        uint32[] hw = heap uint32[64];
        KernelBuffer<uint32> bw = heap KernelBuffer<uint32>(64);
        KernelStream s #= KernelStream.current();
        widthk.launch(s, grid: [1], block: [64])(bw);
        s.sync();
        bw.download(hw);
        return hw[0];
    }
    public static int32 verify(float32[] h, uint32 n, float32[] want) {
        uint32 i = 0;
        while (i < n) {
            if (h[i] != want[i]) {
                System.stdout.println("lane " + (int32) i + " got " + h[i]
                    + " want " + want[i]);
                return 100 + (int32) i;
            }
            i = i + 1;
        }
        return 0;
    }
)CJ";

// ---- (a) the idiom itself, ragged: lanes take a different trip count --- //
//
// n = 40 four-element rows, stride 32: lanes 0..7 (mod 32) take two trips,
// the rest one. So the wave sum differs by wave, and a width-1 reduce, a
// uniform trip count, or a lane that ran one trip too many all read wrong.
const char* kRaggedSrc = R"CJ(
    @Kernel
    public static void lk(KernelBuffer<float32> out, KernelBuffer<float32> in,
                          uint32 n) {
        uint32 t = KernelThread.globalIdX();
        uint32 lane = KernelThread.x() % 32;
        float32 acc = 0.0f;
        uint32 e = lane;
        while (e < n) {
            Vector<float32,4> v = in.vload<4>((int64) (e * 4));
            acc = acc + v[0] + v[1] + v[2] + v[3];
            e = e + 32;
        }
        out[t] = Wave.reduceSumF32(acc);
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hin = heap float32[256];
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < 256) { hin[i] = 1.0f; i = i + 1; }
        i = 0;
        while (i < n) { hout[i] = -1.0f; i = i + 1; }
        // Per lane: 8.0 for lane%32 < 8 else 4.0; the wave sums W of them.
        i = 0;
        while (i < n) {
            float32 s = 0.0f;
            uint32 j = 0;
            while (j < w) {
                uint32 l = (i / w) * w + j;
                s = s + ((l % 32 < 8) ? 8.0f : 4.0f);
                j = j + 1;
            }
            want[i] = s;
            i = i + 1;
        }
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bin.upload(hin);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            lk.launch(s, grid: [1], block: [64])(bout, bin, 40);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, aLaneStridedLoopWithRaggedTripsWidensTheReduceAfterIt) {
    const int r = runOnCpu(std::string(kPreamble) + kRaggedSrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (-1 refused, 100+i wrong at lane i)";
}

// ---- (b) the wave mat-vec shape: the loop under a uniform guard --------- //
//
// `if (r0 < nr) { b = lane >> 3; while (b < nb) { ... b += 4 } }` then the
// reduce and the lane-0 store. The guard is workgroup-uniform by provenance
// (Workgroup.x()) but holds no barrier, so 4a never split it; the loop under
// it has to be reached as scaffold through a scaffold branch.
const char* kGuardedSrc = R"CJ(
    @Kernel
    public static void mv(KernelBuffer<float32> y, KernelBuffer<float32> in,
                          uint32 rows, uint32 nb) {
        uint32 lane = KernelThread.x();
        uint32 r0 = Workgroup.x();
        float32 acc = 0.0f;
        if (r0 < rows) {
            uint32 sub = lane & 7;
            uint32 b = lane >> 3;
            while (b < nb) {
                acc = acc + in[(int64) (b * 8 + sub)];
                b = b + 4;
            }
        }
        float32 t0 = Wave.reduceSumF32(acc);
        if (lane == 0) {
            if (r0 < rows) { y[(int64) r0] = t0; }
        }
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        float32[] hin = heap float32[64];
        uint32 i = 0;
        while (i < 64) { hin[i] = (float32) i; i = i + 1; }
        float32[] hy = heap float32[2];
        hy[0] = -1.0f; hy[1] = -1.0f;
        // Block 0, wave 0 = lanes 0..w-1: lane l reads in[l] and, when its
        // b = l>>3 walk continues, in[32+l] (nb = 6 blocks: b = 0, 4 for
        // lanes 0..7; b = 1, 5 for 8..15; ...). Sum over the wave's lanes.
        float32 want0 = 0.0f;
        i = 0;
        while (i < w) {
            uint32 sub = i & 7;
            uint32 b = i >> 3;
            while (b < 6) { want0 = want0 + (float32) (b * 8 + sub); b = b + 4; }
            i = i + 1;
        }
        float32[] want = heap float32[2];
        want[0] = want0;
        want[1] = -1.0f;
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(64);
        KernelBuffer<float32> by = heap KernelBuffer<float32>(2);
        bin.upload(hin);
        by.upload(hy);
        KernelStream s #= KernelStream.current();
        try {
            mv.launch(s, grid: [2], block: [32])(by, bin, 1, 6);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        by.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        return verify(hy, 2, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, theWaveMatVecShapeUnderAUniformGuardLowersAndAgrees) {
    const int r = runOnCpu(std::string(kPreamble) + kGuardedSrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (-1 refused, 100 wrong row sum, 101 the "
                       "guarded-out row was written)";
}

// ---- (c) the top-k shape: tainted loops nested in a uniform loop -------- //
//
// `while (k < used) { e = lane; while (e < experts) {...}; vk = reduce(...);
// ... }`: the inner loop is per-work-item, the outer is uniform, and a wave
// op sits between the two inside the outer body.
const char* kNestedSrc = R"CJ(
    @Kernel
    public static void tk(KernelBuffer<float32> out, KernelBuffer<float32> in,
                          uint32 experts, uint32 used) {
        uint32 t = KernelThread.globalIdX();
        uint32 lane = KernelThread.x();
        float32 acc = 0.0f;
        uint32 k = 0;
        while (k < used) {
            float32 bv = 0.0f;
            uint32 e = lane;
            while (e < experts) {
                bv = bv + in[(int64) e];
                e = e + 32;
            }
            float32 vk = Wave.reduceSumF32(bv);
            acc = acc + vk;
            k = k + 1;
        }
        out[t] = acc;
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hin = heap float32[64];
        uint32 i = 0;
        while (i < 64) { hin[i] = 1.0f; i = i + 1; }
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        i = 0;
        while (i < n) {
            hout[i] = -1.0f;
            float32 s = 0.0f;
            uint32 j = 0;
            while (j < w) {
                uint32 l = (i / w) * w + j;
                uint32 e = l;
                while (e < 40) { s = s + 1.0f; e = e + 32; }
                j = j + 1;
            }
            want[i] = 3.0f * s;
            i = i + 1;
        }
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(64);
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bin.upload(hin);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            tk.launch(s, grid: [1], block: [64])(bout, bin, 40, 3);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, taintedLoopsNestedInAUniformLoopWithAWaveOpBetween) {
    const int r = runOnCpu(std::string(kPreamble) + kNestedSrc);
    EXPECT_EQ(r, 0) << "r=" << r;
}

// ---- (d) the qkPrep shape: lane-strided loops either side of a barrier -- //
const char* kBarrierSrc = R"CJ(
    @Kernel
    public static void qk(KernelBuffer<float32> out, KernelBuffer<float32> in,
                          uint32 hd) {
        Shared<float32> part = shared float32[64];
        uint32 lane = KernelThread.x();
        float32 ss = 0.0f;
        uint32 i = lane;
        while (i < hd) { ss = ss + in[(int64) i]; i = i + 64; }
        float32 wsum = Wave.reduceSumF32(ss);
        uint32 ww = Wave.width();
        if (lane % ww == 0) { part[lane / ww] = wsum; }
        Barrier.workgroup();
        float32 tot = 0.0f;
        uint32 p = 0;
        while (p < 64 / ww) { tot = tot + part[p]; p = p + 1; }
        uint32 j = lane;
        while (j < hd) { out[(int64) j] = in[(int64) j] * tot; j = j + 64; }
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 hd = 100;
        float32[] hin = heap float32[hd];
        float32[] hout = heap float32[hd];
        float32[] want = heap float32[hd];
        uint32 i = 0;
        while (i < hd) { hin[i] = 1.0f; hout[i] = -1.0f; want[i] = 100.0f; i = i + 1; }
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(hd);
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(hd);
        bin.upload(hin);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            qk.launch(s, grid: [1], block: [64])(bout, bin, hd);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, hd, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, laneStridedLoopsEitherSideOfABarrierLowerAndAgree) {
    const int r = runOnCpu(std::string(kPreamble) + kBarrierSrc);
    EXPECT_EQ(r, 0) << "r=" << r;
}

// ---- (e) a per-work-item break leaves the loop for that work-item ------ //
//
// The exiting edge is not the header's: `if (j >= t) break;` inside a loop
// whose header condition is uniform. Each work-item accumulates min(n, t).
const char* kBreakSrc = R"CJ(
    @Kernel
    public static void bk(KernelBuffer<float32> out, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) {
            if (j >= t) { break; }
            acc = acc + 1.0f;
            j = j + 1;
        }
        out[t] = Wave.reduceSumF32(acc);
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < n) {
            hout[i] = -1.0f;
            float32 s = 0.0f;
            uint32 j = 0;
            while (j < w) {
                uint32 l = (i / w) * w + j;
                s = s + (float32) (l < 5 ? l : 5);
                j = j + 1;
            }
            want[i] = s;
            i = i + 1;
        }
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            bk.launch(s, grid: [1], block: [64])(bout, 5);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

// TRACKED: xpu-kernel-adaptor 4.2.1.3. A `break` gives the loop a SECOND
// exiting edge, and the predicated scaffold in 4b takes a loop whose exits
// are the header's and the latch's. Measured 2026-09-26: this shape is
// refused, not miscompiled, so the kernel has no cpu device code and the
// launch raises. When 4b learns the multi-exit case this flips to the value
// check (each work-item accumulates min(n, t), the wave sums those).
TEST(XpuCpuFissionUniformize, aPerWorkItemBreakIsStillRefused) {
    const int r = runOnCpu(std::string(kPreamble) + kBreakSrc);
    EXPECT_EQ(r, -1)
        << "a per-work-item `break` inside a scaffold loop REGISTERED (r=" << r
        << "); until 4.2.1.3 lands it must be refused rather than run a wave "
           "op at width 1";
}

// ---- (f) a return inside the loop ends the work-item, not the loop ------ //
//
// Work-item 3 returns from inside the loop before writing; the others run
// every trip. The leaver's slot keeps its sentinel, it must not run the code
// after the loop either (the activity mask), and the wave reduce after the
// loop sees it as an absent lane (mask-as-data): its wave sums 5 over the
// remaining lanes, every other wave sums 5 over all of them.
const char* kReturnSrc = R"CJ(
    @Kernel
    public static void rk(KernelBuffer<float32> out, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) {
            if (t == 3 && j == 2) { return; }
            acc = acc + 1.0f;
            j = j + 1;
        }
        out[t] = Wave.reduceSumF32(acc);
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < n) {
            hout[i] = -1.0f;
            float32 s = 0.0f;
            uint32 j = 0;
            while (j < w) {
                uint32 l = (i / w) * w + j;
                if (l != 3) { s = s + 5.0f; }
                j = j + 1;
            }
            want[i] = (i == 3) ? -1.0f : s;
            i = i + 1;
        }
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            rk.launch(s, grid: [1], block: [64])(bout, 5);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, aReturnInsideAScaffoldLoopEndsThatWorkItem) {
    const int r = runOnCpu(std::string(kPreamble) + kReturnSrc);
    EXPECT_EQ(r, 0) << "r=" << r;
}

// ======================================================================== //
// xpu-kernel-adaptor 4.2.1.3 -- three scaffold defects measured at 072d0bed
// and documented beside the control shapes in XpuCpuVectorScalarizeTests.
// ======================================================================== //

// (g) A `break` on a WORKGROUP-UNIFORM condition. Measured 2026-09-25: the
// second exiting edge made 4b decline to scaffold, the whole kernel landed in
// one region whose work-item loop enclosed the while loop, and the reduce was
// left scalar. Under the predicated scaffold the break is one more leaving
// edge, and every lane accumulates min(n, m).
const char* kUniformBreakSrc = R"CJ(
    @Kernel
    public static void ub(KernelBuffer<float32> out, uint32 n, uint32 m) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) {
            if (j == m) { break; }
            acc = acc + 1.0f;
            j = j + 1;
        }
        out[t] = Wave.reduceSumF32(acc);
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < n) { hout[i] = -1.0f; want[i] = 3.0f * (float32) w; i = i + 1; }
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            ub.launch(s, grid: [1], block: [64])(bout, 6, 3);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

// TRACKED: xpu-kernel-adaptor 4.2.1.3. The same second-exiting-edge shape,
// with the break on a workgroup-uniform condition. Refused for the same
// reason, and pinned as refused so the day it lowers is visible.
TEST(XpuCpuFissionUniformize, aUniformBreakInsideAScaffoldLoopIsStillRefused) {
    const int r = runOnCpu(std::string(kPreamble) + kUniformBreakSrc);
    EXPECT_EQ(r, -1)
        << "a uniform `break` inside a scaffold loop REGISTERED (r=" << r << ")";
}

// (h) A wave reduce of a workgroup-uniform CONSTANT inside a scaffold loop.
// Measured 2026-09-25: it answered 0 where W is right. The vectorizer sees a
// call whose operands are all loop-invariant and replicates it once per
// vector iteration as a scalar, the width-1 identity, which the gate now
// refuses (4.2.1.2) rather than ships. Widening it is the fix pinned here:
// every lane answers n * W.
const char* kUniformReduceSrc = R"CJ(
    @Kernel
    public static void ur(KernelBuffer<float32> out, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) { acc = acc + Wave.reduceSumF32(1.0f); j = j + 1; }
        out[t] = acc;
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < n) { hout[i] = -1.0f; want[i] = 4.0f * (float32) w; i = i + 1; }
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            ur.launch(s, grid: [1], block: [64])(bout, 4);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

// TRACKED: xpu-kernel-adaptor 4.2.1.3, and the reason the gate grew its
// vanished-wave-op check. The accumulator is workgroup-uniform, so the
// region's work-item loop has every work-item re-reading the same start
// value, every iteration's result but the last is dead, and LLVM deletes the
// region's whole vector body -- measured 2026-09-26 off the emitted IR. The
// kernel used to REGISTER and answer 0 where W is right, a silent wrong
// answer. It is now refused by name. The fix that flips this to a value check
// is per-work-item accumulation for a local carrying a wave result.
TEST(XpuCpuFissionUniformize, aWaveReduceOfAUniformConstantIsRefusedNotWrong) {
    const int r = runOnCpu(std::string(kPreamble) + kUniformReduceSrc);
    EXPECT_EQ(r, -1)
        << "a uniform accumulator fed by a wave reduce answered " << r
        << " instead of being refused; 0 (the width-1 identity) silently "
           "replacing " << "n*W is the outcome the gate exists to prevent";
}

// (i) A workgroup-UNIFORM accumulator updated inside NESTED scaffold loops.
// Measured 2026-09-25: 4 where 16 is right, because 4b snapshotted the
// uniform local per loop TRIP and the outer header's snapshot rewound the
// inner loop's work. The snapshot belongs to the REGION: every work-item
// starts a region with the value the region was entered with, whatever loop
// nest the region sits in.
const char* kNestedUniformAccSrc = R"CJ(
    @Kernel
    public static void nu(KernelBuffer<float32> out, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) {
            uint32 k = 0;
            while (k < n) { acc = acc + 1.0f; k = k + 1; }
            j = j + 1;
        }
        out[t] = Wave.reduceSumF32(acc);
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 n = 64;
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < n) { hout[i] = -1.0f; want[i] = 16.0f * (float32) w; i = i + 1; }
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            nu.launch(s, grid: [1], block: [64])(bout, 4);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, aUniformAccumulatorInNestedScaffoldLoopsKeepsTheInnerUpdates) {
    const int r = runOnCpu(std::string(kPreamble) + kNestedUniformAccSrc);
    EXPECT_EQ(r, 0) << "r=" << r;
}

// (j) The same rule with no loop at all: a uniform local initialised in one
// region and updated in the next (across a barrier) is per work-item on the
// device, so every work-item answers 1, not the block's count.
const char* kUniformAcrossBarrierSrc = R"CJ(
    @Kernel
    public static void ab(KernelBuffer<float32> out) {
        uint32 t = KernelThread.globalIdX();
        float32 cnt = 0.0f;
        out[t] = 0.0f;
        Barrier.workgroup();
        cnt = cnt + 1.0f;
        Barrier.workgroup();
        out[t] = cnt;
    }
    public static int32 run() {
        uint32 n = 64;
        float32[] hout = heap float32[n];
        float32[] want = heap float32[n];
        uint32 i = 0;
        while (i < n) { hout[i] = -1.0f; want[i] = 1.0f; i = i + 1; }
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            ab.launch(s, grid: [1], block: [64])(bout);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return verify(hout, n, want);
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, aUniformLocalUpdatedInALaterRegionIsPerWorkItem) {
    const int r = runOnCpu(std::string(kPreamble) + kUniformAcrossBarrierSrc);
    EXPECT_EQ(r, 0) << "r=" << r;
}

}  // namespace
