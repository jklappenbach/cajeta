// Native cooperative-matrix loads and stores at any placement (coop-converting-store
// Unit 5, spec §2.5). wmma on sm_89 needs each lane's contiguous run aligned: 8 bytes
// for an f32 or s32 accumulator, 4 for an f16 or bf16 fragment and an 8-bit operand,
// with the stride a multiple of the same (measured, NvptxOperandMapTests). Off that, a
// misaligned tile FAULTS and an odd stride puts cells in the wrong place silently.
// Every placement must read and write the cells the reference does.
//
// Operand loads are checked through the multiply: A loaded at the placement times an
// aligned identity B is A, stored aligned; B likewise against an identity A.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class U {
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Store(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.store(y, off, 0, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32StoreCol(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.store(y, off, 1, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void i32Store(KernelBuffer<int32> y, KernelBuffer<int32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<int32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.store(y, off, 0, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Load(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, off, 0, stride);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16A(KernelBuffer<float32> y, KernelBuffer<float16> a, KernelBuffer<float16> eye,
                            uint32 off, uint32 stride) {
        CooperativeMatrix<float16,16,16,0> ta;
        CooperativeMatrix<float16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(a, off, 0, stride);
        tb.load(eye, 0, 0, 16);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16ACol(KernelBuffer<float32> y, KernelBuffer<float16> a, KernelBuffer<float16> eye,
                               uint32 off, uint32 stride) {
        CooperativeMatrix<float16,16,16,0> ta;
        CooperativeMatrix<float16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(a, off, 1, stride);
        tb.load(eye, 0, 0, 16);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16B(KernelBuffer<float32> y, KernelBuffer<float16> b, KernelBuffer<float16> eye,
                            uint32 off, uint32 stride) {
        CooperativeMatrix<float16,16,16,0> ta;
        CooperativeMatrix<float16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(eye, 0, 0, 16);
        tb.load(b, off, 0, stride);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void bf16A(KernelBuffer<float32> y, KernelBuffer<bfloat16> a, KernelBuffer<bfloat16> eye,
                             uint32 off, uint32 stride) {
        CooperativeMatrix<bfloat16,16,16,0> ta;
        CooperativeMatrix<bfloat16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(a, off, 0, stride);
        tb.load(eye, 0, 0, 16);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void s8A(KernelBuffer<int32> y, KernelBuffer<int8> a, KernelBuffer<int8> eye,
                           uint32 off, uint32 stride) {
        CooperativeMatrix<int8,16,16,0> ta;
        CooperativeMatrix<int8,16,16,1> tb;
        CooperativeMatrix<int32,16,16,2> acc;
        ta.load(a, off, 0, stride);
        tb.load(eye, 0, 0, 16);
        acc.splat(0);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void s8B(KernelBuffer<int32> y, KernelBuffer<int8> b, KernelBuffer<int8> eye,
                           uint32 off, uint32 stride) {
        CooperativeMatrix<int8,16,16,0> ta;
        CooperativeMatrix<int8,16,16,1> tb;
        CooperativeMatrix<int32,16,16,2> acc;
        ta.load(eye, 0, 0, 16);
        tb.load(b, off, 0, stride);
        acc.splat(0);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }

    static int32 cell(uint32 r, uint32 c, uint32 layout, uint32 off, uint32 stride) {
        if (layout == 0) { return (int32) (off + r * stride + c); }
        return (int32) (off + c * stride + r);
    }
    // -1 when right; otherwise mismatches * 1000 + the first bad index.
    static int32 verdict(float32[] got, float32[] want, int32 n) {
        int32 bad = 0;
        int32 first = -1;
        for (int32 k = 0; k < n; k = k + 1) {
            if (got[k] != want[k]) {
                if (first < 0) { first = k; }
                bad = bad + 1;
            }
        }
        if (bad == 0) { return -1; }
        return bad * 1000 + first;
    }

    // Stores: which 0 f32 row, 1 f32 col, 2 i32 row. The tile is placed at (off, stride).
    public static int32 runStore(int32 which, uint32 off, uint32 stride) {
        float32[] h = heap float32[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = (float32) (i * 3 - 200); }
        float32[] got = heap float32[600];
        float32[] want = heap float32[600];
        for (int32 k = 0; k < 600; k = k + 1) { got[k] = -7.0f; want[k] = -7.0f; }
        uint32 layout = which == 1 ? 1 : 0;
        for (uint32 r = 0; r < 16; r = r + 1) {
            for (uint32 c = 0; c < 16; c = c + 1) { want[U.cell(r, c, layout, off, stride)] = h[r * 16 + c]; }
        }
        KernelStream s #= KernelStream.current();
        if (which < 2) {
            KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
            KernelBuffer<float32> y = heap KernelBuffer<float32>(600);
            a.upload(h);
            y.upload(got);
            if (which == 0) { f32Store.launch(s, grid: [1], block: [32])(y, a, off, stride); }
            if (which == 1) { f32StoreCol.launch(s, grid: [1], block: [32])(y, a, off, stride); }
            s.sync();
            y.download(got);
        } else {
            int32[] hi = heap int32[256];
            for (int32 i = 0; i < 256; i = i + 1) { hi[i] = i * 3 - 200; }
            int32[] gi = heap int32[600];
            for (int32 k = 0; k < 600; k = k + 1) { gi[k] = -7; }
            KernelBuffer<int32> a = heap KernelBuffer<int32>(256);
            KernelBuffer<int32> y = heap KernelBuffer<int32>(600);
            a.upload(hi);
            y.upload(gi);
            i32Store.launch(s, grid: [1], block: [32])(y, a, off, stride);
            s.sync();
            y.download(gi);
            for (int32 k = 0; k < 600; k = k + 1) { got[k] = (float32) gi[k]; }
        }
        return U.verdict(got, want, 600);
    }

    // Loads: which 0 f32 accumulator, 1 f16 A row, 2 f16 A col, 3 f16 B, 4 bf16 A,
    // 5 s8 A, 6 s8 B. Memory cell m holds a value of m; the 16 x 16 result must be
    // the tile read at (off, stride).
    public static int32 runLoad(int32 which, uint32 off, uint32 stride) {
        float32[] mem = heap float32[600];
        for (int32 m = 0; m < 600; m = m + 1) { mem[m] = (float32) ((m * 7) % 101 - 50); }
        uint32 layout = which == 2 ? 1 : 0;
        float32[] want = heap float32[256];
        for (uint32 r = 0; r < 16; r = r + 1) {
            for (uint32 c = 0; c < 16; c = c + 1) { want[r * 16 + c] = mem[U.cell(r, c, layout, off, stride)]; }
        }
        float32[] got = heap float32[256];
        KernelStream s #= KernelStream.current();
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        KernelBuffer<int32> yi = heap KernelBuffer<int32>(256);
        if (which == 0) {
            KernelBuffer<float32> a = heap KernelBuffer<float32>(600);
            a.upload(mem);
            f32Load.launch(s, grid: [1], block: [32])(y, a, off, stride);
        }
        if (which >= 1 && which <= 3) {
            float16[] hm = heap float16[600];
            float16[] he = heap float16[256];
            for (int32 m = 0; m < 600; m = m + 1) { hm[m] = (float16) mem[m]; }
            for (int32 i = 0; i < 256; i = i + 1) { he[i] = (float16) ((i / 16 == i % 16) ? 1.0f : 0.0f); }
            KernelBuffer<float16> a = heap KernelBuffer<float16>(600);
            KernelBuffer<float16> e = heap KernelBuffer<float16>(256);
            a.upload(hm);
            e.upload(he);
            if (which == 1) { f16A.launch(s, grid: [1], block: [32])(y, a, e, off, stride); }
            if (which == 2) { f16ACol.launch(s, grid: [1], block: [32])(y, a, e, off, stride); }
            if (which == 3) { f16B.launch(s, grid: [1], block: [32])(y, a, e, off, stride); }
        }
        if (which == 4) {
            bfloat16[] hm = heap bfloat16[600];
            bfloat16[] he = heap bfloat16[256];
            for (int32 m = 0; m < 600; m = m + 1) { hm[m] = (bfloat16) mem[m]; }
            for (int32 i = 0; i < 256; i = i + 1) { he[i] = (bfloat16) ((i / 16 == i % 16) ? 1.0f : 0.0f); }
            KernelBuffer<bfloat16> a = heap KernelBuffer<bfloat16>(600);
            KernelBuffer<bfloat16> e = heap KernelBuffer<bfloat16>(256);
            a.upload(hm);
            e.upload(he);
            bf16A.launch(s, grid: [1], block: [32])(y, a, e, off, stride);
        }
        if (which >= 5) {
            int8[] hm = heap int8[600];
            int8[] he = heap int8[256];
            for (int32 m = 0; m < 600; m = m + 1) { hm[m] = (int8) ((m * 7) % 101 - 50); }
            for (int32 i = 0; i < 256; i = i + 1) { he[i] = (int8) ((i / 16 == i % 16) ? 1 : 0); }
            KernelBuffer<int8> a = heap KernelBuffer<int8>(600);
            KernelBuffer<int8> e = heap KernelBuffer<int8>(256);
            a.upload(hm);
            e.upload(he);
            if (which == 5) { s8A.launch(s, grid: [1], block: [32])(yi, a, e, off, stride); }
            if (which == 6) { s8B.launch(s, grid: [1], block: [32])(yi, a, e, off, stride); }
        }
        s.sync();
        if (which >= 5) {
            int32[] gi = heap int32[256];
            yi.download(gi);
            for (int32 k = 0; k < 256; k = k + 1) { got[k] = (float32) gi[k]; }
        } else {
            y.download(got);
        }
        return U.verdict(got, want, 256);
    }
}
)CJ";

struct Place { unsigned off, stride; const char* what; };
// Aligned (the wmma form), 4-byte aligned with an even stride, an odd element
// offset, and an odd stride.
const Place kPlaces[] = {
    {0, 16, "offset 0, stride 16"},
    {2, 18, "offset 2, stride 18"},
    {1, 18, "offset 1, stride 18"},
    {11, 17, "offset 11, stride 17"},
};

void everyPlacementIsRight(cajeta::xpu::Backend be, const char* name) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.U", o);
    ASSERT_NE(jit, nullptr);
    auto st = jit->lookup<int32_t (*)(int32_t, uint32_t, uint32_t)>("runStore");
    auto ld = jit->lookup<int32_t (*)(int32_t, uint32_t, uint32_t)>("runLoad");
    ASSERT_TRUE(st && ld);
    const char* stores[] = {"f32 store row", "f32 store col", "i32 store"};
    const char* loads[] = {"f32 accumulator load", "f16 A load row", "f16 A load col",
                           "f16 B load", "bf16 A load", "s8 A load", "s8 B load"};
    // The verdict is -1, or mismatches * 1000 + the first bad index.
    for (const Place& p : kPlaces) {
        for (int w = 0; w < 3; ++w) EXPECT_EQ(-1, st(w, p.off, p.stride)) << name << " " << stores[w] << ", " << p.what;
        for (int w = 0; w < 7; ++w) EXPECT_EQ(-1, ld(w, p.off, p.stride)) << name << " " << loads[w] << ", " << p.what;
    }
}

} // namespace

TEST(XpuCoopUnaligned, everyPlacementIsRightOnCpu) {
    everyPlacementIsRight(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuCoopUnalignedDeviceTests, everyPlacementIsRightOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    everyPlacementIsRight(cajeta::xpu::Backend::Nvptx, "nvptx");
}

// The aligned branch keeps one wmma per fragment: the guard does not demote the
// form the GEMMs use.
TEST(XpuCoopUnaligned, nvptxKeepsTheWmmaFormsBehindTheGuard) {
    using namespace cajeta::xpu::probe;
    struct Want { const char* kernel; const char* ptx; };
    const Want wants[] = {
        {"f32Store", "wmma.store.d.sync.aligned.row.m16n16k16.global.f32"},
        {"f32Load", "wmma.load.c.sync.aligned.row.m16n16k16.global.f32"},
        {"f16A", "wmma.load.a.sync.aligned.row.m16n16k16.global.f16"},
        {"f16B", "wmma.load.b.sync.aligned.row.m16n16k16.global.f16"},
        {"s8A", "wmma.load.a.sync.aligned.row.m16n16k16.global.s8"},
    };
    for (const Want& w : wants) {
        Lowered l = lowerForNvptx(kSource, w.kernel, "test.U");
        ASSERT_TRUE(l.ok) << w.kernel << ": " << l.why;
        EXPECT_NE(l.ptx.find(w.ptx), std::string::npos) << w.kernel << " lacks " << w.ptx;
    }
}
