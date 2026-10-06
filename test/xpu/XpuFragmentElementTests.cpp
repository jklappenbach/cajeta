// Per-element access to a cooperative-matrix fragment with the lane-to-element
// map (xpu-kernel-independence 4.9.1.1, spec §4.1).
//
// A fragment is held across the wave, each lane owning some of its elements in
// a layout the hardware chooses. `elements()` is how many this lane holds,
// `get(i)` / `set(i, v)` read and write one, and `row(i)` / `col(i)` say which
// cell of the tile it is. A kernel that walks its elements and writes each
// from its own (row, col) must produce the same matrix as a store and reload
// would, on every backend that lowers the tile: that is what a library
// epilogue needs, and what a wrong map (a transposed, rotated or duplicated
// mapping) cannot produce. The reference interpreter defines its own map, so
// the corpus checks the same kernel (XpuBuildingBlockCorpusTests).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "XpuRefusalProbe.h"
#include "cajeta/xpu/XpuTarget.h"

#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// y[r][c] = 2 * a[r][c] + (r * 16 + c), computed through this lane's elements.
// a[i] = (i * 7) % 11 - 5, so every cell differs from its neighbours and a
// term placed in the wrong cell changes the answer.
std::string program() {
    return R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.XpuLaunchException;
public class M {
    @Kernel
    public static void elemsF32(KernelBuffer<float32> y, KernelBuffer<float32> a) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        uint32 n = acc.elements();
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 r = acc.row(i);
            uint32 c = acc.col(i);
            acc.set(i, acc.get(i) * 2.0f + (float32) (r * 16 + c));
        }
        acc.store(y, 0, 0, 16);
    }
    @Kernel
    public static void elemsI32(KernelBuffer<int32> y, KernelBuffer<int32> a) {
        CooperativeMatrix<int32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        uint32 n = acc.elements();
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 r = acc.row(i);
            uint32 c = acc.col(i);
            acc.set(i, acc.get(i) * 2 + (int32) (r * 16 + c));
        }
        acc.store(y, 0, 0, 16);
    }
    public static float32 runF(uint32 at) {
        float32[] h = heap float32[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = (float32) ((i * 7) % 11 - 5); }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(h);
        y.upload(h);
        KernelStream s #= KernelStream.current();
        elemsF32.launch(s, grid: [1], block: [32])(y, a);
        s.sync();
        y.download(h);
        return h[at];
    }
    public static int32 runI(uint32 at) {
        int32[] h = heap int32[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = (i * 7) % 11 - 5; }
        KernelBuffer<int32> a = heap KernelBuffer<int32>(256);
        KernelBuffer<int32> y = heap KernelBuffer<int32>(256);
        a.upload(h);
        y.upload(h);
        KernelStream s #= KernelStream.current();
        elemsI32.launch(s, grid: [1], block: [32])(y, a);
        s.sync();
        y.download(h);
        return h[at];
    }
    // Launch once under a catch, so a backend that refuses the kernel still
    // returns and the refusal note can be read.
    public static int32 probe() {
        float32[] h = heap float32[256];
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        a.upload(h);
        y.upload(h);
        KernelStream s #= KernelStream.current();
        try {
            elemsF32.launch(s, grid: [1], block: [32])(y, a);
            s.sync();
        } catch (XpuLaunchException e) { }
        return 1;
    }
}
)CJ";
}

int expected(int i) { return 2 * ((i * 7) % 11 - 5) + i; }

// Every cell of both accumulators, on `be`.
void checkEveryCell(cajeta::xpu::Backend be, const char* name) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(program(), "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto runF = jit->lookup<float (*)(unsigned)>("runF");
    auto runI = jit->lookup<int (*)(unsigned)>("runI");
    ASSERT_NE(runF, nullptr);
    ASSERT_NE(runI, nullptr);
    for (unsigned i = 0; i < 256; ++i) {
        EXPECT_EQ(runF(i), (float) expected((int) i)) << name << " float32 cell " << i;
        EXPECT_EQ(runI(i), expected((int) i)) << name << " int32 cell " << i;
    }
}

// Compile for `be` and run the probe, returning what the compile printed.
std::string probeOn(cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(program(), "test.M", o);
    if (jit)
        if (auto fn = jit->lookup<int (*)()>("probe")) fn();
    return testing::internal::GetCapturedStderr();
}

} // namespace

TEST(XpuFragmentElement, everyElementIsVisitedAtItsRowAndColumnOnCpu) {
    checkEveryCell(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuFragmentElement, everyElementIsVisitedAtItsRowAndColumnOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    checkEveryCell(cajeta::xpu::Backend::Nvptx, "nvptx");
}

TEST(XpuFragmentElement, everyElementIsVisitedAtItsRowAndColumnOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    checkEveryCell(cajeta::xpu::Backend::Amdgpu, "amdgpu");
}

// The access lowers on the NATIVE tier of both matrix-core backends: the map
// is the hardware fragment's, not a demotion to the software tile.
TEST(XpuFragmentElement, lowersOnTheNativeTierOfNvptxAndAmdgpu) {
    using namespace cajeta::xpu::probe;
    for (const char* k : {"elemsF32", "elemsI32"}) {
        Lowered l = lowerForNvptx(program(), k);
        EXPECT_TRUE(l.ok) << k << ": " << l.why;
        EXPECT_NE(l.ir.find("llvm.nvvm.wmma"), std::string::npos)
            << k << " must stay on the wmma fragment";
    }
    std::string err = probeOn(cajeta::xpu::Backend::Amdgpu);
    EXPECT_FALSE(cajeta_test::loweringRefused(err)) << err;
    EXPECT_EQ(err.find("[mma-tiering]"), std::string::npos)
        << "the access must not demote the tile:\n" << err;
}

// The SPIR-V cooperative matrix has no documented lane-to-element map, so the
// backend refuses by name rather than guess one.
TEST(XpuFragmentElement, isRefusedByNameOnVulkan) {
    std::string err = probeOn(cajeta::xpu::Backend::Spirv);
    EXPECT_TRUE(cajeta_test::loweringRefused(err)) << err;
    EXPECT_NE(err.find("lane-to-element"), std::string::npos) << err;
}
