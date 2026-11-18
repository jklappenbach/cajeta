//
// SpirvStagedElementTests — per-element access on a native SPIR-V tile, staged
// through workgroup memory (spirv-kernel-parity 3.1).
//
// An f16 operand tile edited element by element must feed the multiply as
// edited: the same bits as the device's multiply of an operand edited on the
// host (gfx1151's matrix cores do not round every cell exactly, on hip as on
// vulkan). An accumulator doubled inside a loop with a uniform runtime trip
// count must come out doubled. XpuFragmentElementTests covers the accumulators'
// (row, col) map cell by cell.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// With `edit`, a'[r][c] = a[r][c] + (r * 16 + c) / 8 through the operand's elements;
// then y = a' * I, doubled `reps` times through the accumulator's elements.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class SE {
    @Kernel
    public static void k(KernelBuffer<float32> y, KernelBuffer<float16> a, KernelBuffer<float16> eye,
                         uint32 edit, uint32 reps) {
        CooperativeMatrix<float16,16,16,0> am;
        CooperativeMatrix<float16,16,16,1> bm;
        CooperativeMatrix<float32,16,16,2> acc;
        am.load(a, 0, 0, 16);
        bm.load(eye, 0, 0, 16);
        acc.splat(0.0f);
        if (edit == 1) {
            uint32 n = am.elements();
            for (uint32 i = 0; i < n; i = i + 1) {
                uint32 r = am.row(i);
                uint32 c = am.col(i);
                am.set(i, (float16) ((float32) am.get(i) + (float32) (r * 16 + c) * 0.125f));
            }
        }
        acc.mma(am, bm);
        for (uint32 t = 0; t < reps; t = t + 1) {
            uint32 m = acc.elements();
            for (uint32 i = 0; i < m; i = i + 1) { acc.set(i, acc.get(i) * 2.0f); }
        }
        acc.store(y, 0, 0, 16);
    }
    public static float32 run(uint32 at, uint32 edit, uint32 reps) {
        float16[] ha = heap float16[256];
        float16[] he = heap float16[256];
        float32[] hy = heap float32[256];
        for (int32 i = 0; i < 256; i = i + 1) {
            float32 v = (float32) ((i * 7) % 11 - 5);
            ha[i] = (float16) (edit == 1 ? v : v + (float32) i * 0.125f);
            he[i] = (float16) ((i / 16 == i % 16) ? 1.0f : 0.0f);
            hy[i] = 0.0f;
        }
        KernelBuffer<float16> a = heap KernelBuffer<float16>(256);
        KernelBuffer<float16> e = heap KernelBuffer<float16>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(ha);
        e.upload(he);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [32])(y, a, e, edit, reps);
        s.sync();
        y.download(hy);
        return hy[at];
    }
}
)CJ";

} // namespace

TEST(SpirvStagedElementTests, anEditedOperandFeedsTheMultiplyAndALoopEditsTheAccumulator) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.SE", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<float (*)(unsigned, unsigned, unsigned)>("run");
    ASSERT_NE(run, nullptr);
    for (unsigned i = 0; i < 256; ++i) {
        const float host = run(i, 0, 0);
        EXPECT_EQ(run(i, 1, 0), host) << "cell " << i << ": the staged edit";
        EXPECT_EQ(run(i, 1, 3), host * 8.0f) << "cell " << i << ": the edit, then three doublings";
    }
}

namespace {

// The int8 operand: a'[r][c] = a[r][c] + (r + c) % 5 through its elements, then the
// int32 product with the identity, which is exact.
const char* kInt8Source = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class SE8 {
    @Kernel
    public static void k(KernelBuffer<int32> y, KernelBuffer<int8> a, KernelBuffer<int8> eye) {
        CooperativeMatrix<int8,16,16,0> am;
        CooperativeMatrix<int8,16,16,1> bm;
        CooperativeMatrix<int32,16,16,2> acc;
        am.load(a, 0, 0, 16);
        bm.load(eye, 0, 0, 16);
        acc.splat(0);
        uint32 n = am.elements();
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 r = am.row(i);
            uint32 c = am.col(i);
            am.set(i, (int8) ((int32) am.get(i) + (int32) ((r + c) % 5)));
        }
        acc.mma(am, bm);
        acc.store(y, 0, 0, 16);
    }
    public static int32 run(uint32 at) {
        int8[] ha = heap int8[256];
        int8[] he = heap int8[256];
        int32[] hy = heap int32[256];
        for (int32 i = 0; i < 256; i = i + 1) {
            ha[i] = (int8) ((i * 7) % 11 - 5);
            he[i] = (int8) ((i / 16 == i % 16) ? 1 : 0);
            hy[i] = 0;
        }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);
        KernelBuffer<int8> e = heap KernelBuffer<int8>(256);
        KernelBuffer<int32> y = heap KernelBuffer<int32>(256);
        a.upload(ha);
        e.upload(he);
        y.upload(hy);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [32])(y, a, e);
        s.sync();
        y.download(hy);
        return hy[at];
    }
}
)CJ";

} // namespace

TEST(SpirvStagedElementTests, anEditedInt8OperandFeedsTheIntegerMultiply) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kInt8Source, "test.SE8", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)(unsigned)>("run");
    ASSERT_NE(run, nullptr);
    for (unsigned i = 0; i < 256; ++i) {
        const int want = ((int) i * 7) % 11 - 5 + (int) ((i / 16 + i % 16) % 5);
        EXPECT_EQ(run(i), want) << "cell " << i;
    }
}

namespace {

// b[r][c] = b[r][c] + 3 * a[r][c] through both tiles' elements in one expression: the value
// reads `a`, a tile of the same type, while `b` is the one being set.
const char* kTwoTileSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class SE2 {
    @Kernel
    public static void k(KernelBuffer<int32> y, KernelBuffer<int32> a, KernelBuffer<int32> b) {
        CooperativeMatrix<int32,16,16,2> am;
        CooperativeMatrix<int32,16,16,2> bm;
        am.load(a, 0, 0, 16);
        bm.load(b, 0, 0, 16);
        for (uint32 i = 0; i < am.elements(); i = i + 1) { bm.set(i, bm.get(i) + am.get(i) * 3); }
        bm.store(y, 0, 0, 16);
    }
    public static int32 run() {
        int32[] ha = heap int32[256];
        int32[] hb = heap int32[256];
        int32[] hy = heap int32[256];
        for (int32 i = 0; i < 256; i = i + 1) { ha[i] = (i * 7) % 11 - 5; hb[i] = 1000 + i; }
        KernelBuffer<int32> a = heap KernelBuffer<int32>(256);
        KernelBuffer<int32> b = heap KernelBuffer<int32>(256);
        KernelBuffer<int32> y = heap KernelBuffer<int32>(256);
        a.upload(ha);
        b.upload(hb);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [32])(y, a, b);
        s.sync();
        y.download(hy);
        for (int32 i = 0; i < 256; i = i + 1) {
            if (hy[i] != 1000 + i + 3 * ((i * 7) % 11 - 5)) { return 1000 + i; }
        }
        return 777;
    }
}
)CJ";

} // namespace

TEST(SpirvStagedElementTests, aSetWhoseValueReadsAnotherTileOfTheSameTypeKeepsItsOwnTile) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTwoTileSource, "test.SE2", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 777) << "1000+i: cell i took the other tile's value";
}
