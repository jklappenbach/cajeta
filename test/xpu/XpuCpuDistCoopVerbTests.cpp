//
// Unit 4A.7.2 — the distributed software coop tile's WORD-FED and EPILOGUE
// verbs on CPU. The loop-exposure spike (XpuCpuDistTileSpikeTests) proved the
// distributed tile vectorizes and computes a plain f32 matmul correctly; this
// file pins the verbs the six CPU-skipped k-quant kernels actually need:
//
//   4A.7.2.2  fromWords  — feed an int8 B operand straight from packed words,
//                          lane l's four words ARE its column l (a direct fill).
//   4A.7.2.3  the epilogue verbs (scaledAccumInto / rank1Accum / ... ) resolve
//                          their per-lane column factor against the wave.
//
// Every test forces the distributed tile through CAJETA_GPU_COOPMATRIX_DIST=on
// (the per-kernel wave width is derived from the tile shape — 4A.7.2.1 — so no
// width env is set). Correctness against an in-kernel reference is the make-or-
// break: a shared-lane bug or a wrong extension produces a wrong product.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // setenv/unsetenv
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

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

// ---- 4A.7.2.2: fromWords feeds the int8 B operand ---------------------- //
//
// A[r][k] = r+1 (loaded), B[k][c] = c+1 (fed by fromWords). Lane l owns column
// c = l (block 16, wave 16), and every K-value in its column equals c+1, so all
// four words are (c+1) replicated into each byte. C[r][c] = sum_k A*B =
// 16*(r+1)*(c+1) in int32 — distinctive per cell (64..4096). The A operand is
// loaded and the accumulator/mma path is the spike's proven route, so a wrong
// cell here is the fromWords fill or the int8 extension.
const char* kFromWordsSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void mm(KernelBuffer<int32> y, KernelBuffer<int8> a) {
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc;
        mc.splat(0);
        ma.load(a, 0, 0, 16);
        int32 c = (int32) (KernelThread.x() % 16);
        int32 v = c + 1;
        int32 w = v | (v << 8) | (v << 16) | (v << 24);
        mb.fromWords(w, w, w, w);
        mc.mma(ma, mb);
        mc.store(y, 0, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[256];
        int32[] hy = heap int32[256];
        int32 r = 0;
        while (r < 16) {
            int32 k = 0;
            while (k < 16) {
                ha[r * 16 + k] = (int8) (r + 1);   // A[r][k] = r+1
                hy[r * 16 + k] = -1;               // sentinel
                k = k + 1;
            }
            r = r + 1;
        }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);
        KernelBuffer<int32> y = heap KernelBuffer<int32>(256);
        a.upload(ha);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [16])(y, a);
        s.sync();
        y.download(hy);
        if (hy[0] == -1) { return -1; }            // refused/skipped
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                int32 want = 16 * (rr + 1) * (cc + 1);
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

// The distributed int8 GEMM with B fed from words is correct. -1 = the kernel
// was refused (fromWords still disqualifies distribution), 1000+cell = a wrong
// product (fill or extension bug).
TEST(XpuCpuDistCoopVerb, fromWordsFeedsInt8BOperand) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kFromWordsSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "distributed int8 fromWords GEMM wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell, -2 = no compile)";
}

// ---- 4A.7.2.3: the epilogue verbs resolve their column factor per lane -- //
//
// A[r][k]=1, B[k][c]=1 (both loaded) -> mc[r][c] = sum_k 1 = 16 (uniform). The
// epilogue introduces the per-cell variation and is what we check. Row/column
// factors ride kernel buffers (not Shared), so these kernels take the plain
// single-work-item-loop path -- the barrier-fission path's marker handling is
// pinned separately. facc[r][c] = (r+1)(c+1)*16 in both tests, distinctive per
// cell (16..4096).
//
// ofLane is the point: the replicated tile REFUSES it (no wave to hold the
// other columns' factors); the distributed lane IS its column, so the factor is
// just its own value.

// Test A -- the Deq path: scaledAccumInto (single) with an ofLane column factor.
const char* kEpiOfLaneSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.WaveVector;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y, KernelBuffer<int8> a,
                          KernelBuffer<int8> b, KernelBuffer<float32> rowF) {
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc;
        CooperativeMatrix<float32,16,16,2> facc;
        mc.splat(0);
        facc.splat(0.0f);
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
        mc.mma(ma, mb);
        int32 col = (int32) (KernelThread.x() % 16);
        float32 cf = (float32) (col + 1);
        mc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cf));
        facc.store(y, 0, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[256];
        int8[] hb = heap int8[256];
        float32[] hrf = heap float32[16];
        float32[] hy = heap float32[256];
        int32 r = 0;
        while (r < 16) {
            hrf[r] = (float32) (r + 1);
            int32 k = 0;
            while (k < 16) {
                ha[r * 16 + k] = (int8) 1;
                hb[r * 16 + k] = (int8) 1;
                hy[r * 16 + k] = -1.0f;
                k = k + 1;
            }
            r = r + 1;
        }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);
        KernelBuffer<int8> b = heap KernelBuffer<int8>(256);
        KernelBuffer<float32> rowF = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(ha);
        b.upload(hb);
        rowF.upload(hrf);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [16])(y, a, b, rowF);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = (float32) (16 * (rr + 1) * (cc + 1));
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

// Test B -- the Id path: scaledAccumInto2 (dual) with ofSlice column factors.
// cf panel[c]=c+1, cg panel = 0 so the second term vanishes; same expected cell.
const char* kEpiOfSliceSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.WaveVector;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y, KernelBuffer<int8> a,
                          KernelBuffer<int8> b, KernelBuffer<float32> rowF,
                          KernelBuffer<float32> rowG, KernelBuffer<float32> cfp,
                          KernelBuffer<float32> cgp) {
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc;
        CooperativeMatrix<float32,16,16,2> facc;
        mc.splat(0);
        facc.splat(0.0f);
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
        mc.mma(ma, mb);
        mc.scaledAccumInto2(facc, rowF, WaveVector.ofSlice(cfp, 0, 1),
                            rowG, WaveVector.ofSlice(cgp, 0, 1));
        facc.store(y, 0, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[256];
        int8[] hb = heap int8[256];
        float32[] hrf = heap float32[16];
        float32[] hrg = heap float32[16];
        float32[] hcf = heap float32[16];
        float32[] hcg = heap float32[16];
        float32[] hy = heap float32[256];
        int32 r = 0;
        while (r < 16) {
            hrf[r] = (float32) (r + 1);
            hrg[r] = 0.0f;
            hcf[r] = (float32) (r + 1);
            hcg[r] = 0.0f;
            int32 k = 0;
            while (k < 16) {
                ha[r * 16 + k] = (int8) 1;
                hb[r * 16 + k] = (int8) 1;
                hy[r * 16 + k] = -1.0f;
                k = k + 1;
            }
            r = r + 1;
        }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);
        KernelBuffer<int8> b = heap KernelBuffer<int8>(256);
        KernelBuffer<float32> rowF = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> rowG = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> cfp = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> cgp = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(ha);
        b.upload(hb);
        rowF.upload(hrf);
        rowG.upload(hrg);
        cfp.upload(hcf);
        cgp.upload(hcg);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [16])(y, a, b, rowF, rowG, cfp, cgp);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = (float32) (16 * (rr + 1) * (cc + 1));
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, scaledAccumIntoResolvesOfLanePerColumn) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kEpiOfLaneSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "distributed ofLane epilogue wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell, -2 = no compile)";
}

TEST(XpuCpuDistCoopVerb, scaledAccumInto2ResolvesOfSlicePerColumn) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kEpiOfSliceSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "distributed ofSlice epilogue wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell, -2 = no compile)";
}

// ---- barrier + distributed coop: the forced width survives fission ----- //
//
// The six real kernels stage a Shared panel behind Barrier.workgroup() and then
// run the coop mma + epilogue, so they take the CPU BARRIER-FISSION path -- and
// there the kernel body is cloned into the wrapper and the kernel erased before
// the wave width is read. The coop-wavew marker must be carried onto the wrapper
// or the fission regions fall back to the host width and the distributed tile
// (which assumed the cooperative width) computes a WRONG product. This is that
// path in miniature: rowF filled cooperatively through a barrier, then a
// distributed int8 GEMM + ofLane epilogue. facc[r][c] = (r+1)(c+1)*16.
const char* kBarrierDistSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.WaveVector;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.Barrier;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y,
                          KernelBuffer<int8> a, KernelBuffer<int8> b) {
        Shared<float32> rowF = shared float32[16];
        uint32 tid = KernelThread.x();
        rowF[tid] = (float32) (tid + 1);
        Barrier.workgroup();
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc;
        CooperativeMatrix<float32,16,16,2> facc;
        mc.splat(0);
        facc.splat(0.0f);
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
        mc.mma(ma, mb);
        int32 col = (int32) (tid % 16);
        float32 cf = (float32) (col + 1);
        mc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cf));
        facc.store(y, 0, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[256];
        int8[] hb = heap int8[256];
        float32[] hy = heap float32[256];
        int32 r = 0;
        while (r < 16) {
            int32 k = 0;
            while (k < 16) {
                ha[r * 16 + k] = (int8) 1;
                hb[r * 16 + k] = (int8) 1;
                hy[r * 16 + k] = -1.0f;
                k = k + 1;
            }
            r = r + 1;
        }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);
        KernelBuffer<int8> b = heap KernelBuffer<int8>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(ha);
        b.upload(hb);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [16])(y, a, b);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = (float32) (16 * (rr + 1) * (cc + 1));
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, distributedTileForcedWidthSurvivesBarrierFission) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kBarrierDistSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "distributed coop through barrier fission wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell, -2 = no compile)";
}

// ---- 4A.7.2.4: distribution is the DEFAULT for kernels that need it ---- //
//
// A kernel using fromWords or a WaveVector.ofLane epilogue factor has NO
// replicated lowering, so on cpu it MUST take the distributed tile or be
// skipped. After 4A.7.2.4 that happens automatically, with NO env: if the gate
// did NOT fire, fromWords/ofLane would hit the replicated tile, be unsupported,
// and the kernel would be skipped -- the sentinel would survive and run() would
// return -1. run()==0 with no env is therefore proof the kernel auto-distributed.
TEST(XpuCpuDistCoopVerb, fromWordsAutoDistributesWithoutEnv) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");     // no opt-in: needed => distributed
    int r = runOnCpu(kFromWordsSrc);
    EXPECT_EQ(r, 0)
        << "fromWords kernel did not auto-distribute; r=" << r
        << " (-1 = skipped == not distributed, 1000+cell = wrong)";
}

TEST(XpuCpuDistCoopVerb, ofLaneAutoDistributesWithoutEnv) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    int r = runOnCpu(kEpiOfLaneSrc);
    EXPECT_EQ(r, 0)
        << "ofLane epilogue kernel did not auto-distribute; r=" << r
        << " (-1 = skipped == not distributed, 1000+cell = wrong)";
}

}  // namespace
