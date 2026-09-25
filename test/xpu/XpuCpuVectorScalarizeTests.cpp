//
// xpu-kernel-adaptor 4.2.1 — a cajeta `Vector<T,N>` value must not stop the
// cpu backend's work-item loop from widening at the wave width.
//
// MEASURED CAUSE. LoopVectorizationLegality::canVectorizeInstrs refuses any
// instruction whose result type is not a valid vector ELEMENT type, and a
// fixed vector type is not one: `<4 x float>` cannot be widened to `<8 x
// <4 x float>>`. So a single `vload<4>` living in the same fission region as
// a wave op refuses the whole work-item loop with "instruction return type
// cannot be vectorized", the wave op is left scalar, and CpuRegistration's
// loud left-scalar gate refuses the kernel rather than run a wave op with
// width-1 semantics. That is why the attnFlashDecode family and the q4k
// quant math sat in cajeta-llm's census as DECLINED with no loop shape to
// blame.
//
// THE FIX these tests pin. vectorizeFunction now takes the wave-kernel
// wrapper through a Scalarizer plus InstCombine prefix (twice: the first
// round leaves the `bitcast i32 to <4 x i8>` chains asWords and asBytes
// emit, and InstCombine must fold them before the second round can finish),
// and an EarlyCSE plus InstCombine cleanup after mem2reg, before LoopRotate.
// The pass ORDER is measured, not reasoned: SimplifyCFG in that slot instead
// costs the nested-loop shape a whole region, which is what
// uniformLoopWaveKernelsStillWiden is here to catch.
//
// SCOPE. Only the cpu VECTOR-TYPE cause is addressed here. The other, larger
// cause is a loop whose exit condition depends on the work-item id (the
// lane-strided `for (e = lane; e < N; e += waveWidth)` idiom), which keeps
// the work-item loop from being innermost at all. That one needs a fission
// transform, is tracked under xpu-kernel-adaptor 4.2.1, and
// aLaneStridedLoopWaveKernelIsStillRefused pins that it is still refused.
//
// Every assertion checks the VALUE as well as the registration: a kernel that
// registered but ran the wave op at width 1 returns the single work-item's own
// number instead of the wave's, so registration alone would not catch it.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // setenv/unsetenv
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile `src` for the cpu backend and call its `run`. A kernel the gate
// refuses has no cpu device code, so its launch raises and `run` answers -1.
int runOnCpu(const char* src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

// ---- (a) one vload feeding one wave reduce, no loop at all ------------- //
//
// The minimal shape of the cause. Each work-item loads four 1.0f and sums
// them to 4.0f, so a wave reduce answers 4*W and a width-1 reduce answers 4.
// `run` probes W with a kernel that has no vector value in it, so the
// expectation is the true host width rather than a hardcoded 8.
const char* kVloadWaveSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    public static void widthk(KernelBuffer<uint32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = Wave.width();
    }
    @Kernel
    public static void vk(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 t = KernelThread.globalIdX();
        Vector<float32,4> v = in.vload<4>((int64) (t * 4));
        float32 s = v[0] + v[1] + v[2] + v[3];
        out[t] = Wave.reduceSumF32(s);
    }
    public static int32 run() {
        uint32 n = 64;
        uint32[] hw = heap uint32[n];
        KernelBuffer<uint32> bw = heap KernelBuffer<uint32>(n);
        KernelStream s #= KernelStream.current();
        widthk.launch(s, grid: [1], block: [64])(bw);
        s.sync();
        bw.download(hw);
        uint32 w = hw[0];
        if (w < 2) { return -3; }

        float32[] hin = heap float32[n * 4];
        float32[] hout = heap float32[n];
        uint32 i = 0;
        while (i < n * 4) { hin[i] = 1.0f; i = i + 1; }
        i = 0;
        while (i < n) { hout[i] = -1.0f; i = i + 1; }
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(n * 4);
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bin.upload(hin);
        bout.upload(hout);
        try {
            vk.launch(s, grid: [1], block: [64])(bout, bin);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        float32 want = 4.0f * (float32) w;
        i = 0;
        while (i < n) {
            if (hout[i] != want) { return 100 + (int32) i; }
            i = i + 1;
        }
        return (int32) w;
    }
}
)CJ";

TEST(XpuCpuVectorScalarize, vloadFedWaveReduceRegistersAndWidens) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    const int r = runOnCpu(kVloadWaveSrc);
    EXPECT_GE(r, 2)
        << "a vload<4> feeding Wave.reduceSumF32 did not register and widen; r="
        << r << " (-1 = the gate refused the kernel, 100+i = it registered but "
              "lane i read " << " a wrong reduce, which is the width-1 identity)";
}

// ---- (b) the q4k quant math: vload<32>, asWords/asBytes, dotAccum ------ //
//
// The real shape from cajeta-llm's wave mat-vecs, reduced to a self-checking
// kernel. w and a hold 1 in every byte, so `& 15` leaves 1, dotAccum sums four
// 1*1 products into each of eight int32 lanes (4 per lane, 32 over the
// vector), and the wave reduce over that answers 32*W.
const char* kQuantMathWaveSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    public static void widthk(KernelBuffer<uint32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = Wave.width();
    }
    @Kernel
    public static void qk(KernelBuffer<int32> out, KernelBuffer<int32> zb,
                          KernelBuffer<int8> w, KernelBuffer<int8> a) {
        uint32 t = KernelThread.globalIdX();
        Vector<int32,8> z = zb.vload<8>(0L);
        Vector<int8,32> raw = w.vload<32>(0L);
        Vector<int32,8> words = raw.asWords();
        Vector<int8,32> back = words.asBytes();
        Vector<uint8,32> u = back.asUnsigned();
        Vector<uint8,32> m = u & 15;
        Vector<int32,8> r = m.dotAccum(a.vload<32>(0L), z);
        int32 acc = r[0] + r[1] + r[2] + r[3] + r[4] + r[5] + r[6] + r[7];
        out[t] = (int32) Wave.reduceSum((uint32) acc);
    }
    public static int32 run() {
        uint32 n = 64;
        uint32[] hw = heap uint32[n];
        KernelBuffer<uint32> bw = heap KernelBuffer<uint32>(n);
        KernelStream s #= KernelStream.current();
        widthk.launch(s, grid: [1], block: [64])(bw);
        s.sync();
        bw.download(hw);
        uint32 wv = hw[0];
        if (wv < 2) { return -3; }

        int8[] hwt = heap int8[32];
        int8[] hac = heap int8[32];
        int32[] hz = heap int32[8];
        int32[] hout = heap int32[n];
        uint32 i = 0;
        while (i < 32) { hwt[i] = (int8) 1; hac[i] = (int8) 1; i = i + 1; }
        i = 0;
        while (i < 8) { hz[i] = 0; i = i + 1; }
        i = 0;
        while (i < n) { hout[i] = -1; i = i + 1; }
        KernelBuffer<int8> bwt = heap KernelBuffer<int8>(32);
        KernelBuffer<int8> bac = heap KernelBuffer<int8>(32);
        KernelBuffer<int32> bz = heap KernelBuffer<int32>(8);
        KernelBuffer<int32> bout = heap KernelBuffer<int32>(n);
        bwt.upload(hwt);
        bac.upload(hac);
        bz.upload(hz);
        bout.upload(hout);
        try {
            qk.launch(s, grid: [1], block: [64])(bout, bz, bwt, bac);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1) { return -1; }
        int32 want = 32 * (int32) wv;
        i = 0;
        while (i < n) {
            if (hout[i] != want) { return 100 + (int32) i; }
            i = i + 1;
        }
        return (int32) wv;
    }
}
)CJ";

TEST(XpuCpuVectorScalarize, quantMathWaveKernelRegistersAndMatchesTheOracle) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    const int r = runOnCpu(kQuantMathWaveSrc);
    EXPECT_GE(r, 2)
        << "the q4k quant-math shape (vload<32>, asWords/asBytes, dotAccum) did "
           "not register and widen; r=" << r
        << " (-1 = refused, 100+i = wrong reduce at lane i)";
}

// ---- (c) the control shapes: the pass order must not cost a region ----- //
//
// Six wave kernels with NO vector value in them. Every one of them widened
// before the scalarize prefix existed, so every one must still widen after:
// this is the test that pins the pass ORDER. Putting SimplifyCFG where the
// EarlyCSE plus InstCombine cleanup goes was measured to drop the nested
// shape from four vectorized regions to three, which shows up here as gNested
// answering the per-lane count instead of the wave's.
const char* kControlShapesSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
import cajeta.lang.System;
public class M {
    @Kernel
    public static void widthk(KernelBuffer<uint32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = Wave.width();
    }
    @Kernel
    public static void aNoLoop(KernelBuffer<float32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = Wave.reduceSumF32(1.0f);
    }
    @Kernel
    public static void bUniformLoop(KernelBuffer<float32> out, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) { acc = acc + 1.0f; j = j + 1; }
        out[t] = Wave.reduceSumF32(acc);
    }
    // The reduce argument is READ PER WORK-ITEM (in[t], all ones). A reduce of
    // a workgroup-uniform constant in this position answers 0 instead of W on
    // this backend today, measured 2026-09-25 at 072d0bed and unrelated to the
    // scalarize prefix, so the control uses the shape the real kernels use.
    @Kernel
    public static void eReduceInLoop(KernelBuffer<float32> out,
                                     KernelBuffer<float32> in, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) { acc = acc + Wave.reduceSumF32(in[t]); j = j + 1; }
        out[t] = acc;
    }
    // NO fEarlyExit control here, and the omission is measured. A `break` --
    // even on a workgroup-uniform condition -- gives the loop a second exiting
    // edge, fission step 4b then declines to scaffold it, the whole kernel
    // lands in ONE region whose work-item loop encloses the while loop, and
    // LoopVectorize (innermost loops only) leaves the wave reduce scalar. The
    // remark is "cannot prove it is safe to reorder floating-point operations"
    // on the inner loop. That is a third, smaller cause of the cpu declines,
    // it is not what the scalarize prefix addresses, and pinning it as a
    // control would have pinned a refusal rather than a widening.
    // The accumulator is PER WORK-ITEM (in[t]) for the same reason
    // eReduceInLoop's argument is. A workgroup-uniform accumulator updated
    // inside NESTED scaffold loops loses the inner loop's updates on this
    // backend today (it answers 4 where 16 is right, measured 2026-09-25 at
    // 072d0bed: step 4b snapshots a uniform local per trip and the outer
    // header's snapshot rewinds the inner loop's work). Unrelated to the
    // scalarize prefix, and no real kernel accumulates a uniform there.
    @Kernel
    public static void gNested(KernelBuffer<float32> out,
                               KernelBuffer<float32> in, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        uint32 j = 0;
        while (j < n) {
            uint32 k = 0;
            while (k < n) { acc = acc + in[t]; k = k + 1; }
            j = j + 1;
        }
        out[t] = Wave.reduceSumF32(acc);
    }
    @Kernel
    public static void hInt64Iv(KernelBuffer<float32> out, uint32 n) {
        uint32 t = KernelThread.globalIdX();
        float32 acc = 0.0f;
        int64 j = 0L;
        while (j < (int64) n) { acc = acc + 1.0f; j = j + 1L; }
        out[t] = Wave.reduceSumF32(acc);
    }

    // 0 on success, else 1000*shape + 1 for a refusal / 1000*shape + 2 for a
    // wrong value, so a failure names WHICH control shape stopped widening and
    // no failure code can be mistaken for a plausible reduce total.
    public static int32 run() {
        uint32 n = 64;
        uint32 trips = 4;
        uint32[] hw = heap uint32[n];
        KernelBuffer<uint32> bw = heap KernelBuffer<uint32>(n);
        KernelStream s #= KernelStream.current();
        widthk.launch(s, grid: [1], block: [64])(bw);
        s.sync();
        bw.download(hw);
        uint32 w = hw[0];
        if (w < 2) { return -3; }
        float32 fw = (float32) w;

        float32[] h = heap float32[n];
        KernelBuffer<float32> b = heap KernelBuffer<float32>(n);

        int32 bad = 0;
        bad = checkA(b, h, n, fw, 1);
        if (bad != 0) { return bad; }
        bad = checkB(b, h, n, trips, fw, 2);
        if (bad != 0) { return bad; }
        float32[] hones = heap float32[n];
        uint32 z = 0;
        while (z < n) { hones[z] = 1.0f; z = z + 1; }
        KernelBuffer<float32> bones = heap KernelBuffer<float32>(n);
        bones.upload(hones);
        bad = checkE(b, bones, h, n, trips, fw, 3);
        if (bad != 0) { return bad; }
        bad = checkG(b, bones, h, n, trips, fw, 5);
        if (bad != 0) { return bad; }
        bad = checkH(b, h, n, trips, fw, 6);
        if (bad != 0) { return bad; }
        return 0;
    }

    public static int32 verify(float32[] h, uint32 n, float32 want, int32 tag) {
        uint32 i = 0;
        while (i < n) {
            if (h[i] != want) {
                System.stdout.println("SHAPE " + tag + " lane " + (int32) i
                    + " got " + h[i] + " want " + want);
                return 1000 * tag + 2;
            }
            i = i + 1;
        }
        return 0;
    }
    public static void arm(float32[] h, uint32 n, KernelBuffer<float32> b) {
        uint32 i = 0;
        while (i < n) { h[i] = -1.0f; i = i + 1; }
        b.upload(h);
    }
    public static int32 checkA(KernelBuffer<float32> b,
                                float32[] h, uint32 n, float32 fw, int32 tag) {
        KernelStream s #= KernelStream.current();
        arm(h, n, b);
        try { aNoLoop.launch(s, grid: [1], block: [64])(b); s.sync(); }
        catch (Exception e) { return 1000 * tag + 1; }
        b.download(h);
        return verify(h, n, fw, tag);
    }
    public static int32 checkB(KernelBuffer<float32> b,
                                float32[] h, uint32 n, uint32 trips,
                                float32 fw, int32 tag) {
        KernelStream s #= KernelStream.current();
        arm(h, n, b);
        try { bUniformLoop.launch(s, grid: [1], block: [64])(b, trips); s.sync(); }
        catch (Exception e) { return 1000 * tag + 1; }
        b.download(h);
        return verify(h, n, fw * (float32) trips, tag);
    }
    public static int32 checkE(KernelBuffer<float32> b,
                                KernelBuffer<float32> in,
                                float32[] h, uint32 n, uint32 trips,
                                float32 fw, int32 tag) {
        KernelStream s #= KernelStream.current();
        arm(h, n, b);
        try {
            eReduceInLoop.launch(s, grid: [1], block: [64])(b, in, trips);
            s.sync();
        }
        catch (Exception e) { return 1000 * tag + 1; }
        b.download(h);
        return verify(h, n, fw * (float32) trips, tag);
    }
    public static int32 checkG(KernelBuffer<float32> b,
                                KernelBuffer<float32> in,
                                float32[] h, uint32 n, uint32 trips,
                                float32 fw, int32 tag) {
        KernelStream s #= KernelStream.current();
        arm(h, n, b);
        try {
            gNested.launch(s, grid: [1], block: [64])(b, in, trips);
            s.sync();
        }
        catch (Exception e) { return 1000 * tag + 1; }
        b.download(h);
        return verify(h, n, fw * (float32) (trips * trips), tag);
    }
    public static int32 checkH(KernelBuffer<float32> b,
                                float32[] h, uint32 n, uint32 trips,
                                float32 fw, int32 tag) {
        KernelStream s #= KernelStream.current();
        arm(h, n, b);
        try { hInt64Iv.launch(s, grid: [1], block: [64])(b, trips); s.sync(); }
        catch (Exception e) { return 1000 * tag + 1; }
        b.download(h);
        return verify(h, n, fw * (float32) trips, tag);
    }
}
)CJ";

TEST(XpuCpuVectorScalarize, uniformLoopWaveKernelsStillWiden) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    const int r = runOnCpu(kControlShapesSrc);
    EXPECT_EQ(r, 0)
        << "a control wave kernel with no vector value stopped widening; r=" << r
        << " (1000*shape+1 = refused, 1000*shape+2 = wrong reduce; shapes are "
           "1 aNoLoop, 2 bUniformLoop, 3 eReduceInLoop, 5 gNested, "
           "6 hInt64Iv)";
}

// ---- (d) the gate must still FIRE -------------------------------------- //
//
// A check with no firing test reads clean when it is disabled. This kernel
// carries the OTHER cause: a loop whose exit condition depends on the work-item
// id (`e = lane; while (e < n) { ... e = e + 32; }`). Fission cannot make that
// loop workgroup-uniform scaffold, so it stays inside the work-item region and
// the work-item loop is not innermost, which LoopVectorize will not widen.
// Scalarizing the vector values does not change that, and must not: the wave
// reduce would run at width 1 and answer this work-item's own number.
//
// TRACKED: xpu-kernel-adaptor 4.2.1 — when fission learns to uniformize a
// per-work-item trip count this kernel starts registering and this test flips
// to an equality on 32*W.
const char* kLaneStridedRefusedSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class M {
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
        uint32 n = 64;
        float32[] hin = heap float32[256];
        float32[] hout = heap float32[n];
        uint32 i = 0;
        while (i < 256) { hin[i] = 1.0f; i = i + 1; }
        i = 0;
        while (i < n) { hout[i] = -1.0f; i = i + 1; }
        KernelBuffer<float32> bin = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> bout = heap KernelBuffer<float32>(n);
        bin.upload(hin);
        bout.upload(hout);
        KernelStream s #= KernelStream.current();
        try {
            lk.launch(s, grid: [1], block: [64])(bout, bin, 32);
            s.sync();
        } catch (Exception e) {
            return -1;
        }
        bout.download(hout);
        if (hout[0] == -1.0f) { return -1; }
        return 0;
    }
}
)CJ";

TEST(XpuCpuVectorScalarize, aLaneStridedLoopWaveKernelIsStillRefused) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    const int r = runOnCpu(kLaneStridedRefusedSrc);
    EXPECT_EQ(r, -1)
        << "a wave kernel whose loop trip count depends on the work-item id was "
           "REGISTERED (r=" << r
        << "); scalarizing vector values must not fool the left-scalar gate, "
           "because that loop still leaves the work-item loop non-innermost";
}

}  // namespace
