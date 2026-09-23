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
        mm.launch(s, grid: [1], block: [32])(y, a);
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
        mm.launch(s, grid: [1], block: [32])(y, a, b, rowF);
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
        mm.launch(s, grid: [1], block: [32])(y, a, b, rowF, rowG, cfp, cgp);
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
        // 32-lane wave, one 16-row tile: lanes 0-15 stage the 16 row factors
        // (the second half of the wave, lanes 16-31, are group 1 of the G=2
        // tile and must not write past rowF[15]).
        if (tid < 16) { rowF[tid] = (float32) (tid + 1); }
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
        mm.launch(s, grid: [1], block: [32])(y, a, b);
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

// ---- PROBE: a column-major B load on the distributed tile (G=2) --------- //
//
// The real q6kWmmaDeqMw4 loads its B operand COLUMN-MAJOR (mb.load(deq, off, 1,
// 16)); every other test here loads row-major (layout 0), so the distributed
// load's column-major path at wave=32 / G=2 was never exercised. This isolates
// it: A[r][k]=r+1 (row-major), B[k][c]=c+1 stored column-major so element (k,c)
// lives at b[c*16+k]. C[r][c] = sum_k (r+1)(c+1) = 16(r+1)(c+1), distinctive per
// cell. A wrong cell means the distributed column-major load (distBufIdx's cm
// branch) is the deqMw4 bug; a pass rules it out and points at the tiling.
const char* kColMajorBSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y,
                          KernelBuffer<float32> a, KernelBuffer<float32> b) {
        CooperativeMatrix<float32,16,16,0> ma;
        CooperativeMatrix<float32,16,16,1> mb;
        CooperativeMatrix<float32,16,16,2> mc;
        mc.splat(0.0f);
        ma.load(a, 0, 0, 16);   // A row-major
        mb.load(b, 0, 1, 16);   // B COLUMN-MAJOR (layout 1) -- the probe
        mc.mma(ma, mb);
        mc.store(y, 0, 0, 16);
    }
    public static int32 run() {
        float32[] ha = heap float32[256];
        float32[] hb = heap float32[256];
        float32[] hy = heap float32[256];
        int32 r = 0;
        while (r < 16) {
            int32 c = 0;
            while (c < 16) {
                ha[r * 16 + c] = (float32) (r + 1);   // A[r][k]=r+1 (row-major)
                hb[c * 16 + r] = (float32) (c + 1);   // B[k=r][c]=c+1 (col-major)
                hy[r * 16 + c] = -1.0f;
                c = c + 1;
            }
            r = r + 1;
        }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> b = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(ha);
        b.upload(hb);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [32])(y, a, b);
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

TEST(XpuCpuDistCoopVerb, columnMajorBLoadIsCorrectAtG2) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kColMajorBSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "distributed column-major B load wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell, -2 = no compile)";
}

// ---- PROBE: two coop waves in one block, distinct tiles (multi-wave) ---- //
//
// The real deqMw4 is block:[256] = 8 waves, each wave (wid=tid/32) computing a
// DIFFERENT output row-block from its own weights. Every test above is a single
// 32-lane wave. This isolates the multi-wave case: block:[64] = 2 waves, each
// loading its own B (offset wid*256) and storing to its own output (offset
// wid*256), against a shared A. C_wid[r][c] = 16*(r+1)*((c+1)+wid*100), distinct
// per wave. A wrong wave-1 cell (or wave 1 clobbering wave 0) means multiple
// coop waves in one CPU block do not coexist; both correct rules multi-wave out.
const char* kTwoWaveSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y,
                          KernelBuffer<float32> a, KernelBuffer<float32> b) {
        uint32 wid = KernelThread.x() / 32;
        CooperativeMatrix<float32,16,16,0> ma;
        CooperativeMatrix<float32,16,16,1> mb;
        CooperativeMatrix<float32,16,16,2> mc;
        mc.splat(0.0f);
        ma.load(a, 0, 0, 16);              // A shared across waves
        mb.load(b, wid * 256, 0, 16);      // B per wave
        mc.mma(ma, mb);
        mc.store(y, wid * 256, 0, 16);     // output per wave
    }
    public static int32 run() {
        float32[] ha = heap float32[256];
        float32[] hb = heap float32[512];
        float32[] hy = heap float32[512];
        int32 r = 0;
        while (r < 16) {
            int32 c = 0;
            while (c < 16) {
                ha[r * 16 + c] = (float32) (r + 1);              // A[r][k]=r+1
                hb[r * 16 + c] = (float32) (c + 1);              // wave0 B
                hb[256 + r * 16 + c] = (float32) ((c + 1) + 100);// wave1 B
                hy[r * 16 + c] = -1.0f;
                hy[256 + r * 16 + c] = -1.0f;
                c = c + 1;
            }
            r = r + 1;
        }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> b = heap KernelBuffer<float32>(512);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(512);
        a.upload(ha);
        b.upload(hb);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [64])(y, a, b);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 w = 0;
        while (w < 2) {
            int32 rr = 0;
            while (rr < 16) {
                int32 cc = 0;
                while (cc < 16) {
                    float32 want = (float32) (16 * (rr + 1) * ((cc + 1) + w * 100));
                    if (hy[w * 256 + rr * 16 + cc] != want) {
                        return 10000 * (w + 1) + rr * 16 + cc;
                    }
                    cc = cc + 1;
                }
                rr = rr + 1;
            }
            w = w + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, twoCoopWavesInOneBlockAreDistinct) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kTwoWaveSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "two coop waves in one block wrong; r=" << r
        << " (-1 = refused, 10000*(wave+1)+cell = first wrong cell)";
}

// ---- PROBE: facc accumulated across a loop of epilogue calls ----------- //
//
// deqMw4 splats facc ONCE, then loops { mc.splat; mc.mma; scaledAccumInto(facc,
// rowF, ofLane(cf)) }, so facc accumulates the epilogue term every iteration.
// Every epilogue test above makes exactly one call. This isolates the loop:
// same inputs each of 3 iterations, so facc must be 3x the single-call result.
// A[r][k]=1,B[k][c]=1 -> mc=16; rowF[r]=r+1, cf(c)=c+1 -> one term =
// 16*(r+1)*(c+1); three -> 48*(r+1)*(c+1). A wrong cell means facc does not
// persist/accumulate across calls (a re-splat, stale read, or double-add).
const char* kLoopAccumSrc = R"CJ(
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
        facc.splat(0.0f);
        int32 col = (int32) (KernelThread.x() % 16);
        float32 cf = (float32) (col + 1);
        int32 it = 0;
        while (it < 3) {
            mc.splat(0);
            ma.load(a, 0, 0, 16);
            mb.load(b, 0, 0, 16);
            mc.mma(ma, mb);
            mc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cf));
            it = it + 1;
        }
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
        mm.launch(s, grid: [1], block: [32])(y, a, b, rowF);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = (float32) (48 * (rr + 1) * (cc + 1));
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, faccAccumulatesAcrossEpilogueLoop) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kLoopAccumSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "facc loop accumulation wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell; expected 3x term)";
}

// ---- PROBE: a barrier INSIDE the accumulation loop (fission x loop) ----- //
//
// deqMw4's barrier lives inside its b-loop: { stage Shared; barrier; mma +
// scaledAccumInto(facc,...); barrier } repeated, facc accumulating. The
// straight-line barrier test and the no-barrier loop test both pass; this is
// their untested product -- barrier fission of a LOOP body wrapped around the
// coop tile. Same math as the loop probe (3 iterations -> 48*(r+1)*(c+1)), but
// the row factor is staged into Shared behind a barrier each iteration. A wrong
// cell means the fissioned loop corrupts the coop tile's carried state.
const char* kBarrierLoopSrc = R"CJ(
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
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc;
        CooperativeMatrix<float32,16,16,2> facc;
        facc.splat(0.0f);
        int32 col = (int32) (tid % 16);
        float32 cf = (float32) (col + 1);
        int32 bb = 0;
        while (bb < 3) {
            if (tid < 16) { rowF[tid] = (float32) (tid + 1); }
            Barrier.workgroup();
            mc.splat(0);
            ma.load(a, 0, 0, 16);
            mb.load(b, 0, 0, 16);
            mc.mma(ma, mb);
            mc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cf));
            Barrier.workgroup();
            bb = bb + 1;
        }
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
        mm.launch(s, grid: [1], block: [32])(y, a, b);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = (float32) (48 * (rr + 1) * (cc + 1));
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, coopSurvivesBarrierInsideLoop) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kBarrierLoopSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "coop through barrier-in-loop wrong; r=" << r
        << " (-1 = refused, 1000+cell = first wrong cell; expected 3x term)";
}


// Multi-accumulator distributed coop: the shape a native Q6-style deq-matmul
// kernel drives (four int32 accumulators mc0-3 feeding four f32 accumulators
// facc0-3, `ma` reloaded once per accumulator, ONE `mb` reused across all four
// mma's) run through the distributed CPU tile inside a barrier-loop. The single-
// accumulator tests above (coopSurvivesBarrierInsideLoop) never exercise more
// than one per-lane accumulator alloca at a time; this pins that several
// distinct accumulators keep their own per-lane slots and row->slot mapping.
// Block n is filled with (n+1) so mc_n = 16*(n+1), distinct per accumulator;
// after 3 loop iters facc_n[r][c] = 3 * 16*(n+1) * rowF(r) * cf(c)
//   = 48*(n+1)*(r+1)*(c+1). Each facc_n stores to its OWN 256-element region
// (offset n*256, a flat element offset -- NOT a row-block index), so any
// cross-accumulator overlap shows as a wrong cell: run() returns
// 1000 + n*256 + r*16 + c at the first mismatch.
const char* kMultiAccumSrc = R"CJ(
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
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc0;
        CooperativeMatrix<int32,16,16,2> mc1;
        CooperativeMatrix<int32,16,16,2> mc2;
        CooperativeMatrix<int32,16,16,2> mc3;
        CooperativeMatrix<float32,16,16,2> facc0;
        CooperativeMatrix<float32,16,16,2> facc1;
        CooperativeMatrix<float32,16,16,2> facc2;
        CooperativeMatrix<float32,16,16,2> facc3;
        facc0.splat(0.0f);
        facc1.splat(0.0f);
        facc2.splat(0.0f);
        facc3.splat(0.0f);
        int32 col = (int32) (tid % 16);
        float32 cf = (float32) (col + 1);
        int32 bb = 0;
        while (bb < 3) {
            if (tid < 16) { rowF[tid] = (float32) (tid + 1); }
            Barrier.workgroup();
            mb.load(b, 0, 0, 16);
            mc0.splat(0);
            ma.load(a, 0, 0, 16);
            mc0.mma(ma, mb);
            mc0.scaledAccumInto(facc0, rowF, WaveVector.ofLane(cf));
            mc1.splat(0);
            ma.load(a, 256, 0, 16);
            mc1.mma(ma, mb);
            mc1.scaledAccumInto(facc1, rowF, WaveVector.ofLane(cf));
            mc2.splat(0);
            ma.load(a, 512, 0, 16);
            mc2.mma(ma, mb);
            mc2.scaledAccumInto(facc2, rowF, WaveVector.ofLane(cf));
            mc3.splat(0);
            ma.load(a, 768, 0, 16);
            mc3.mma(ma, mb);
            mc3.scaledAccumInto(facc3, rowF, WaveVector.ofLane(cf));
            Barrier.workgroup();
            bb = bb + 1;
        }
        facc0.store(y, 0, 0, 16);
        facc1.store(y, 256, 0, 16);
        facc2.store(y, 512, 0, 16);
        facc3.store(y, 768, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[1024];
        int8[] hb = heap int8[256];
        float32[] hy = heap float32[1024];
        int32 n = 0;
        while (n < 4) {
            int32 r = 0;
            while (r < 16) {
                int32 k = 0;
                while (k < 16) {
                    ha[(n * 16 + r) * 16 + k] = (int8) (n + 1);
                    k = k + 1;
                }
                r = r + 1;
            }
            n = n + 1;
        }
        int32 i = 0;
        while (i < 256) { hb[i] = (int8) 1; i = i + 1; }
        int32 j = 0;
        while (j < 1024) { hy[j] = -1.0f; j = j + 1; }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(1024);
        KernelBuffer<int8> b = heap KernelBuffer<int8>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(1024);
        a.upload(ha);
        b.upload(hb);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [32])(y, a, b);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 nn = 0;
        while (nn < 4) {
            int32 rr = 0;
            while (rr < 16) {
                int32 cc = 0;
                while (cc < 16) {
                    float32 want = (float32) (48 * (nn + 1) * (rr + 1) * (cc + 1));
                    if (hy[nn * 256 + rr * 16 + cc] != want) {
                        return 1000 + nn * 256 + rr * 16 + cc;
                    }
                    cc = cc + 1;
                }
                rr = rr + 1;
            }
            nn = nn + 1;
        }
        return 0;
    }
}
)CJ";

// Four accumulators, `ma` reloaded per accumulator, one shared `mb` reused
// across all four mma's, inside a barrier-loop. A wrong facc_n names its block
// in the return value; r==0 means every accumulator kept its own per-lane slots.
TEST(XpuCpuDistCoopVerb, multiAccumReuseMbAcrossBarrierLoop) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kMultiAccumSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "multi-accumulator coop wrong; r=" << r
        << " (-1 refused, 1000+n*256+r*16+c = first wrong cell; block n = (r-1000)/256)";
}

// ---- PROBE: an ofLane column factor that VARIES per epilogue iteration ---- //
//
// Every ofLane epilogue test above feeds a LOOP-INVARIANT cf (cf = col+1,
// computed once). deqMw4 is the one kernel that feeds ofLane a value that
// changes every call: cfv = dv * scj, a different scale for each of the 16
// sub-blocks j, accumulated into facc each time. deqMw8 (which is CORRECT on
// the CPU distributed tile) never uses ofLane -- its column factors come from
// ofSlice. So "ofLane with a per-call-varying factor" is the untested seam
// that distinguishes deqMw4 (wrong on CPU distributed) from everything green.
//
// Single accumulator, so the store offset is a plain 0 -- no multi-accumulator
// overlap. Same mc/rowF as faccAccumulatesAcrossEpilogueLoop (its CONTROL: cf
// constant -> 48*(r+1)*(c+1), passes), but here cf = (col+1)*(it+1), so:
//   facc[r][c] = sum_{it=0,1,2} 16*(r+1) * ((c+1)*(it+1))
//              = 16*(r+1)*(c+1) * (1+2+3) = 96*(r+1)*(c+1).
// If ofLane is bound to the FIRST iteration's cf (a hoist/stale-read bug that a
// constant-cf test cannot see), each cell is 3*16*(r+1)*(c+1)*1 = 48*(r+1)*(c+1)
// instead -- exactly half, and the return value names the first wrong cell.
const char* kVaryingOfLaneSrc = R"CJ(
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
        facc.splat(0.0f);
        int32 col = (int32) (KernelThread.x() % 16);
        int32 it = 0;
        while (it < 3) {
            float32 cf = (float32) ((col + 1) * (it + 1));   // VARIES per call
            mc.splat(0);
            ma.load(a, 0, 0, 16);
            mb.load(b, 0, 0, 16);
            mc.mma(ma, mb);
            mc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cf));
            it = it + 1;
        }
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
        mm.launch(s, grid: [1], block: [32])(y, a, b, rowF);
        s.sync();
        y.download(hy);
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = (float32) (96 * (rr + 1) * (cc + 1));
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, varyingOfLaneCfAcrossEpilogueLoop) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kVaryingOfLaneSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "varying ofLane cf wrong; r=" << r
        << " (-1 refused, 1000+cell = first wrong cell; want 96*(r+1)*(c+1),"
           " 48*... means ofLane bound to the first iteration's cf)";
}

// ---- PROBE: ofLane column factor from a PER-LANE MEMORY LOAD ---------- //
//
// The one thing deqMw4's cfv has that varyingOfLaneCfAcrossEpilogueLoop's cf
// does NOT: it is not pure arithmetic on the lane index, it is a value LOADED
// from memory at a per-lane offset (scaleAtDev(packed, myRo/2+b) and
// packed[ro+192+j], both keyed on lane16). The pure-arithmetic ofLane cf
// distributes per-column correctly (that probe passes); this asks whether a
// per-lane MEMORY-LOADED ofLane cf still distributes, or collapses to one
// lane's value across the 16-wide tile (period-16 — deqMw4's CPU fingerprint).
// One wave, one accumulator, so a failure isolates the memory-load path alone.
const char* kMemLoadOfLaneSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.WaveVector;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y, KernelBuffer<int8> a,
                          KernelBuffer<int8> b, KernelBuffer<float32> rowF,
                          KernelBuffer<float32> cfBuf) {
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc;
        CooperativeMatrix<float32,16,16,2> facc;
        facc.splat(0.0f);
        int32 col = (int32) (KernelThread.x() % 16);
        float32 cf = cfBuf[col];        // PER-LANE MEMORY LOAD, not arithmetic
        mc.splat(0);
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
        mc.mma(ma, mb);
        mc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cf));
        facc.store(y, 0, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[256];
        int8[] hb = heap int8[256];
        float32[] hrf = heap float32[16];
        float32[] hcf = heap float32[16];
        float32[] hy = heap float32[256];
        int32 r = 0;
        while (r < 16) {
            hrf[r] = (float32) (r + 1);
            hcf[r] = (float32) (r + 1);
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
        KernelBuffer<float32> cfBuf = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(ha);
        b.upload(hb);
        rowF.upload(hrf);
        cfBuf.upload(hcf);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [32])(y, a, b, rowF, cfBuf);
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

TEST(XpuCpuDistCoopVerb, memLoadedOfLaneCfDistributesPerColumn) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kMemLoadOfLaneSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "memory-loaded ofLane cf wrong; r=" << r
        << " (-1 refused, 1000+cell = first wrong cell; want 16*(r+1)*(c+1)."
           " A whole 16-wide tile sharing one column's factor is the period-16"
           " deqMw4 CPU fingerprint — the memory-load path does not distribute.)";
}

// ---- PROBE: one COLUMN-MAJOR mb reused across FOUR mmas ---------------- //
//
// deqMw4's inner pattern that no test covers: mb.load(deq, off, 1, 16) ONCE
// (column-major, layout 1), then four mc_n.mma(ma_n, mb) reusing that single
// mb with ma reloaded between. columnMajorBLoadIsCorrectAtG2 is column-major
// but ONE mma; multiAccumReuseMbAcrossBarrierLoop reuses across four but
// ROW-major. This is the intersection. B is asymmetric (B[k][c]=2k+c, so
// column-major differs from row-major) and A_n[r][k]=n+1 (distinct per mma).
//   mc_n[r][c] = (n+1) * sum_k (2k+c) = (n+1) * (240 + 16c),  distinct per (n,c).
// A wrong mc_n (n>=1) means reusing a column-major mb across mmas corrupts it
// (a stale/re-read/aliasing bug the row-major reuse test cannot see).
const char* kCmReuseSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void mm(KernelBuffer<int32> y, KernelBuffer<int8> a,
                          KernelBuffer<int8> b) {
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb;
        CooperativeMatrix<int32,16,16,2> mc0;
        CooperativeMatrix<int32,16,16,2> mc1;
        CooperativeMatrix<int32,16,16,2> mc2;
        CooperativeMatrix<int32,16,16,2> mc3;
        mc0.splat(0);
        mc1.splat(0);
        mc2.splat(0);
        mc3.splat(0);
        mb.load(b, 0, 1, 16);              // ONE column-major B, reused
        ma.load(a, 0, 0, 16);   mc0.mma(ma, mb);
        ma.load(a, 256, 0, 16); mc1.mma(ma, mb);
        ma.load(a, 512, 0, 16); mc2.mma(ma, mb);
        ma.load(a, 768, 0, 16); mc3.mma(ma, mb);
        mc0.store(y, 0, 0, 16);
        mc1.store(y, 256, 0, 16);
        mc2.store(y, 512, 0, 16);
        mc3.store(y, 768, 0, 16);
    }
    public static int32 run() {
        int8[] ha = heap int8[1024];
        int8[] hb = heap int8[256];
        int32[] hy = heap int32[1024];
        int32 n = 0;
        while (n < 4) {
            int32 r = 0;
            while (r < 16) {
                int32 k = 0;
                while (k < 16) { ha[(n * 16 + r) * 16 + k] = (int8) (n + 1); k = k + 1; }
                r = r + 1;
            }
            n = n + 1;
        }
        // column-major storage: b[c*16 + k] = 2k + c  ->  load(layout 1) gives B[k][c]=2k+c
        int32 c = 0;
        while (c < 16) {
            int32 k = 0;
            while (k < 16) { hb[c * 16 + k] = (int8) (2 * k + c); k = k + 1; }
            c = c + 1;
        }
        int32 j = 0;
        while (j < 1024) { hy[j] = -1; j = j + 1; }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(1024);
        KernelBuffer<int8> b = heap KernelBuffer<int8>(256);
        KernelBuffer<int32> y = heap KernelBuffer<int32>(1024);
        a.upload(ha);
        b.upload(hb);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [32])(y, a, b);
        s.sync();
        y.download(hy);
        if (hy[0] == -1) { return -1; }
        int32 nn = 0;
        while (nn < 4) {
            int32 rr = 0;
            while (rr < 16) {
                int32 cc = 0;
                while (cc < 16) {
                    int32 want = (nn + 1) * (240 + 16 * cc);
                    if (hy[nn * 256 + rr * 16 + cc] != want) {
                        return 1000 + nn * 256 + rr * 16 + cc;
                    }
                    cc = cc + 1;
                }
                rr = rr + 1;
            }
            nn = nn + 1;
        }
        return 0;
    }
}
)CJ";

TEST(XpuCpuDistCoopVerb, columnMajorMbReusedAcrossFourMmas) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu(kCmReuseSrc);
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "column-major mb reused across 4 mmas wrong; r=" << r
        << " (-1 refused, 1000+n*256+r*16+c = first wrong cell; block n=(r-1000)/256)";
}
}  // namespace
