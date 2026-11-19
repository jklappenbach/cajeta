// A float32 accumulator fragment stored at 16 bits: `acc.storeAs<U>(dst, offset,
// layout, stride)` (specs/coop-converting-store-spec.md §2.1, plan Unit 1).
//
// Each stored cell must equal the scalar cast `(U) x` of the fragment's element at
// that (row, col), and every other cell of the destination must be untouched. The
// oracle is computed on the host from the input, so it checks the placement (both
// layouts, an odd stride, an offset, a Shared destination) and the conversion
// (ties to even, overflow to infinity, NaN, denormals, signed zero) without
// trusting the backend's own element map. The reference interpreter defines the
// same operation (XpuBuildingBlockCorpusTests, bbStoreAs).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "XpuRefusalProbe.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>
#include <optional>

#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// The kernels take the layout as a constant (the native stores need one) and the
// offset and stride at run time. `which` picks the kernel: 0 row-major global,
// 1 column-major global, 2 a Shared tile (column-major, offset 3, stride 17, for
// float16; row-major, offset 5, stride 17, for bfloat16) copied out to global,
// 3 a row-major Shared tile at offset 0, stride 16.
//
// On nvptx the placement picks the form (spec §2.2, §2.5): a float16 tile on a
// 4-byte boundary with an even stride takes wmma.store.d.f16, a row-major bfloat16
// tile there the packed pairs, and anything else the per-element store. The
// configurations below reach all three.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.XpuLaunchException;
public class S {
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16Row(KernelBuffer<float16> y, KernelBuffer<float32> a,
                              uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<float16>(y, off, 0, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16Col(KernelBuffer<float16> y, KernelBuffer<float32> a,
                              uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<float16>(y, off, 1, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16Shared(KernelBuffer<float16> y, KernelBuffer<float32> a,
                                 uint32 off, uint32 stride) {
        Shared<float16> t = shared float16[300];
        uint32 g = KernelThread.x();
        for (uint32 k = g; k < 300; k = k + 32) { t[k] = (float16) (-7.0f); }
        Barrier.workgroup();
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<float16>(t, 3, 1, 17);
        Barrier.workgroup();
        for (uint32 k = g; k < 300; k = k + 32) { y[k] = t[k]; }
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void bf16Row(KernelBuffer<bfloat16> y, KernelBuffer<float32> a,
                               uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<bfloat16>(y, off, 0, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void bf16Col(KernelBuffer<bfloat16> y, KernelBuffer<float32> a,
                               uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<bfloat16>(y, off, 1, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void bf16Shared(KernelBuffer<bfloat16> y, KernelBuffer<float32> a,
                                  uint32 off, uint32 stride) {
        Shared<bfloat16> t = shared bfloat16[300];
        uint32 g = KernelThread.x();
        for (uint32 k = g; k < 300; k = k + 32) { t[k] = (bfloat16) (-7.0f); }
        Barrier.workgroup();
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<bfloat16>(t, 5, 0, 17);
        Barrier.workgroup();
        for (uint32 k = g; k < 300; k = k + 32) { y[k] = t[k]; }
    }
    // Row-major into Shared at the tile's start: the aligned forms on nvptx.
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16SharedRow(KernelBuffer<float16> y, KernelBuffer<float32> a,
                                    uint32 off, uint32 stride) {
        Shared<float16> t = shared float16[300];
        uint32 g = KernelThread.x();
        for (uint32 k = g; k < 300; k = k + 32) { t[k] = (float16) (-7.0f); }
        Barrier.workgroup();
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<float16>(t, 0, 0, 16);
        Barrier.workgroup();
        for (uint32 k = g; k < 300; k = k + 32) { y[k] = t[k]; }
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void bf16SharedRow(KernelBuffer<bfloat16> y, KernelBuffer<float32> a,
                                     uint32 off, uint32 stride) {
        Shared<bfloat16> t = shared bfloat16[300];
        uint32 g = KernelThread.x();
        for (uint32 k = g; k < 300; k = k + 32) { t[k] = (bfloat16) (-7.0f); }
        Barrier.workgroup();
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<bfloat16>(t, 0, 0, 16);
        Barrier.workgroup();
        for (uint32 k = g; k < 300; k = k + 32) { y[k] = t[k]; }
    }
    // storeAs at the accumulator's own type is a plain store.
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Row(KernelBuffer<float32> y, KernelBuffer<float32> a,
                              uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        acc.storeAs<float32>(y, off, 0, stride);
    }

    // Values that round to even at both widths, overflow, are NaN, are denormal
    // at one width or the other, and a negative zero.
    static float32 special(int32 k, float32 zero) {
        if (k == 0) { return 1.00048828125f; }      // 1 + 2^-11: f16 tie, down to 1
        if (k == 1) { return 1.00146484375f; }      // 1 + 3*2^-11: f16 tie, up to 1 + 2^-9
        if (k == 2) { return 70000.0f; }            // f16 infinity
        if (k == 3) { return -70000.0f; }
        if (k == 4) { return 65520.0f; }            // f16 tie at the top: infinity
        if (k == 5) { return 3.4028235e38f; }       // bf16 rounds up to infinity
        if (k == 6) { return zero / zero; }         // NaN
        if (k == 7) { return 0.000001f; }           // f16 subnormal
        if (k == 8) { return 0.00000006f; }         // near the smallest f16 subnormal
        if (k == 9) { return 1.0e-40f; }            // f32 denormal: bf16 subnormal, f16 zero
        if (k == 10) { return -0.0f; }
        if (k == 11) { return 1.00390625f; }        // 1 + 2^-8: bf16 tie, down to 1
        if (k == 12) { return 1.01171875f; }        // 1 + 3*2^-8: bf16 tie, up
        if (k == 13) { return 0.1f; }
        if (k == 14) { return -3.14159f; }
        return 1.0e-45f;                            // the smallest f32 denormal
    }
    static float32 input(int32 i, float32 zero) {
        if (i % 3 == 0) { return S.special((i / 3) % 16, zero); }
        return (float32) i * 0.37109375f - 41.3f;
    }
    // Equal bit for bit up to NaN payload: NaN matches NaN, and zeros match in sign.
    static boolean same(float32 x, float32 y) {
        if (x != x) { return y != y; }
        if (x != y) { return false; }
        if (x != 0.0f) { return true; }
        return 1.0f / x == 1.0f / y;
    }
    // Where (r, c) lands; -1 when the configuration does not place it.
    static int32 cell(uint32 r, uint32 c, uint32 layout, uint32 off, uint32 stride) {
        if (layout == 0) { return (int32) (off + r * stride + c); }
        return (int32) (off + c * stride + r);
    }
    // -1 when every cell is right; otherwise mismatches * 1000 + the first bad cell.
    static int32 verdict(float32[] got, float32[] want) {
        int32 bad = 0;
        int32 first = -1;
        for (int32 k = 0; k < 300; k = k + 1) {
            if (!S.same(got[k], want[k])) {
                if (first < 0) { first = k; }
                bad = bad + 1;
            }
        }
        if (bad == 0) { return -1; }
        return bad * 1000 + first;
    }

    public static int32 runF16(int32 which, uint32 off0, uint32 stride0, float32 zero) {
        float32[] h = heap float32[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = S.input(i, zero); }
        float16[] y0 = heap float16[300];
        for (int32 k = 0; k < 300; k = k + 1) { y0[k] = (float16) (-7.0f); }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float16> y = heap KernelBuffer<float16>(300);
        a.upload(h);
        y.upload(y0);
        uint32 layout = 0;
        uint32 off = off0;
        uint32 stride = stride0;
        KernelStream s #= KernelStream.current();
        if (which == 0) { f16Row.launch(s, grid: [1], block: [32])(y, a, off, stride); }
        if (which == 1) { layout = 1; f16Col.launch(s, grid: [1], block: [32])(y, a, off, stride); }
        if (which == 2) {
            layout = 1; off = 3; stride = 17;
            f16Shared.launch(s, grid: [1], block: [32])(y, a, off, stride);
        }
        if (which == 3) {
            off = 0; stride = 16;
            f16SharedRow.launch(s, grid: [1], block: [32])(y, a, off, stride);
        }
        s.sync();
        y.download(y0);
        float32[] got = heap float32[300];
        float32[] want = heap float32[300];
        for (int32 k = 0; k < 300; k = k + 1) {
            got[k] = (float32) y0[k];
            want[k] = (float32) ((float16) (-7.0f));
        }
        for (uint32 r = 0; r < 16; r = r + 1) {
            for (uint32 c = 0; c < 16; c = c + 1) {
                want[S.cell(r, c, layout, off, stride)] = (float32) ((float16) h[r * 16 + c]);
            }
        }
        return S.verdict(got, want);
    }

    public static int32 runBF16(int32 which, uint32 off0, uint32 stride0, float32 zero) {
        float32[] h = heap float32[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = S.input(i, zero); }
        bfloat16[] y0 = heap bfloat16[300];
        for (int32 k = 0; k < 300; k = k + 1) { y0[k] = (bfloat16) (-7.0f); }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<bfloat16> y = heap KernelBuffer<bfloat16>(300);
        a.upload(h);
        y.upload(y0);
        uint32 layout = 0;
        uint32 off = off0;
        uint32 stride = stride0;
        KernelStream s #= KernelStream.current();
        if (which == 0) { bf16Row.launch(s, grid: [1], block: [32])(y, a, off, stride); }
        if (which == 1) { layout = 1; bf16Col.launch(s, grid: [1], block: [32])(y, a, off, stride); }
        if (which == 2) {
            off = 5; stride = 17;
            bf16Shared.launch(s, grid: [1], block: [32])(y, a, off, stride);
        }
        if (which == 3) {
            off = 0; stride = 16;
            bf16SharedRow.launch(s, grid: [1], block: [32])(y, a, off, stride);
        }
        s.sync();
        y.download(y0);
        float32[] got = heap float32[300];
        float32[] want = heap float32[300];
        for (int32 k = 0; k < 300; k = k + 1) {
            got[k] = (float32) y0[k];
            want[k] = (float32) ((bfloat16) (-7.0f));
        }
        for (uint32 r = 0; r < 16; r = r + 1) {
            for (uint32 c = 0; c < 16; c = c + 1) {
                want[S.cell(r, c, layout, off, stride)] = (float32) ((bfloat16) h[r * 16 + c]);
            }
        }
        return S.verdict(got, want);
    }

    public static int32 runF32(uint32 off, uint32 stride, float32 zero) {
        float32[] h = heap float32[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = S.input(i, zero); }
        float32[] got = heap float32[300];
        for (int32 k = 0; k < 300; k = k + 1) { got[k] = -7.0f; }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(300);
        a.upload(h);
        y.upload(got);
        KernelStream s #= KernelStream.current();
        // A device with no matrix unit whose subgroups can't run 32 lanes refuses
        // the portable tile by name (gpu-kernel-parity reason 11): -2.
        try {
            f32Row.launch(s, grid: [1], block: [32])(y, a, off, stride);
            s.sync();
        } catch (XpuLaunchException e) { return -2; }
        y.download(got);
        float32[] want = heap float32[300];
        for (int32 k = 0; k < 300; k = k + 1) { want[k] = -7.0f; }
        for (uint32 r = 0; r < 16; r = r + 1) {
            for (uint32 c = 0; c < 16; c = c + 1) { want[S.cell(r, c, 0, off, stride)] = h[r * 16 + c]; }
        }
        return S.verdict(got, want);
    }
}
)CJ";

const char* kKernels[] = {"f16Row", "f16Col", "f16Shared", "f16SharedRow", "bf16Row",
                          "bf16Col", "bf16Shared", "bf16SharedRow", "f32Row"};

struct Config { int which; unsigned off, stride; const char* what; };
const Config kConfigs[] = {
    {0, 0, 16, "row-major, packed"},
    {0, 11, 17, "row-major, offset 11, odd stride 17"},
    {0, 2, 18, "row-major, offset 2, stride 18"},
    {1, 0, 16, "column-major, packed"},
    {1, 5, 17, "column-major, offset 5, odd stride 17"},
    {2, 0, 0, "a Shared tile at an odd stride"},
    {3, 0, 0, "a row-major Shared tile"},
};

void everyCellConverts(cajeta::xpu::Backend be, const char* name,
                       bool f32Refused = false) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.S", o);
    ASSERT_NE(jit, nullptr);
    auto f16 = jit->lookup<int32_t (*)(int32_t, uint32_t, uint32_t, float)>("runF16");
    auto bf16 = jit->lookup<int32_t (*)(int32_t, uint32_t, uint32_t, float)>("runBF16");
    auto f32 = jit->lookup<int32_t (*)(uint32_t, uint32_t, float)>("runF32");
    ASSERT_TRUE(f16 && bf16 && f32);
    // The verdict is -1, or mismatches * 1000 + the first bad cell.
    for (const Config& c : kConfigs) {
        EXPECT_EQ(-1, f16(c.which, c.off, c.stride, 0.0f)) << name << " float16, " << c.what;
        EXPECT_EQ(-1, bf16(c.which, c.off, c.stride, 0.0f)) << name << " bfloat16, " << c.what;
    }
    // storeAs<float32> IS store, so it keeps store's placement contract: the
    // native wmma store on nvptx needs an aligned tile and a stride that is a
    // multiple of 16 bytes (offset 11 at stride 17 read back 256 wrong cells).
    EXPECT_EQ(f32Refused ? -2 : -1, f32(32, 16, 0.0f))
        << name << (f32Refused ? " storeAs<float32> should be refused by name"
                               : " storeAs<float32>");
}

// Compile `src` for `be` and launch its `probe` under a catch, returning what the
// compile printed.
std::string probeOn(const std::string& src, cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.B", o);
    if (jit)
        if (auto fn = jit->lookup<int32_t (*)()>("probe")) fn();
    return testing::internal::GetCapturedStderr();
}

// One kernel `bad` whose body is `body`, with a probe that launches it.
std::string refusalSource(const std::string& params, const std::string& body,
                          const std::string& launchArgs, const std::string& setup) {
    return std::string(R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.XpuLaunchException;
public class B {
    @Kernel
    public static void bad()CJ") + params + ") {\n" + body + R"CJ(
    }
    public static int32 probe() {
)CJ" + setup + R"CJ(
        KernelStream s #= KernelStream.current();
        try {
            bad.launch(s, grid: [1], block: [32]))CJ" + launchArgs + R"CJ(;
            s.sync();
        } catch (XpuLaunchException e) { }
        return 1;
    }
}
)CJ";
}

struct Refusal { std::string src; const char* what; const char* names; };

std::vector<Refusal> refusals() {
    return {
        {refusalSource("KernelBuffer<int32> y",
                       "CooperativeMatrix<float32,16,16,2> acc;\n"
                       "acc.splat(1.0f);\nacc.storeAs<int32>(y, 0, 0, 16);",
                       "(y)", "KernelBuffer<int32> y = heap KernelBuffer<int32>(256);"),
         "an integer U", "int32"},
        {refusalSource("KernelBuffer<float16> y, KernelBuffer<float16> x",
                       "CooperativeMatrix<float16,16,16,0> t;\n"
                       "t.load(x, 0, 0, 16);\nt.storeAs<float16>(y, 0, 0, 16);",
                       "(y, x)",
                       "KernelBuffer<float16> y = heap KernelBuffer<float16>(256);\n"
                       "KernelBuffer<float16> x = heap KernelBuffer<float16>(256);"),
         "an operand fragment", "accumulator"},
        {refusalSource("KernelBuffer<float16> y",
                       "CooperativeMatrix<int32,16,16,2> acc;\n"
                       "acc.splat(1);\nacc.storeAs<float16>(y, 0, 0, 16);",
                       "(y)", "KernelBuffer<float16> y = heap KernelBuffer<float16>(256);"),
         "an int32 accumulator", "float32 accumulator"},
        {refusalSource("KernelBuffer<bfloat16> y",
                       "CooperativeMatrix<float16,16,16,2> acc;\n"
                       "acc.splat((float16) 1.0f);\nacc.storeAs<bfloat16>(y, 0, 0, 16);",
                       "(y)", "KernelBuffer<bfloat16> y = heap KernelBuffer<bfloat16>(256);"),
         "a float16 accumulator", "float32 accumulator"},
    };
}

} // namespace

TEST(XpuCoopStoreAs, everyCellConvertsAsTheScalarCastOnCpu) {
    everyCellConverts(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuCoopStoreAsDeviceTests, everyCellConvertsAsTheScalarCastOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    everyCellConverts(cajeta::xpu::Backend::Nvptx, "nvptx");
}

// Vulkan goes through the per-subgroup staging tile (decision 3). A device without
// cooperative matrices runs the kernel's portable variant instead (gpu-kernel-parity,
// the `$pt` rung), which converts in the portable tile's store, and says so once.
TEST(XpuCoopStoreAsDeviceTests, everyCellConvertsAsTheScalarCastOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    const bool native = cajeta::xpu::vulkan::VulkanDriver::coopMatrixAvailable();
    // With no matrix unit, the float16 and bfloat16 kernels (staged element access)
    // run their virtual-wave variants; the float32 kernel (load and store only) has
    // only the 32-lane portable tile, which a narrower device refuses by name.
    const bool narrow = !cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(32);
    const bool f32Refused = !native && narrow;
    testing::internal::CaptureStderr();
    everyCellConverts(cajeta::xpu::Backend::Spirv,
                      native ? "vulkan" : narrow ? "vulkan, virtual wave" : "vulkan, portable rung",
                      f32Refused);
    std::string err = testing::internal::GetCapturedStderr();
    if (f32Refused) {
        EXPECT_NE(err.find("[xpu-launch-refused]"), std::string::npos) << err;
        EXPECT_NE(err.find("f32Row"), std::string::npos) << err;
    } else if (!native) {
        EXPECT_NE(err.find("[xpu-rung]"), std::string::npos)
            << "a device without cooperative matrices takes the portable rung by name:\n"
            << err;
    }
}

TEST(XpuCoopStoreAsDeviceTests, everyCellConvertsAsTheScalarCastOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    everyCellConverts(cajeta::xpu::Backend::Amdgpu, "amdgpu");
}

// What it cannot mean is refused by name, on the native tier and on the cpu tile.
TEST(XpuCoopStoreAs, whatItCannotMeanIsRefusedByName) {
    for (const Refusal& r : refusals()) {
        auto l = cajeta::xpu::probe::lowerForNvptx(r.src, "bad", "test.B");
        EXPECT_FALSE(l.ok) << r.what << " lowered on nvptx";
        EXPECT_NE(l.why.find("storeAs"), std::string::npos) << r.what << ": " << l.why;
        EXPECT_NE(l.why.find(r.names), std::string::npos) << r.what << ": " << l.why;
        std::string err = probeOn(r.src, cajeta::xpu::Backend::Cpu);
        EXPECT_TRUE(cajeta_test::loweringRefused(err)) << r.what << " on cpu:\n" << err;
        EXPECT_NE(err.find("storeAs"), std::string::npos) << r.what << " on cpu:\n" << err;
    }
}

// Every kernel lowers on nvptx and on both AMD matrix-core families: RDNA's WMMA
// (gfx1151) and CDNA's MFMA (gfx942). Their values are checked where a device runs
// (the 4090, proton's gfx1151, gem5's MI300X).
TEST(XpuCoopStoreAs, lowersOnNvptxAndBothAmdFamilies) {
    using namespace cajeta::xpu::probe;
    for (const char* k : kKernels) {
        Lowered n = lowerForNvptx(kSource, k, "test.S");
        EXPECT_TRUE(n.ok) << k << " on sm_89: " << n.why;
        for (const char* arch : {"gfx1151", "gfx942"}) {
            Lowered a = lowerForAmdgpu(kSource, k, "test.S", arch);
            EXPECT_TRUE(a.ok) << k << " on " << arch << ": " << a.why;
        }
    }
}

// Unit 2 (spec §2.2): nvptx has its converting forms in the code: wmma's
// converting store for float16 at both layouts and into Shared, and the packed
// pairs for bfloat16.
TEST(XpuCoopStoreAs, nvptxEmitsTheConvertingStores) {
    using namespace cajeta::xpu::probe;
    struct Want { const char* kernel; std::vector<const char*> ptx; };
    const Want wants[] = {
        {"f16Row", {"wmma.store.d.sync.aligned.row.m16n16k16.global.f16", "cvt.rn.f16x2.f32"}},
        {"f16Col", {"wmma.store.d.sync.aligned.col.m16n16k16.global.f16"}},
        {"f16SharedRow", {"wmma.store.d.sync.aligned.row.m16n16k16.shared.f16"}},
        {"bf16Row", {"cvt.rn.bf16x2.f32", "st.global.b32"}},
    };
    for (const Want& w : wants) {
        Lowered l = lowerForNvptx(kSource, w.kernel, "test.S");
        ASSERT_TRUE(l.ok) << w.kernel << ": " << l.why;
        for (const char* p : w.ptx)
            EXPECT_NE(l.ptx.find(p), std::string::npos) << w.kernel << " lacks " << p << "\n"
                                                         << l.ptx;
    }
}

// spirv-val on a SPIR-V binary: nullopt when the tool is not installed.
std::optional<bool> spirvValid(const std::vector<uint8_t>& spirv, const std::string& tag) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    auto path = std::filesystem::temp_directory_path() / ("cajeta_storeas_" + tag + ".spv");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spirv.data()), (std::streamsize) spirv.size());
    }
    std::string file = path.string();
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, "--target-env", "vulkan1.3", file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    return rc == 0;
}

// Unit 3 (spec §2.3): every kernel lowers for vulkan through the staging tile, with
// no refusal, and the module passes spirv-val.
TEST(XpuCoopStoreAs, vulkanLowersThroughTheStagingTileAndValidates) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource, "test.S",
                                                           "storeas_spv");
    auto klass = module->getStructures()["test.S"];
    ASSERT_TRUE(klass);
    for (const char* k : kKernels) {
        auto m = cajeta::xpu::probe::findMethod(klass, k);
        ASSERT_TRUE(m) << k;
        // One target machine a kernel, as VulkanRegistration builds them: the SPIR-V
        // backend keeps state in the machine, and reusing one across modules failed
        // instruction selection about 1 run in 10 (`cannot select: G_ADD ...:iid`).
        auto tm = createSpirvTargetMachine("vulkan1.3");
        ASSERT_NE(tm, nullptr);
        llvm::LLVMContext ctx;
        llvm::Module dev(std::string("storeas_") + k, ctx);
        configureDeviceModule(dev, *tm);
        std::string why;
        try {
            lowerKernel(m, dev);
        } catch (cajeta::Exception& e) {
            why = e.getMessage();
        } catch (const std::exception& e) { why = e.what(); }
        ASSERT_TRUE(why.empty()) << k << " refused on vulkan: " << why;
        std::string failure;
        std::vector<uint8_t> spirv = emitSpirv(dev, *tm, &failure);
        ASSERT_FALSE(spirv.empty()) << k << ": " << failure;
        // Without spirv-val (Phoenix) the lowering is still checked; proton validates.
        if (auto valid = spirvValid(spirv, k)) EXPECT_TRUE(*valid) << "spirv-val rejected " << k;
    }
}
