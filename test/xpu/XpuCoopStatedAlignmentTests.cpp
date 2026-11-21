// A cooperative-matrix load, store or storeAs that states its alignment (coop-stated-
// alignment spec §2). Since 0.54.0 every native load and store on nvptx carries a
// run-time placement guard, because wmma needs each lane's run aligned: 8 bytes for an
// f32 or s32 accumulator, 4 for a 16-bit fragment and an 8-bit operand
// (XpuCoopUnalignedTests). The guard keeps both paths in every site and spilled two
// cajeta-llm wmma kernels on sm_89. `align: A` states that the tile's address and its
// pitch in bytes are multiples of A; where A covers the run, nvptx emits wmma alone.
//
// The per-element form is the only writer of `cajeta_placement_taken`, so a kernel
// whose PTX never stores to it has no guarded site left. The symbol itself can
// outlive a guard the optimizer folded, so the test looks for the store.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.Shared;
import cajeta.xpu.XpuLaunchException;
public class S {
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Store16(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.store(y, off, 0, stride, align: 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Store4(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.store(y, off, 0, stride, align: 4);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Store2(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.store(y, off, 0, stride, align: 2);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32StoreBare(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.store(y, off, 0, stride);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f32Load16(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, off, 0, stride, align: 16);
        acc.store(y, 0, 0, 16, align: 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16A4(KernelBuffer<float32> y, KernelBuffer<float16> a, KernelBuffer<float16> eye,
                             uint32 off, uint32 stride) {
        CooperativeMatrix<float16,16,16,0> ta;
        CooperativeMatrix<float16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(a, off, 0, stride, align: 4);
        tb.load(eye, 0, 0, 16, align: 16);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16, align: 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void s8B16(KernelBuffer<int32> y, KernelBuffer<int8> b, KernelBuffer<int8> eye,
                             uint32 off, uint32 stride) {
        CooperativeMatrix<int8,16,16,0> ta;
        CooperativeMatrix<int8,16,16,1> tb;
        CooperativeMatrix<int32,16,16,2> acc;
        ta.load(eye, 0, 0, 16, align: 16);
        tb.load(b, off, 0, stride, align: 16);
        acc.splat(0);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16, align: 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void storeAsF16(KernelBuffer<float16> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.storeAs<float16>(y, off, 0, stride, align: 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void storeAsBf16(KernelBuffer<bfloat16> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.storeAs<bfloat16>(y, off, 0, stride, align: 4);
    }
    // Shared tiles at constant offsets with a constant stride: the guard folds with no
    // statement (spec §2.2).
    @Kernel @Occupancy(maxThreads = 32)
    public static void sharedConst(KernelBuffer<float32> y) {
        Shared<float16> sa = shared float16[16 * 72];
        Shared<float16> sb = shared float16[16 * 72];
        CooperativeMatrix<float16,16,16,0> ta;
        CooperativeMatrix<float16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(sa, 0, 0, 72);
        tb.load(sb, 16, 0, 72);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16, align: 16);
    }

    static int32 cell(uint32 r, uint32 c, uint32 off, uint32 stride) {
        return (int32) (off + r * stride + c);
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

    // The stated forms at an aligned run-time placement: 0 f32 store, 1 f32 load,
    // 2 f16 A load, 3 s8 B load, 4 storeAs f16, 5 storeAs bf16. -2 when the device
    // refuses the tile by name (a coop-less Vulkan narrower than 32 lanes).
    public static int32 run(int32 which, uint32 off, uint32 stride) {
        try {
            return S.runAt(which, off, stride);
        } catch (XpuLaunchException e) {
            return -2;
        }
    }
    static int32 runAt(int32 which, uint32 off, uint32 stride) {
        float32[] mem = heap float32[600];
        for (int32 m = 0; m < 600; m = m + 1) { mem[m] = (float32) ((m * 7) % 101 - 50); }
        float32[] got = heap float32[600];
        float32[] want = heap float32[600];
        for (int32 k = 0; k < 600; k = k + 1) { got[k] = -7.0f; want[k] = -7.0f; }
        KernelStream s #= KernelStream.current();
        if (which == 0 || which >= 4) {
            // A stored tile: the 16 x 16 source lands at (off, stride).
            for (uint32 r = 0; r < 16; r = r + 1) {
                for (uint32 c = 0; c < 16; c = c + 1) { want[S.cell(r, c, off, stride)] = mem[r * 16 + c]; }
            }
            KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
            a.upload(mem);
            if (which == 0) {
                KernelBuffer<float32> y = heap KernelBuffer<float32>(600);
                y.upload(got);
                f32Store16.launch(s, grid: [1], block: [32])(y, a, off, stride);
                s.sync();
                y.download(got);
            }
            if (which == 4) {
                float16[] h = heap float16[600];
                for (int32 k = 0; k < 600; k = k + 1) { h[k] = (float16) (-7.0f); }
                KernelBuffer<float16> y = heap KernelBuffer<float16>(600);
                y.upload(h);
                storeAsF16.launch(s, grid: [1], block: [32])(y, a, off, stride);
                s.sync();
                y.download(h);
                for (int32 k = 0; k < 600; k = k + 1) { got[k] = (float32) h[k]; }
            }
            if (which == 5) {
                bfloat16[] h = heap bfloat16[600];
                for (int32 k = 0; k < 600; k = k + 1) { h[k] = (bfloat16) (-7.0f); }
                KernelBuffer<bfloat16> y = heap KernelBuffer<bfloat16>(600);
                y.upload(h);
                storeAsBf16.launch(s, grid: [1], block: [32])(y, a, off, stride);
                s.sync();
                y.download(h);
                for (int32 k = 0; k < 600; k = k + 1) { got[k] = (float32) h[k]; }
            }
            return S.verdict(got, want, 600);
        }
        // A loaded tile: the 16 x 16 result is the tile read at (off, stride).
        for (uint32 r = 0; r < 16; r = r + 1) {
            for (uint32 c = 0; c < 16; c = c + 1) { want[r * 16 + c] = mem[S.cell(r, c, off, stride)]; }
        }
        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);
        if (which == 1) {
            KernelBuffer<float32> a = heap KernelBuffer<float32>(600);
            a.upload(mem);
            f32Load16.launch(s, grid: [1], block: [32])(y, a, off, stride);
        }
        if (which == 2) {
            float16[] hm = heap float16[600];
            float16[] he = heap float16[256];
            for (int32 m = 0; m < 600; m = m + 1) { hm[m] = (float16) mem[m]; }
            for (int32 i = 0; i < 256; i = i + 1) { he[i] = (float16) ((i / 16 == i % 16) ? 1.0f : 0.0f); }
            KernelBuffer<float16> a = heap KernelBuffer<float16>(600);
            KernelBuffer<float16> e = heap KernelBuffer<float16>(256);
            a.upload(hm);
            e.upload(he);
            f16A4.launch(s, grid: [1], block: [32])(y, a, e, off, stride);
        }
        if (which == 3) {
            int8[] hm = heap int8[600];
            int8[] he = heap int8[256];
            for (int32 m = 0; m < 600; m = m + 1) { hm[m] = (int8) ((m * 7) % 101 - 50); }
            for (int32 i = 0; i < 256; i = i + 1) { he[i] = (int8) ((i / 16 == i % 16) ? 1 : 0); }
            KernelBuffer<int8> a = heap KernelBuffer<int8>(600);
            KernelBuffer<int8> e = heap KernelBuffer<int8>(256);
            KernelBuffer<int32> yi = heap KernelBuffer<int32>(256);
            a.upload(hm);
            e.upload(he);
            s8B16.launch(s, grid: [1], block: [32])(yi, a, e, off, stride);
            s.sync();
            int32[] gi = heap int32[256];
            yi.download(gi);
            for (int32 k = 0; k < 256; k = k + 1) { got[k] = (float32) gi[k]; }
            return S.verdict(got, want, 256);
        }
        s.sync();
        y.download(got);
        return S.verdict(got, want, 256);
    }
}
)CJ";

bool guarded(const cajeta::xpu::probe::Lowered& l) {
    return l.ptx.find("[cajeta_placement_taken]") != std::string::npos;
}

} // namespace

// A statement that covers every site's run leaves wmma alone in the kernel.
TEST(XpuCoopStatedAlignment, aCoveringStatementDropsTheGuardOnNvptx) {
    using namespace cajeta::xpu::probe;
    struct Want { const char* kernel; const char* ptx; };
    const Want wants[] = {
        {"f32Store16", "wmma.store.d.sync.aligned.row.m16n16k16.global.f32"},
        {"f32Load16", "wmma.load.c.sync.aligned.row.m16n16k16.global.f32"},
        {"f16A4", "wmma.load.a.sync.aligned.row.m16n16k16.global.f16"},
        {"s8B16", "wmma.load.b.sync.aligned.row.m16n16k16.global.s8"},
        {"storeAsF16", "wmma.store.d.sync.aligned.row.m16n16k16.global.f16"},
    };
    for (const Want& w : wants) {
        Lowered l = lowerForNvptx(kSource, w.kernel, "test.S");
        ASSERT_TRUE(l.ok) << w.kernel << ": " << l.why;
        EXPECT_NE(l.ptx.find(w.ptx), std::string::npos) << w.kernel << " lacks " << w.ptx;
        EXPECT_FALSE(guarded(l)) << w.kernel << " still carries the per-element form";
    }
}

// storeAs<bfloat16> row-major has no wmma store; a statement of 4 takes the paired
// store alone: four 4-byte stores of a converted pair per lane.
TEST(XpuCoopStatedAlignment, aStatedBf16StoreAsTakesThePairedStoreAloneOnNvptx) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(kSource, "storeAsBf16", "test.S");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(countOf(l.ptx, "st.global.v2.b16"), 4u) << l.ptx;
    EXPECT_FALSE(guarded(l));
}

// A statement narrower than the run changes nothing, and neither does no statement.
TEST(XpuCoopStatedAlignment, aNarrowerStatementOrNoneKeepsTheGuardOnNvptx) {
    using namespace cajeta::xpu::probe;
    for (const char* k : {"f32Store4", "f32Store2", "f32StoreBare"}) {
        Lowered l = lowerForNvptx(kSource, k, "test.S");
        ASSERT_TRUE(l.ok) << k << ": " << l.why;
        EXPECT_TRUE(guarded(l)) << k << " lost the guard";
        EXPECT_NE(l.ptx.find("wmma.store.d.sync.aligned.row.m16n16k16.global.f32"),
                  std::string::npos) << k;
    }
}

// Shared tiles at constant aligned offsets with a constant stride need no statement.
TEST(XpuCoopStatedAlignment, aConstantAlignedPlacementFoldsTheGuardOnNvptx) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(kSource, "sharedConst", "test.S");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ptx.find("wmma.load.a.sync.aligned.row.m16n16k16.shared.f16"),
              std::string::npos);
    EXPECT_FALSE(guarded(l));
}

namespace {

// `stmt` replaces the store in a one-store kernel.
std::string refusalSource(const std::string& stmt) {
    return std::string(R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
public class R {
    @Kernel @Occupancy(maxThreads = 32)
    public static void k(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 n) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        )CJ") + stmt + R"CJ(
    }
}
)CJ";
}

} // namespace

TEST(XpuCoopStatedAlignment, aBadStatementIsRefusedNamingTheVerb) {
    using namespace cajeta::xpu::probe;
    struct Case { const char* stmt; const char* verb; const char* why; };
    const Case cases[] = {
        {"acc.store(y, off, 0, 16, align: n);", "CooperativeMatrix.store", "constant"},
        {"acc.store(y, off, 0, 16, align: 3);", "CooperativeMatrix.store", "power of two"},
        {"acc.store(y, off, 0, 16, align: 32);", "CooperativeMatrix.store", "power of two"},
        {"acc.load(a, off, 0, 16, align: 0);", "CooperativeMatrix.load", "power of two"},
        {"acc.storeAs<float16>(y, off, 0, 16, align: 6);", "CooperativeMatrix.storeAs",
         "power of two"},
        {"acc.store(y, off, 0, 16, pitch: 16);", "CooperativeMatrix.store", "align:"},
    };
    for (const Case& c : cases) {
        Lowered l = lowerForNvptx(refusalSource(c.stmt), "k", "test.R");
        EXPECT_FALSE(l.ok) << c.stmt;
        EXPECT_NE(l.why.find(c.verb), std::string::npos) << c.stmt << ": " << l.why;
        EXPECT_NE(l.why.find(c.why), std::string::npos) << c.stmt << ": " << l.why;
    }
}

namespace {

// Aligned run-time placements: offsets and strides of 16 elements are 16 bytes or
// more for every element type the kernels use, int8 included.
struct Place { unsigned off, stride; };
const Place kPlaces[] = {{0, 16}, {16, 32}, {32, 16}};

void theStatedFormsAreRight(cajeta::xpu::Backend be, const char* name) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.S", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)(int32_t, uint32_t, uint32_t)>("run");
    ASSERT_TRUE(run);
    const char* what[] = {"f32 store", "f32 load", "f16 A load", "s8 B load", "storeAs f16",
                          "storeAs bf16"};
    for (const Place& p : kPlaces)
        for (int w = 0; w < 6; ++w) {
            const int32_t got = run(w, p.off, p.stride);
            // Phoenix's llvmpipe has no matrix unit and 8-lane subgroups, and refuses
            // the portable tile by name (SpirvPortableTileTests).
            if (got == -2 && be == cajeta::xpu::Backend::Spirv)
                GTEST_SKIP() << name << " refuses the tile: no matrix unit, subgroups under 32";
            EXPECT_EQ(-1, got)
                << name << " " << what[w] << ", offset " << p.off << ", stride " << p.stride;
        }
}

} // namespace

TEST(XpuCoopStatedAlignment, theStatedFormsAreRightOnCpu) {
    theStatedFormsAreRight(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuCoopStatedAlignmentDeviceTests, theStatedFormsAreRightOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    theStatedFormsAreRight(cajeta::xpu::Backend::Nvptx, "nvptx");
}

TEST(XpuCoopStatedAlignmentDeviceTests, theStatedFormsAreRightOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no ROCm device";
    theStatedFormsAreRight(cajeta::xpu::Backend::Amdgpu, "amdgpu");
}

TEST(XpuCoopStatedAlignmentDeviceTests, theStatedFormsAreRightOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    theStatedFormsAreRight(cajeta::xpu::Backend::Spirv, "vulkan");
}

namespace {

std::string fixture(const std::string& name) {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    std::string base = root && *root ? root : CAJETA_SOURCE_ROOT_DEFAULT;
#else
    std::string base = root && *root ? root : ".";
#endif
    std::ifstream in(std::filesystem::path(base) / "test/xpu/fixtures/test" / (name + ".cajeta"));
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// cajeta-llm's two kernels that began to spill on sm_89 under the guard (8 and 24
// bytes) spill nothing once every load and store states `align: 16`. The unstated
// copy is the control: it still spills, so the fixture reproduces the cost.
TEST(XpuCoopStatedAlignment, theLlmWmmaKernelsStopSpillingWhenTheyStateAlignment) {
    using namespace cajeta::xpu::probe;
    const std::string bare = fixture("Wmma");
    const std::string stated = fixture("WmmaStated");
    ASSERT_FALSE(bare.empty());
    ASSERT_FALSE(stated.empty());
    // cajeta-llm's leg builds --release, which is what its spill gate measures. A
    // checked build pays for its statement tests.
    const cajeta::CompilerFlags release =
        cajeta::CompilerFlags::defaultsForMode(cajeta::CompilerMode::Release);
    for (const char* k : {"q4kWmmaDeqMw8Kernel", "q5kWmmaIdMwKernel"}) {
        Lowered lb = lowerForNvptx(bare, k, "test.Wmma", "sm_89", "llm_bare", release);
        Lowered ls = lowerForNvptx(stated, k, "test.WmmaStated", "sm_89", "llm_stated", release);
        ASSERT_TRUE(lb.ok) << k << ": " << lb.why;
        ASSERT_TRUE(ls.ok) << k << ": " << ls.why;
        EXPECT_TRUE(guarded(lb)) << k;
        EXPECT_FALSE(guarded(ls)) << k;
        PtxasFrame fb = ptxasFrame(lb.ptx, k);
        PtxasFrame fs = ptxasFrame(ls.ptx, k);
        if (!fb.ok || !fs.ok) GTEST_SKIP() << "no ptxas 12.1 or newer";
        EXPECT_GT(fb.spillStores, 0u) << k << ": the control no longer spills";
        EXPECT_EQ(fs.spillStores, 0u) << k << " spills " << fs.spillStores << " B with "
                                      << fs.registers << " registers";
    }
}

// ---- Unit 2: a wrong statement is caught in a checked build (spec §2.3) ----------
//
// A build with ubTraps on (--debug) tests each stated tile placement. A violation is
// recorded in the kernel's fault record, the access is skipped so a misaligned wmma
// never faults the context, and the launch raises naming the kernel, the line, the
// statement and the misaligned address or pitch. A release build carries no check.

namespace {

const char* kViolation = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.XpuLaunchException;
public class V {
    @Kernel @Occupancy(maxThreads = 32)
    public static void stated(KernelBuffer<float32> y, KernelBuffer<float32> a, uint32 off, uint32 stride) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16, align: 16);
        acc.store(y, off, 0, stride, align: 16);
    }
    /** 1 when the launch threw, plus 2/4/8/16/32 for each part of the message: the
     *  statement, its line, the kernel, the address, the pitch; 100 + the cells that
     *  differ from the source when it did not throw. */
    public static int32 run(uint32 off, uint32 stride) {
        float32[] h = heap float32[600];
        for (int32 i = 0; i < 600; i = i + 1) { h[i] = (float32) (i + 1); }
        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(600);
        a.upload(h);
        KernelStream s #= KernelStream.current();
        int32 r = 0;
        try {
            stated.launch(s, grid: [1], block: [32])(y, a, off, stride);
            s.sync();
            float32[] got = heap float32[600];
            y.download(got);
            r = 100;
            for (uint32 i = 0; i < 16; i = i + 1) {
                for (uint32 j = 0; j < 16; j = j + 1) {
                    if (got[off + i * stride + j] != h[i * 16 + j]) { r = r + 1; }
                }
            }
        } catch (XpuLaunchException e) {
            String m = e.getMessage();
            System.stderr.println(m);
            r = 1;
            if (m.contains("align: 16")) { r = r + 2; }
            if (m.contains("line 12")) { r = r + 4; }
            if (m.contains("'stated'")) { r = r + 8; }
            if (m.contains("address")) { r = r + 16; }
            if (m.contains("pitch")) { r = r + 32; }
        }
        return r;
    }
}
)CJ";

cajeta::CompilerFlags modeFlags(cajeta::CompilerMode mode, bool ubTraps) {
    cajeta::CompilerFlags f = cajeta::CompilerFlags::defaultsForMode(mode);
    f.ubTraps = ubTraps;
    return f;
}

bool checksPlacement(const cajeta::xpu::probe::Lowered& l) {
    return l.ptx.find("__cajeta_xpu_align_fault") != std::string::npos;
}

void aWrongStatementRaises(cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kViolation, "test.V", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)(uint32_t, uint32_t)>("run");
    ASSERT_NE(run, nullptr);
    // 2 floats is 8 bytes: the tile's address is off the 16-byte statement.
    EXPECT_EQ(run(2, 16), 1 + 2 + 4 + 8 + 16);
    // A pitch of 18 floats is 72 bytes, also off it.
    EXPECT_EQ(run(0, 18), 1 + 2 + 4 + 8 + 32);
    // The record is read after the launch, so the next conforming launch runs.
    EXPECT_EQ(run(4, 20), 100);
}

} // namespace

TEST(XpuCoopStatedAlignment, aCheckedBuildTestsTheStatementOnNvptx) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(kViolation, "stated", "test.V", "sm_89", "coop_chk",
                              modeFlags(cajeta::CompilerMode::Debug, true));
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_TRUE(checksPlacement(l)) << l.ptx;
    // The checked build keeps the guard-free wmma: the check skips, it does not gather.
    EXPECT_FALSE(guarded(l));
}

TEST(XpuCoopStatedAlignment, aReleaseBuildCarriesNoCheckOnNvptx) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(kViolation, "stated", "test.V", "sm_89", "coop_rel",
                              modeFlags(cajeta::CompilerMode::Release, false));
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_FALSE(checksPlacement(l)) << l.ptx;
    EXPECT_FALSE(guarded(l));
}

TEST(XpuCoopStatedAlignment, aWrongStatementRaisesNamingTheCallOnCpu) {
    aWrongStatementRaises(cajeta::xpu::Backend::Cpu);
}

TEST(XpuCoopStatedAlignmentDeviceTests, aWrongStatementRaisesNamingTheCallOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    aWrongStatementRaises(cajeta::xpu::Backend::Nvptx);
}

TEST(XpuCoopStatedAlignmentDeviceTests, aWrongStatementRaisesNamingTheCallOnAmdgpu) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no ROCm device";
    aWrongStatementRaises(cajeta::xpu::Backend::Amdgpu);
}

// ---- Unit 3: the docs' example (CajetaXPU.md §3.3, "Tile placement") -------------

TEST(XpuCoopStatedAlignment, theDocsExampleLowersToWmmaAloneOnNvptx) {
    using namespace cajeta::xpu::probe;
    const char* src = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
public class D {
    @Kernel @Occupancy(maxThreads = 32)
    public static void doc(KernelBuffer<float16> y, KernelBuffer<float16> xs, KernelBuffer<float16> w,
                           uint32 offset, uint32 stride, uint32 n) {
        CooperativeMatrix<float16,16,16,0> a;
        CooperativeMatrix<float16,16,16,1> b;
        CooperativeMatrix<float32,16,16,2> acc;
        acc.splat(0.0f);
        a.load(xs, offset, 0, stride, align: 16);
        b.load(w, 0, 0, 16, align: 16);
        acc.mma(a, b);
        acc.storeAs<float16>(y, offset, 0, n, align: 16);
    }
}
)CJ";
    Lowered l = lowerForNvptx(src, "doc", "test.D");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_FALSE(guarded(l));
    EXPECT_NE(l.ptx.find("wmma.store.d.sync.aligned.row.m16n16k16.global.f16"),
              std::string::npos);
}
