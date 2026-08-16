//
// CajetaXPU — Swizzled<T,S> (xpu-pipelined-gemm-primitives Unit 3): a workgroup-
// shared LDS tile whose element addressing is permuted by a conflict-free XOR
// swizzle, applied IDENTICALLY at every access so it is transparent at the
// logical-index level (the permutation is an involution: col ^= row & (S-1)).
//
// On a backend with no swizzle (CPU here) `swizzleAddr` is the identity, so a
// Swizzled tile behaves byte-for-byte like a plain Shared tile — the CPU is the
// correctness oracle (swizzling is a perf-only transform that must never change
// results). On AMD the XOR is emitted; its involution + cross-access consistency
// are checked GPU-free (the IR carries the xor) and on-device (round-trip holds).
//
// These JIT-compile `--xpu-backend=cpu`: (1) a Swizzled tile written then read
// cross-lane round-trips every value, and (2) a Swizzled tile and a plain Shared
// tile driven by the same logical access pattern produce identical output.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// Each thread writes tile[t] = t*7+1 into a Swizzled<uint32,16> tile, barriers,
// then reads a DIFFERENT lane tile[(t*13+5)&255]. Because the swizzle maps a
// given logical index to the same physical slot whether written or read, the
// cross-lane read returns exactly what that logical index was written with:
// out[t] == ((t*13+5)&255)*7 + 1 for every t. (On CPU the swizzle is identity;
// the point is the primitive compiles + the logical model holds end to end.)
const char* kSwizzleRoundTrip = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Swizzled;
public class M {
    @Kernel
    public static void swz(KernelBuffer<uint32> out) {
        Swizzled<uint32, 16> tile = shared uint32[256];
        uint32 t = KernelThread.x();
        tile[t] = t * 7 + 1;
        Barrier.workgroup();
        uint32 j = (t * 13 + 5) & 255;
        out[t] = tile[j];
    }
    public static uint32 run() {
        uint32 n = 256;
        uint32[] hout = heap uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = 0; }
        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);
        out.upload(hout);
        KernelStream s #= KernelStream.current();
        swz.launch(s, grid: [1], block: [256])(out);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 j = (i * 13 + 5) & 255;
            if (hout[i] != j * 7 + 1) { return 100 + i; }
        }
        return 12345;
    }
}
)CJ";

// The same logical access pattern through a Swizzled tile and a plain Shared
// tile must produce identical output — the swizzle is transparent. Two kernels,
// byte-identical bodies but for the tile's declared type; run() compares lanes.
const char* kSwizzleMatchesPlain = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Shared;
import cajeta.xpu.Swizzled;
public class M {
    @Kernel
    public static void viaSwizzle(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Swizzled<int32, 16> tile = shared int32[256];
        uint32 t = KernelThread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        out[t] = tile[t] + tile[(t + 7) & 255];
    }
    @Kernel
    public static void viaPlain(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Shared<int32> tile = shared int32[256];
        uint32 t = KernelThread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        out[t] = tile[t] + tile[(t + 7) & 255];
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] hin = heap int32[n];
        int32[] hA = heap int32[n];
        int32[] hB = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) (i * 3 + 1); hA[i] = 0; hB[i] = 0; }
        KernelBuffer<int32> in = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> oa = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> ob = heap KernelBuffer<int32>(n);
        in.upload(hin);
        oa.upload(hA);
        ob.upload(hB);
        KernelStream s #= KernelStream.current();
        viaSwizzle.launch(s, grid: [1], block: [256])(oa, in);
        viaPlain.launch(s, grid: [1], block: [256])(ob, in);
        s.sync();
        oa.download(hA);
        ob.download(hB);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hA[i] != hB[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

// Stage into a Swizzled tile two ways — CoopStage.panel and AsyncCopy.copy — then
// read it back by direct index. The staging writes and the direct read must apply
// the SAME swizzle to a given logical slot, so out[t] == in[t] for both. (This is
// the cross-access-consistency check: write path and read path agree.)
const char* kSwizzleStaged = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Swizzled;
import cajeta.xpu.CoopStage;
import cajeta.xpu.AsyncCopy;
public class M {
    @Kernel
    public static void viaCoop(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Swizzled<int32, 16> tile = shared int32[256];
        uint32 t = KernelThread.x();
        CoopStage.panel(tile, in, 0, 0, 1, 256, 256);
        Barrier.workgroup();
        out[t] = tile[t];
    }
    @Kernel
    public static void viaAsync(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Swizzled<int32, 16> tile = shared int32[256];
        uint32 t = KernelThread.x();
        AsyncCopy.copy(tile, 0, in, 0, 256);
        AsyncCopy.commit();
        AsyncCopy.wait(0);
        Barrier.workgroup();
        out[t] = tile[t];
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] hin = heap int32[n];
        int32[] hA = heap int32[n];
        int32[] hB = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) (i * 5 + 2); hA[i] = 0; hB[i] = 0; }
        KernelBuffer<int32> in = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> oa = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> ob = heap KernelBuffer<int32>(n);
        in.upload(hin);
        oa.upload(hA);
        ob.upload(hB);
        KernelStream s #= KernelStream.current();
        viaCoop.launch(s, grid: [1], block: [256])(oa, in);
        viaAsync.launch(s, grid: [1], block: [256])(ob, in);
        s.sync();
        oa.download(hA);
        ob.download(hB);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hA[i] != hin[i]) { return (int32) (100 + i); }
            if (hB[i] != hin[i]) { return (int32) (1000 + i); }
        }
        return 555;
    }
}
)CJ";

// U3b: a tiled 16x16 GEMM where A and B are staged into Swizzled<float16,16> LDS
// tiles (CoopStage.panel), then WMMA-loaded from those swizzled tiles. The
// CooperativeMatrix fragment loads must apply the SAME swizzle the staging did, or
// the matmul is wrong. A[i][k]=(i+2k)%5, B[k][j]=(3k+j)%4 (i,k,j-distinct, small
// enough to be exact in half); run() compares the kernel's C against an in-kernel
// reference and returns 999 on full match.
const char* kSwizzledWmma = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.Barrier;
import cajeta.xpu.Swizzled;
import cajeta.xpu.CoopStage;
import cajeta.xpu.CooperativeMatrix;
public class M {
    @Kernel
    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,
                            KernelBuffer<float32> c) {
        Swizzled<float16, 16> sa = shared float16[256];
        Swizzled<float16, 16> sb = shared float16[256];
        CoopStage.panel(sa, a, 0, 0, 16, 16, 16);
        CoopStage.panel(sb, b, 0, 0, 16, 16, 16);
        Barrier.workgroup();
        CooperativeMatrix<float16, 16, 16, 0> ma;
        ma.load(sa, 0, 0, 16);
        CooperativeMatrix<float16, 16, 16, 1> mb;
        mb.load(sb, 0, 0, 16);
        CooperativeMatrix<float32, 16, 16, 2> mc;
        mc.splat(0.0f);
        mc.mma(ma, mb);
        mc.store(c, 0, 0, 16);
    }
    public static int32 run() {
        uint32 n = 256;
        float16[] ha = heap float16[n];
        float16[] hb = heap float16[n];
        float32[] hc = heap float32[n];
        for (uint32 i = 0; i < 16; i = i + 1) {
            for (uint32 k = 0; k < 16; k = k + 1) {
                ha[i * 16 + k] = (float16) ((float32) ((i + 2 * k) % 5));
                hb[i * 16 + k] = (float16) ((float32) ((3 * i + k) % 4));
                hc[i * 16 + k] = 0.0f;
            }
        }
        KernelBuffer<float16> a = heap KernelBuffer<float16>(n);
        KernelBuffer<float16> b = heap KernelBuffer<float16>(n);
        KernelBuffer<float32> c = heap KernelBuffer<float32>(n);
        a.upload(ha);
        b.upload(hb);
        c.upload(hc);
        KernelStream s #= KernelStream.current();
        wmma.launch(s, grid: [1], block: [32])(a, b, c);
        s.sync();
        c.download(hc);
        for (uint32 i = 0; i < 16; i = i + 1) {
            for (uint32 j = 0; j < 16; j = j + 1) {
                float32 acc = 0.0f;
                for (uint32 k = 0; k < 16; k = k + 1) {
                    acc = acc + (float32) ((i + 2 * k) % 5) * (float32) ((3 * k + j) % 4);
                }
                if (hc[i * 16 + j] != acc) { return (int32) (100 + i * 16 + j); }
            }
        }
        return 999;
    }
}
)CJ";

} // namespace

// U3.1.a (CPU half): a Swizzled tile write-then-cross-lane-read round-trips every
// logical value (the swizzle is consistent across write and read).
TEST(XpuSwizzledTests, swizzledRoundTripCrossLane) {
    auto jit = CajetaJit::compile(kSwizzleRoundTrip, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<unsigned (*)()>("run");
    ASSERT_NE(fn, nullptr);
    unsigned r = fn();
    EXPECT_EQ(r, 12345u) << "fail code " << r << " (100+i: out[i] != (i*13+5&255)*7+1)";
}

// U3.1.a (transparency): a Swizzled tile and a plain Shared tile under the same
// logical access pattern give identical output — swizzling never changes results.
TEST(XpuSwizzledTests, swizzledMatchesPlainShared) {
    auto jit = CajetaJit::compile(kSwizzleMatchesPlain, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: swizzle[i] != plain[i])";
}

// U3.1.a (cross-access consistency): staging into a Swizzled tile via CoopStage
// and via AsyncCopy, then reading by direct index, both round-trip — the write
// path and the read path apply the same swizzle to each logical slot.
TEST(XpuSwizzledTests, swizzledStagedRoundTrips) {
    auto jit = CajetaJit::compile(kSwizzleStaged, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 555) << "fail code " << r << " (100+i CoopStage, 1000+i AsyncCopy)";
}

// U3b (CPU software-tier oracle): a 16x16 GEMM whose A/B tiles are staged into
// Swizzled LDS and WMMA-loaded from there computes correctly — the fragment loads
// apply the same swizzle as the staging. (Software cooperative-matrix tile on CPU;
// the AMD on-device test exercises the real WMMA path.)
