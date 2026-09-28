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
import cajeta.xpu.Group;
import cajeta.xpu.GroupOp;
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

// DISABLED, NEEDS A FIX (xpu-kernel-adaptor 4.2.1.9). Red on main at 1fd837d6
// and a0572e8d, at wave width 16 and 8. The post-fission verifier refuses qk:
// "%tot = load ... does not dominate %30 = fadd %tot, %elem15". 4b peels the
// uniform tail of `while (p < 64 / ww) { tot = tot + part[p]; p = p + 1; }`
// into the scaffold latch, and scaffoldSafe accepts the fadd and the store of
// tot although their operands are defined in the body region. A tail check that
// stops at any operand defined outside the tail, trueEntry or the header makes
// this pass. It then turns aWaveReduceOfAUniformConstantIsRefusedNotWrong into
// a silent 4096 for 64, because that refusal came from the same bad tail: the
// wave-fed read-modify-write of `acc` then stays in one shared slot. Land the
// tail check together with per-work-item storage for a wave-fed RMW local.
// RE-ENABLED 2026-09-27. The disable note below called the fix exactly right,
// and both halves of it are now in CpuBarrierFission: the peel stops at any
// operand defined outside the peeled run, and a wave-fed read-modify-written
// local gets a per-work-item context array so the tail fix does not turn this
// into a silent wrong answer. Two sessions reached the same diagnosis
// independently (xpu-kernel-adaptor 4.2.1.9).
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

// DISABLED, NEEDS A FIX (xpu-kernel-adaptor 4.2.1.9). Red on main at 1fd837d6
// and a0572e8d, at wave width 16 and 8: "lane 0 got 80 want 75" at 16. Two
// causes, read off CAJETA_XPU_CPU_DUMP_PREOPT. (1) The body region reaches both
// the `return` and the scaffold latch, and the walk picks wrapEnd over the latch
// when both are reached, so the work-item loop exits to wrap.end and no alive
// array is built. Ordering latch and stop before ret fixes it. (2) acc and j are
// uniform by taint but each work-item runs its own trips, so they must be
// context arrays. Forcing into forceCtx every local the predicated body stores
// and reads back across a trip, or reads after the loop, makes this pass, and
// it also makes XpuCpuDistCoopVerb.waveOpLeftScalarIsRefused register a
// cooperative mma under a per-lane trip count. That kernel must stay refused.
// RE-ENABLED 2026-09-27. The disable note below called the fix exactly right,
// and both halves of it are now in CpuBarrierFission: the peel stops at any
// operand defined outside the peeled run, and a wave-fed read-modify-written
// local gets a per-work-item context array so the tail fix does not turn this
// into a silent wrong answer. Two sessions reached the same diagnosis
// independently (xpu-kernel-adaptor 4.2.1.9).
// RE-ENABLED 2026-09-27 — the cause measured below was the region walk ranking
// a `ret` above the loop latch, so no activity mask was ever built. Kept as
// the record of how it was found (xpu-kernel-adaptor 4.2.1.9).
// The sibling probe beside it passes since the peel and wave-fed-RMW fixes, so
// those were not what this one needed.
//
// MEASURED 2026-09-27 at width 16: "lane 0 got 80 want 75", and 80 is 16 * 5 —
// EVERY lane of the wave contributed a full five, work-item 3 included. So the
// early `return` did not end that work-item at all: it ran every trip and
// reached the reduce. Expected is 15 * 5, the wave minus the one that left.
//
// The shape is a `return` from inside a PREDICATED scaffold loop. The return
// block is not the loop's exit block, so 4b's exit-edge redirect leaves that
// edge alone, and whether the work-item stops depends on the region walk
// claiming the return block and step 9 turning its `ret` into "clear the
// activity mask, then take the work-item latch". One of those is not
// happening here. Diagnose with [wave-qual] plus the region dump before
// changing anything: the activity mask is only materialized when a region can
// be left early, which is itself a condition worth checking first.
TEST(XpuCpuFissionUniformize, aReturnInsideAScaffoldLoopEndsThatWorkItem) {
    const int r = runOnCpu(std::string(kPreamble) + kReturnSrc);
    EXPECT_EQ(r, 0) << "r=" << r;
}

// ---- (f2) the predicated loop under a PER-WAVE guard with dead waves ---- //
//
// q2kQ8WaveMatVecKernel's shape, the width-portable wave mat-vec: `row =
// globalIdX / width; if (row < rows) { lane-strided loop }` then the reduce
// and the lane-0 store. With two rows and a block of 64 threads, every wave
// past the second is DEAD: its work-items never pass the guard, never run
// the loop's preheader, never arm or clear anything. On 2026-09-28 the llm
// test for this kernel, running on cpu for the first time (its cpu-name
// guard had come off), spun one thread for 33 minutes on a two-row
// mat-vec. Whatever the dead waves leave behind must not keep the
// scaffold loop alive: the kernel must end, and rows 0 and 1 must be right.
const char* kDeadWaveSrc = R"CJ(
    @Kernel
    public static void dw(KernelBuffer<float32> y, KernelBuffer<float32> in,
                          uint32 rows, uint32 nb) {
        uint32 w = Wave.width();
        uint32 lane = KernelThread.x() % w;
        uint32 row = KernelThread.globalIdX() / w;
        float32 acc = 0.0f;
        if (row < rows) {
            uint32 b = lane;
            while (b < nb) {
                acc = acc + in[(int64) (row * nb + b)];
                b = b + w;
            }
        }
        float32 t0 = Wave.reduceSumF32(acc);
        if (lane == 0) {
            if (row < rows) { y[(int64) row] = t0; }
        }
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 nb = 12;
        float32[] hin = heap float32[2 * nb];
        uint32 i = 0;
        while (i < 2 * nb) { hin[i] = (float32) (i + 1); i = i + 1; }
        float32[] want = heap float32[2];
        uint32 r = 0;
        while (r < 2) {
            float32 s = 0.0f;
            uint32 b = 0;
            while (b < nb) { s = s + hin[r * nb + b]; b = b + 1; }
            want[r] = s;
            r = r + 1;
        }
        float32[] hy = heap float32[2];
        hy[0] = -1.0f; hy[1] = -1.0f;
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(2 * nb);
        KernelBuffer<float32> by = heap KernelBuffer<float32>(2);
        bin.upload(hin);
        by.upload(hy);
        KernelStream s #= KernelStream.current();
        try {
            dw.launch(s, grid: [1], block: [64])(by, bin, 2, nb);
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

TEST(XpuCpuFissionUniformize, aPredicatedLoopUnderADeadWaveGuardStillEnds) {
    const int r = runOnCpu(std::string(kPreamble) + kDeadWaveSrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (-1 refused, 100+i wrong at row i;"
                       " a TIMEOUT here is the scaffold loop never ending)";
}

// ---- (g2) the Group surface and a wave op in ONE kernel, on cpu --------- //
//
// On cpu every Group native is the runtime's one-work-item stub: width 1,
// lane 0, identity reduce. A kernel that takes its geometry from
// Group.width() but reduces with Wave.reduceSumF32 is a WAVE kernel (it
// vectorizes at the host width) with width-1 geometry. q2kQ8WaveMatVecKernel
// strides its loop by width / 4, which is 0, and on 2026-09-28 its first cpu
// run spun one thread for 33 minutes on a two-row mat-vec. Such a kernel is
// REFUSED on cpu by name (xpu-kernel-adaptor Unit 9 owns the fold), and a
// Group-only kernel still lowers and answers right at width 1: both arms.
const char* kGroupPlusWaveSrc = R"CJ(
    @Kernel
    public static void gw(KernelBuffer<float32> y, KernelBuffer<float32> in,
                          uint32 rows, uint32 nb) {
        uint32 w = (uint32) Group.width();
        uint32 lane = KernelThread.x() % w;
        uint32 row = KernelThread.globalIdX() / w;
        float32 acc = 0.0f;
        if (row < rows) {
            uint32 b = lane;
            while (b < nb) {
                acc = acc + in[(int64) (row * nb + b)];
                b = b + w;
            }
        }
        float32 tot = Wave.reduceSumF32(acc);
        if (lane == 0 && row < rows) { y[(int64) row] = tot; }
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32 nb = 12;
        float32[] hin = heap float32[2 * nb];
        uint32 i = 0;
        while (i < 2 * nb) { hin[i] = (float32) (i + 1); i = i + 1; }
        float32[] want = heap float32[2];
        uint32 r = 0;
        while (r < 2) {
            float32 s = 0.0f;
            uint32 b = 0;
            while (b < nb) { s = s + hin[r * nb + b]; b = b + 1; }
            want[r] = s;
            r = r + 1;
        }
        float32[] hy = heap float32[2];
        hy[0] = -1.0f; hy[1] = -1.0f;
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(2 * nb);
        KernelBuffer<float32> by = heap KernelBuffer<float32>(2);
        bin.upload(hin);
        by.upload(hy);
        KernelStream s #= KernelStream.current();
        try {
            gw.launch(s, grid: [1], block: [64])(by, bin, 2, nb);
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

// FOLDED 2026-09-28 (Julian: "Fold"). Group.width() is the wave width on cpu
// now, so this kernel's geometry and its wave reduce agree: it lowers, runs at
// the host width and answers the row sums. The refusal it used to pin moved
// to the one mix that still cannot agree, a Group kernel with a forced-32 coop
// tile, whose host launch geometry cannot follow a per-kernel 32.
TEST(XpuCpuFissionUniformize, aGroupWidthKernelWithAWaveReduceLowersAndAgreesOnCpu) {
    const int r = runOnCpu(std::string(kPreamble) + kGroupPlusWaveSrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (-1 refused, 100+i wrong at row i)";
}

// Group.width() and Wave.width() are ONE width on cpu: what every lane sees.
const char* kWidthsAgreeSrc = R"CJ(
    @Kernel
    public static void wa(KernelBuffer<uint32> out) {
        uint32 t = KernelThread.globalIdX();
        out[(int64) (t * 3)] = (uint32) Group.width();
        out[(int64) (t * 3 + 1)] = Wave.width();
        out[(int64) (t * 3 + 2)] = (uint32) Group.laneId();
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        uint32[] h = heap uint32[64 * 3];
        KernelBuffer<uint32> b = heap KernelBuffer<uint32>(64 * 3);
        KernelStream s #= KernelStream.current();
        try {
            wa.launch(s, grid: [1], block: [64])(b);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        b.download(h);
        uint32 t = 0;
        while (t < 64) {
            if (h[t * 3] != w) {
                System.stdout.println("work-item " + (int32) t + ": Group.width "
                    + (int32) h[t * 3] + " but the wave is " + (int32) w);
                return 100 + (int32) t;
            }
            if (h[t * 3 + 1] != w) { return 200 + (int32) t; }
            if (h[t * 3 + 2] != t % w) {
                System.stdout.println("work-item " + (int32) t + ": Group.laneId "
                    + (int32) h[t * 3 + 2] + " want " + (int32) (t % w));
                return 300 + (int32) t;
            }
            t = t + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuFissionUniformize, groupWidthAndLaneIdAreTheWaveOnCpu) {
    const int r = runOnCpu(std::string(kPreamble) + kWidthsAgreeSrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (100+t Group.width is not the wave at work-item t,"
                       " 300+t Group.laneId is not t mod width)";
}

// Group.stripe + Group.reduce: the width-agnostic mat-vec row, on cpu at the
// host width. Each lane strides the row, the group reduce combines the lanes.
const char* kStripeReduceSrc = R"CJ(
    @Kernel
    public static void sr(KernelBuffer<float32> y, KernelBuffer<float32> in,
                          uint32 rows, int32 nb) {
        uint32 w = (uint32) Group.width();
        uint32 row = KernelThread.globalIdX() / w;
        float32 acc = 0.0f;
        if (row < rows) {
            for (int32 b : Group.stripe(nb)) {
                acc = acc + in[(int64) row * (int64) nb + (int64) b];
            }
        }
        float32 tot = Group.reduce(GroupOp.Add, acc);
        if (Group.laneId() == 0 && row < rows) { y[(int64) row] = tot; }
    }
    public static int32 run() {
        uint32 w = probeWidth();
        if (w < 2) { return -3; }
        int32 nb = 13;
        float32[] hin = heap float32[2 * 13];
        uint32 i = 0;
        while (i < 26) { hin[i] = (float32) (i + 1); i = i + 1; }
        float32[] want = heap float32[2];
        uint32 r = 0;
        while (r < 2) {
            float32 s = 0.0f;
            uint32 b = 0;
            while (b < 13) { s = s + hin[r * 13 + b]; b = b + 1; }
            want[r] = s;
            r = r + 1;
        }
        float32[] hy = heap float32[2];
        hy[0] = -1.0f; hy[1] = -1.0f;
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(26);
        KernelBuffer<float32> by = heap KernelBuffer<float32>(2);
        bin.upload(hin);
        by.upload(hy);
        KernelStream s #= KernelStream.current();
        try {
            sr.launch(s, grid: [1], block: [64])(by, bin, 2, nb);
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

TEST(XpuCpuFissionUniformize, groupStripeAndReduceAgreeAtTheWaveWidthOnCpu) {
    const int r = runOnCpu(std::string(kPreamble) + kStripeReduceSrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (-1 refused, 100+i wrong at row i)";
}

const char* kGroupOnlySrc = R"CJ(
    @Kernel
    public static void go(KernelBuffer<float32> y, KernelBuffer<float32> in,
                          uint32 rows, uint32 nb) {
        uint32 w = (uint32) Group.width();
        uint32 lane = KernelThread.x() % w;
        uint32 row = KernelThread.globalIdX() / w;
        float32 acc = 0.0f;
        if (row < rows) {
            uint32 b = lane;
            while (b < nb) {
                acc = acc + in[(int64) (row * nb + b)];
                b = b + w;
            }
        }
        float32 tot = Group.reduce(GroupOp.Add, acc);
        if (lane == 0 && row < rows) { y[(int64) row] = tot; }
    }
    public static int32 run() {
        uint32 nb = 12;
        float32[] hin = heap float32[2 * nb];
        uint32 i = 0;
        while (i < 2 * nb) { hin[i] = (float32) (i + 1); i = i + 1; }
        float32[] want = heap float32[2];
        uint32 r = 0;
        while (r < 2) {
            float32 s = 0.0f;
            uint32 b = 0;
            while (b < nb) { s = s + hin[r * nb + b]; b = b + 1; }
            want[r] = s;
            r = r + 1;
        }
        float32[] hy = heap float32[2];
        hy[0] = -1.0f; hy[1] = -1.0f;
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(2 * nb);
        KernelBuffer<float32> by = heap KernelBuffer<float32>(2);
        bin.upload(hin);
        by.upload(hy);
        KernelStream s #= KernelStream.current();
        try {
            go.launch(s, grid: [1], block: [64])(by, bin, 2, nb);
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

TEST(XpuCpuFissionUniformize, aGroupOnlyKernelLowersAndAnswersAtTheWaveWidth) {
    const int r = runOnCpu(std::string(kPreamble) + kGroupOnlySrc);
    EXPECT_EQ(r, 0) << "r=" << r << " (-1 refused, 100+i wrong at row i)";
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

// xpu-kernel-adaptor 4.2.1.3, and it took three answers to get right. The
// accumulator reads as workgroup-uniform -- nothing tid-derived reaches it --
// but it is fed by a WAVE RESULT, so each wave's copy differs and it is per
// work-item in fact. Sharing one slot across the block gave, in order: 0,
// because every work-item re-read the same start value, every iteration but
// the last was dead and LLVM deleted the region's vector body (caught as a
// refusal by the gate's vanished-wave-op check); then 2048 for 32, once the
// loop began to scaffold and the slot accumulated across all 64 work-items.
// A local read-modify-written with a wave result now gets a CONTEXT ARRAY, so
// each work-item accumulates its own and the answer is trips * W.
TEST(XpuCpuFissionUniformize, aWaveFedAccumulatorIsPerWorkItem) {
    const int r = runOnCpu(std::string(kPreamble) + kUniformReduceSrc);
    EXPECT_EQ(r, 0)
        << "r=" << r << " (-1 refused, 100+i wrong at lane i; the wave sums W "
           "ones per trip and each work-item keeps its own total)";
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
