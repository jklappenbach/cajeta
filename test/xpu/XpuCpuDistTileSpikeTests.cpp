//
// Unit 4A.7 loop-exposure SPIKE. The distributed software coop tile on CPU
// walled (4A.7.2): LoopVectorize widens only the INNERMOST loop, and the
// distributed tile's work-item loop encloses the tile's inner loops (perLane,
// K), so LV took the inner K-reduction at the host width 8 and never widened
// the work-item loop to the cooperative 16 -- and the per-lane state, an array
// alloca indexed by the loop var, was shared across the SIMD lanes. A distributed
// 16x16 f32 matmul came out WRONG.
//
// The fix under test: emit the distributed tile's constant-trip inner loops
// (perLane, K) fully UNROLLED in the lowering, so the work-item loop is genuinely
// innermost and the per-lane state is constant-indexed (SROA-promotable). This
// probes whether that exposes the work-item loop to LoopVectorize at the forced
// wave width AND computes correctly.
//
// Correctness is the make-or-break, exactly as in the reversal spike: a work-item
// loop vectorized below the cooperative width with a shared per-lane alloca
// produces a WRONG matmul, so a correct 16x16 product means the loop-exposure
// worked. The CPU cooperative wave is now the universal WMMA warp of 32
// (CpuTarget::distributedCoopMatrixWaveWidth), emulated in one 32-wide SIMD
// vector and forced onto the work-item loop through the per-kernel coop-wavew
// marker; block:[32] fills it (G=2 over the 16x16 tile). The only seam here is
// CAJETA_GPU_COOPMATRIX_DIST=on, which opts the kernel into the distributed
// tile; the control runs the SAME kernel without it (the replicated tile), so a
// pass there proves the distributed path is additive, not a regression.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // setenv/unsetenv
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// A[r][k] = r+1, B[k][c] = c+1  ->  C[r][c] = sum_k (r+1)(c+1) = 16*(r+1)*(c+1),
// distinctive per cell (16..4096), so a lane-sharing bug shows as a wrong cell.
// run() returns 0 if every cell matches the in-kernel reference, 1000 + cell at
// the first mismatch, or -1 if the kernel produced nothing (skipped).
const char* kSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void mm(KernelBuffer<float32> y,
                          KernelBuffer<float32> a, KernelBuffer<float32> b) {
        CooperativeMatrix<float32,16,16,0> ma;
        CooperativeMatrix<float32,16,16,1> mb;
        CooperativeMatrix<float32,16,16,2> mc;
        mc.splat(0.0f);
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
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
                ha[r * 16 + c] = (float32) (r + 1);   // A[r][k] = r+1
                hb[r * 16 + c] = (float32) (c + 1);   // B[k][c] = c+1
                hy[r * 16 + c] = -1.0f;               // sentinel
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
        // If the kernel was refused, the sentinel survives.
        if (hy[0] == -1.0f) { return -1; }
        int32 rr = 0;
        while (rr < 16) {
            int32 cc = 0;
            while (cc < 16) {
                float32 want = 0.0f;
                int32 k = 0;
                while (k < 16) {
                    want = want + ha[rr * 16 + k] * hb[k * 16 + cc];
                    k = k + 1;
                }
                if (hy[rr * 16 + cc] != want) { return 1000 + rr * 16 + cc; }
                cc = cc + 1;
            }
            rr = rr + 1;
        }
        return 0;
    }
}
)CJ";

int runOnCpu() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSrc, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

}  // namespace

// THE SPIKE (now at the wave=32 base): the distributed tile with its inner
// loops unrolled in the lowering, the work-item loop forced to the cooperative
// 32 (the universal WMMA warp the CPU emulates in one 32-wide SIMD vector, G=2
// over the 16x16 tile). A correct 16x16 product means LoopVectorize took VF=32
// and the per-lane state promoted. If it walls, the value tells how: -1 refused,
// 1000+cell wrong.
TEST(XpuCpuDistTileSpike, distributedMatmulIsCorrectAtWave32) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");            // wave is the per-kernel 32
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);
    int r = runOnCpu();
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "distributed cpu matmul wrong; r=" << r
        << " (-1 = refused/skipped, 1000+cell = first wrong cell, -2 = no compile)";
}

// CONTROL: the replicated tile (no seams) is correct, so the distributed path
// is additive, not a regression of ordinary cpu coop kernels.
TEST(XpuCpuDistTileSpike, replicatedMatmulStillCorrect) {
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    EXPECT_EQ(runOnCpu(), 0);
}

// 4A.7.2.1: the cooperative wave width is now a per-kernel property, NOT the
// CAJETA_XPU_CPU_WAVE_WIDTH env. With only the distribution opt-in set and the
// width env UNSET, decideCoopDistribution derives waveW == Cols (16) from the
// tile shape, pins it on the kernel (prepareDistributedCoopMatrix), and
// cpuVectorWidthI32 reads the marker off the kernel to force the work-item loop
// to 16. A correct 16x16 product with no width env proves the width came from
// the kernel. (If the marker path were broken the loop would take the host
// width 8 and the shared-lane matmul would be wrong, exactly as at 4A.7.1.)
TEST(XpuCpuDistTileSpike, distributedMatmulWidthComesFromKernelNotEnv) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");            // width is per-kernel now
    setenv("CAJETA_GPU_COOPMATRIX_DIST", "on", 1);   // opt-in only
    int r = runOnCpu();
    unsetenv("CAJETA_GPU_COOPMATRIX_DIST");
    EXPECT_EQ(r, 0)
        << "per-kernel wave width failed; r=" << r
        << " (-1 = refused/skipped, 1000+cell = first wrong cell)";
}
