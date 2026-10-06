// A finished operation becomes library code (xpu-kernel-independence Unit
// 10, spec §4.2, §4.3): `TileOps.scaledAccumI32`, the Q4_K tile's integer
// fold written over the building blocks (per-element fragment access with
// the lane map, a Shared panel), against the built-in verb.
//
// Two kernels of the Q4_K tile's shape (256 threads, eight waves, four
// int8 products per wave folded into int32 accumulators by a per-column
// scale from a Shared panel once per K-slice) differ only in who spells the
// fold. They must agree bit for bit, on cpu and on the nvptx device, and
// through the reference interpreter; and the library's time must be within
// 3% of the built-in's at the engine's prefill shape on sm_89, interleaved
// (spec §7.4). A library that needs a compiler verb to be fast is not a
// library, so a miss here is a codegen defect to fix, not a bound to widen.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "XpuRefusalProbe.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/reference/Conformance.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

// `fold` is the one line that differs: the built-in verb, or the library.
std::string program(const std::string& foldPrefix) {
    std::string fold;
    for (int t = 0; t < 4; ++t) {
        std::string n = std::to_string(t);
        fold += foldPrefix == "builtin"
            ? "            mc" + n + ".scaledAccumI32(iacc" + n + ", scv" + n + ");\n"
            : "            TileOps.scaledAccumI32(mc" + n + ", iacc" + n + ", scAll, cBase + "
                  + std::to_string(128 * t) + " + j, 8);\n";
    }
    return R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelTimer;
import cajeta.xpu.Shared;
import cajeta.xpu.TileOps;
import cajeta.xpu.WaveVector;
public class M {
    // Per wave: y[16 x 64] = sum over slices of (A[16x16] . B_t[16x16]) * sc,
    // four tiles side by side, A and B int8 from global, sc from a Shared
    // panel at scAll[cBase + 128 t + j + c * 8] as the Q4_K tile reads it.
    @Kernel
    public static void fold(KernelBuffer<int32> y, KernelBuffer<int8> a, KernelBuffer<int8> b,
                            KernelBuffer<int32> scales, uint32 slices) {
        Shared<int32> scAll = shared int32[2048];
        uint32 tid = KernelThread.x();
        uint32 wid = tid / 32;
        uint32 wg = KernelThread.globalIdX() / 256;
        for (uint32 k = tid; k < 2048; k = k + 256) { scAll[k] = scales[k]; }
        Barrier.workgroup();
        uint32 cBase = (wid % 2) * 512;
        CooperativeMatrix<int8,16,16,0> ma;
        CooperativeMatrix<int8,16,16,1> mb0;
        CooperativeMatrix<int8,16,16,1> mb1;
        CooperativeMatrix<int8,16,16,1> mb2;
        CooperativeMatrix<int8,16,16,1> mb3;
        CooperativeMatrix<int32,16,16,2> mc0;
        CooperativeMatrix<int32,16,16,2> mc1;
        CooperativeMatrix<int32,16,16,2> mc2;
        CooperativeMatrix<int32,16,16,2> mc3;
        CooperativeMatrix<int32,16,16,2> iacc0;
        CooperativeMatrix<int32,16,16,2> iacc1;
        CooperativeMatrix<int32,16,16,2> iacc2;
        CooperativeMatrix<int32,16,16,2> iacc3;
        iacc0.splat(0);
        iacc1.splat(0);
        iacc2.splat(0);
        iacc3.splat(0);
        ma.load(a, wid * 256, 0, 16);
        mb0.load(b, wid * 1024, 1, 16);
        mb1.load(b, wid * 1024 + 256, 1, 16);
        mb2.load(b, wid * 1024 + 512, 1, 16);
        mb3.load(b, wid * 1024 + 768, 1, 16);
        for (uint32 s = 0; s < slices; s = s + 1) {
            uint32 j = s % 8;
            WaveVector<int32, 16> scv0 = WaveVector.ofSlice(scAll, cBase + j, 8);
            WaveVector<int32, 16> scv1 = WaveVector.ofSlice(scAll, cBase + 128 + j, 8);
            WaveVector<int32, 16> scv2 = WaveVector.ofSlice(scAll, cBase + 256 + j, 8);
            WaveVector<int32, 16> scv3 = WaveVector.ofSlice(scAll, cBase + 384 + j, 8);
            mc0.splat(0);
            mc1.splat(0);
            mc2.splat(0);
            mc3.splat(0);
            mc0.mma(ma, mb0);
            mc1.mma(ma, mb1);
            mc2.mma(ma, mb2);
            mc3.mma(ma, mb3);
)CJ" + fold + R"CJ(
        }
        uint32 yBase = (wg * 8 + wid) * 1024;
        iacc0.store(y, yBase, 0, 64);
        iacc1.store(y, yBase + 16, 0, 64);
        iacc2.store(y, yBase + 32, 0, 64);
        iacc3.store(y, yBase + 48, 0, 64);
    }
    // One program entry: mode 0 is a checksum of y, 1 the element at `at`,
    // 2 the device-timed nanoseconds of `launches` back-to-back launches
    // (-1 when the timer has no device clock).
    public static int64 go(uint32 grid, uint32 slices, uint32 launches, int32 mode, int32 at) {
        int32 n = (int32) grid * 8192;
        int8[] ha = heap int8[2048];
        for (int32 i = 0; i < 2048; i = i + 1) { ha[i] = (int8) ((i * 7 + 3) % 16 - 8); }
        int8[] hb = heap int8[8192];
        for (int32 i = 0; i < 8192; i = i + 1) { hb[i] = (int8) ((i * 7 + 5) % 16 - 8); }
        int32[] hs = heap int32[2048];
        for (int32 i = 0; i < 2048; i = i + 1) { hs[i] = (i * 13) % 63 + 1; }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(2048);
        KernelBuffer<int8> b = heap KernelBuffer<int8>(8192);
        KernelBuffer<int32> sc = heap KernelBuffer<int32>(2048);
        a.upload(ha);
        b.upload(hb);
        sc.upload(hs);
        KernelBuffer<int32> y = heap KernelBuffer<int32>((uint64) n);
        int32[] h = heap int32[n];
        y.upload(h);
        KernelStream s #= KernelStream.current();
        fold.launch(s, grid: [grid], block: [256])(y, a, b, sc, slices);
        s.sync();
        if (mode == 2) {
            KernelTimer t #= KernelTimer.create();
            t.begin(s);
            for (uint32 i = 0; i < launches; i = i + 1) {
                fold.launch(s, grid: [grid], block: [256])(y, a, b, sc, slices);
            }
            t.end(s);
            int64 ns = t.elapsedNanos();
            s.sync();
            t.destroy();
            return ns;
        }
        y.download(h);
        if (mode == 1) { return (int64) h[at]; }
        int64 tot = 0;
        for (int32 i = 0; i < n; i = i + 1) { tot = tot + (int64) h[i] * (int64) (i % 97 + 1); }
        return tot;
    }
}
)CJ";
}

struct Variant {
    std::unique_ptr<CajetaJit> jit;
    using Go = int64_t (*)(unsigned, unsigned, unsigned, int32_t, int32_t);
    Go go = nullptr;
    int64_t sum(unsigned grid, unsigned slices) { return go(grid, slices, 0, 0, 0); }
    int64_t at(unsigned grid, unsigned slices, int32_t i) { return go(grid, slices, 0, 1, i); }
    int64_t timeNs(unsigned grid, unsigned slices, unsigned launches) {
        return go(grid, slices, launches, 2, 0);
    }
    Variant(cajeta::xpu::Backend be, const char* which) {
        CajetaJit::Options o;
        o.xpuBackends = {be};
        jit = CajetaJit::compile(program(which), "test.M", o);
        EXPECT_NE(jit, nullptr) << which;
        if (!jit) return;
        go = jit->lookup<Go>("go");
        EXPECT_NE(go, nullptr);
    }
};

void agreeBitForBit(cajeta::xpu::Backend be, const char* backend) {
    Variant builtin(be, "builtin"), library(be, "library");
    ASSERT_TRUE(builtin.go && library.go);
    // The ragged TcTile shape's worth of workgroups, 64 K-slices.
    EXPECT_EQ(builtin.sum(6, 64), library.sum(6, 64)) << backend;
    EXPECT_NE(builtin.sum(6, 64), 0) << backend << ": the fold wrote nothing";
    for (int32_t i : {0, 1, 17, 63, 64, 1023, 1024, 8191, 8192 * 5 + 777})
        EXPECT_EQ(builtin.at(6, 64, i), library.at(6, 64, i)) << backend << " element " << i;
}

} // namespace

// 4.10.1.1: bit for bit against the built-in.
TEST(XpuLibraryEpilogue, theLibraryFoldAgreesWithTheBuiltInOnCpu) {
    agreeBitForBit(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuLibraryEpilogue, theLibraryFoldAgreesWithTheBuiltInOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    agreeBitForBit(cajeta::xpu::Backend::Nvptx, "nvptx");
}

// The library helper, with its fragment and Shared parameters, is defined in
// the reference too: both kernels recorded on cpu replay and pass.
TEST(XpuLibraryEpilogue, bothFoldsPassTheCorpus) {
    fs::path dir = fs::temp_directory_path() / "cajeta-library-epilogue-corpus";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const char* which : {"builtin", "library"}) {
        CajetaJit::Options o;
        o.xpuBackends = {cajeta::xpu::Backend::Cpu};
        auto jit = CajetaJit::compile(program(which), "test.M", o);
        ASSERT_NE(jit, nullptr);
        auto fn = jit->lookup<Variant::Go>("go");
        ASSERT_NE(fn, nullptr);
        setenv("CAJETA_XPU_RECORD", (dir / which).string().c_str(), 1);
        fn(1, 4, 0, 0, 0);
        unsetenv("CAJETA_XPU_RECORD");
        cajeta::Compiler compiler;
        auto module = cajeta::xpu::probe::compileForInspection(compiler, program(which));
        std::vector<cajeta::MethodPtr> kernels;
        if (auto k = findKernel(module, "test.M", "fold")) kernels.push_back(k);
        ASSERT_EQ(kernels.size(), 1u) << which;
        ref::CorpusRun run = ref::runCorpus(kernels, (dir / which).string(), "");
        ASSERT_FALSE(run.results.empty()) << which << ": nothing recorded";
        for (auto& r : run.results)
            EXPECT_EQ(r.outcome, "pass") << which << ": " << r.kernel << ": " << r.detail;
    }
    fs::remove_all(dir);
}

// 4.10.1.2: within 3% of the built-in on sm_89, interleaved: fifteen rounds
// of a twenty-launch device-timed bracket per variant, alternating the
// order, and the medians compared. The shape is the engine's prefill tile count (the
// 256 x 384 x 1024 ragged shape is six workgroups; 128 fills the device) at
// 64 K-slices.
TEST(XpuLibraryEpilogue, theLibraryFoldIsWithinThreePercentOfTheBuiltInOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    Variant builtin(cajeta::xpu::Backend::Nvptx, "builtin");
    Variant library(cajeta::xpu::Backend::Nvptx, "library");
    ASSERT_TRUE(builtin.go && library.go);
    // 512 K-slices per launch, so a launch is well above the launch floor
    // (6 to 13 us on this box) and the fold is a real share of the time.
    // A warm round each, untimed, then fifteen timed rounds of twenty
    // launches, the order alternating; the device clock's states are what the
    // interleaving and the medians are for.
    builtin.timeNs(128, 512, 20);
    library.timeNs(128, 512, 20);
    std::vector<int64_t> tb, tl;
    for (int round = 0; round < 15; ++round) {
        if (round % 2 == 0) {
            tb.push_back(builtin.timeNs(128, 512, 20));
            tl.push_back(library.timeNs(128, 512, 20));
        } else {
            tl.push_back(library.timeNs(128, 512, 20));
            tb.push_back(builtin.timeNs(128, 512, 20));
        }
    }
    ASSERT_GT(tb.front(), 0) << "no device clock";
    std::sort(tb.begin(), tb.end());
    std::sort(tl.begin(), tl.end());
    int64_t mb = tb[tb.size() / 2], ml = tl[tl.size() / 2];
    std::printf("[library-epilogue] built-in %lld ns, library %lld ns per 20 launches (%.2f%%)\n",
                (long long) mb, (long long) ml, 100.0 * ((double) ml / (double) mb - 1.0));
    EXPECT_LE((double) ml, 1.03 * (double) mb)
        << "the library fold is slower than the built-in by more than 3%";
}

// A helper taking a fragment must get one of the declared shape: a mismatch
// is refused by name, never bound to the wrong tile.
TEST(XpuLibraryEpilogue, aFragmentArgumentOfAnotherShapeIsRefusedByName) {
    using namespace cajeta::xpu::probe;
    std::string src = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.TileOps;
public class M {
    @Kernel
    public static void wrong(KernelBuffer<float32> y) {
        Shared<int32> sc = shared int32[16];
        CooperativeMatrix<float32,16,16,2> acc;
        CooperativeMatrix<int32,16,16,2> iacc;
        acc.splat(1.0f);
        iacc.splat(0);
        TileOps.scaledAccumI32(acc, iacc, sc, 0, 1);
        iacc.store(y, 0, 0, 16);
    }
}
)CJ";
    Lowered l = lowerForNvptx(src, "wrong");
    EXPECT_FALSE(l.ok);
    EXPECT_NE(l.why.find("acc"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("float32"), std::string::npos) << l.why;
}

// With CAJETA_PROBE_PTX=<dir> set, writes both kernels' PTX and device IR
// there, so a timing miss can be read off the instruction stream.
TEST(XpuLibraryEpilogue, dumpsThePtxWhenAsked) {
    const char* dir = std::getenv("CAJETA_PROBE_PTX");
    if (!dir) GTEST_SKIP() << "set CAJETA_PROBE_PTX=<dir> to dump";
    using namespace cajeta::xpu::probe;
    for (const char* w : {"builtin", "library"}) {
        Lowered l = lowerForNvptx(program(w), "fold");
        ASSERT_TRUE(l.ok) << l.why;
        std::ofstream(std::string(dir) + "/" + w + ".ptx") << l.ptx;
        std::ofstream(std::string(dir) + "/" + w + ".ll") << l.ir;
    }
}
