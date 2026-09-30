//
// NvptxWordFormTests — byte vectors on NVPTX are WORDS
// (xpu-kernel-adaptor Unit 12, the device-time half of the llama.cpp gap).
//
// Why this exists. Measured 2026-09-30 on sm_89: a decode token of the 8B
// reference model is 356 launches and about 8.5 ms of DEVICE time, where
// llama.cpp's whole token is 6.2 ms, and replaying the launches through a
// CUDA graph (the submit floor gone) moved the token by 3%. The device time
// is the kernels, and the PTX of the hot one names it:
//
//     q4kQ8WaveMatVecKernel   260 ld.global.b8   258 mad.lo.s32   244 prmt.b32
//                               0 dp4a
//
// Two lowerings, both generic defaults that NVPTX never overrode:
//
//   * `Vector<int8,N>.dotAccum` / `dotSum` / `Group.mac` went through the
//     portable widening reduce: four extends, four multiplies and the adds
//     per 4 bytes. PTX has the instruction, `dp4a`, in all four signedness
//     pairings (`llvm.nvvm.idp4a.{s,u}.{s,u}`), and it is what llama.cpp's
//     mmvq is built on.
//
//   * `KernelBuffer<int8>.vload<N>` loads `<N x i8>` at alignment 1, and
//     NVPTX may not read a word at an unaligned address, so the backend
//     legalized every such load into N byte loads and reassembled them with
//     `prmt`. When the index is PROVABLY a multiple of four the load is a
//     word load; the quantized layouts are word-aligned by construction
//     (a Q4_K block is 144 bytes with its quants at +16), so the proof is
//     there in nearly every kernel. The buffer's base then has to be
//     word-aligned too, which the launch checks and refuses by name.
//
// A check needs tests that assert it fires and tests that assert it does not:
// an index the compiler cannot prove stays a byte load.
//

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace cajeta::xpu::probe;
using cajeta_test::CajetaJit;

namespace {

// dotU:  unsigned weights against signed activations, the dotAccum contract
//        every quantized mat-vec uses, at an index that is a multiple of 32.
// dotS:  signed against signed.
// loose: the same dot at an index that is a runtime argument plus the work
//        item: nothing proves it a multiple of four.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dotU(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<uint8,32> wv = w.vload<32>((int64) i * 32L).asUnsigned();\n"
    "            Vector<int8,32> xv = x.vload<32>((int64) i * 32L);\n"
    "            Vector<int32,8> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void dotS(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<int8,32> wv = w.vload<32>((int64) i * 32L);\n"
    "            Vector<int8,32> xv = x.vload<32>((int64) i * 32L);\n"
    "            Vector<int32,8> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void loose(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n, uint32 off) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<uint8,32> wv = w.vload<32>((int64) (i + off)).asUnsigned();\n"
    "            Vector<int8,32> xv = x.vload<32>((int64) (i + off));\n"
    "            Vector<int32,8> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    /** Sixteen bytes at an index that is a multiple of four but not of sixteen. */\n"
    "    @Kernel\n"
    "    public static void words(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,4> zv = heap Vector<int32,4>(0, 0, 0, 0);\n"
    "            Vector<uint8,16> wv = w.vload<16>((int64) i * 4L).asUnsigned();\n"
    "            Vector<int8,16> xv = x.vload<16>((int64) i * 4L);\n"
    "            Vector<int32,4> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3];\n"
    "        }\n"
    "    }\n"
    "    /** A loop-carried offset that starts at a multiple of 32 and steps by 32. */\n"
    "    @Kernel\n"
    "    public static void loopAligned(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n, uint32 trips) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<int32,8> t = zv;\n"
    "            uint32 o = i * 64;\n"
    "            uint32 k = 0;\n"
    "            while (k < trips) {\n"
    "                Vector<uint8,32> wv = w.vload<32>((int64) o).asUnsigned();\n"
    "                Vector<int8,32> xv = x.vload<32>((int64) o);\n"
    "                t = t + wv.dotAccum(xv, zv);\n"
    "                o = o + 32;\n"
    "                k = k + 1;\n"
    "            }\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    /** The same loop from a start nothing proves: the STEP is aligned, the value is not. */\n"
    "    @Kernel\n"
    "    public static void loopOdd(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n, uint32 trips,\n"
    "            uint32 start) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<int32,8> t = zv;\n"
    "            uint32 o = start;\n"
    "            uint32 k = 0;\n"
    "            while (k < trips) {\n"
    "                o = o + 32;\n"
    "                Vector<uint8,32> wv = w.vload<32>((int64) o).asUnsigned();\n"
    "                Vector<int8,32> xv = x.vload<32>((int64) o);\n"
    "                t = t + wv.dotAccum(xv, zv);\n"
    "                k = k + 1;\n"
    "            }\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

Lowered lower(const char* kernel) {
    return lowerForNvptx(kSource, kernel, "test.M", "sm_89", "wordform");
}

// The device arm: the same three kernels launched against a host reference,
// over bytes that cover the sign bit on both operands, plus a launch whose
// buffer base is NOT word-aligned.
const char* kRunSource =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class R {\n"
    "    @Kernel\n"
    "    public static void dotU(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<uint8,32> wv = w.vload<32>((int64) i * 32L).asUnsigned();\n"
    "            Vector<int8,32> xv = x.vload<32>((int64) i * 32L);\n"
    "            Vector<int32,8> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void dotS(KernelBuffer<int32> out,\n"
    "            KernelBuffer<int8> w, KernelBuffer<int8> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<int8,32> wv = w.vload<32>((int64) i * 32L);\n"
    "            Vector<int8,32> xv = x.vload<32>((int64) i * 32L);\n"
    "            Vector<int32,8> t = wv.dotAccum(xv, zv);\n"
    "            out[(int64) i] = t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7];\n"
    "        }\n"
    "    }\n"
    "    static int32 rows() { return 64; }\n"
    "    static void fill(int8[] w, int8[] x) {\n"
    "        int32 i = 0;\n"
    "        int32 n = R.rows() * 32;\n"
    "        while (i < n) {\n"
    "            w[i] = (int8) ((i * 37 + 11) & 255);\n"
    "            x[i] = (int8) ((i * 91 + 200) & 255);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return;\n"
    "    }\n"
    "    /** Rows where the device disagrees with the host, for unsigned (1) or signed (0) weights. */\n"
    "    public static int32 mismatches(int32 unsignedW) {\n"
    "        int32 rows = R.rows();\n"
    "        int8[] w #= heap int8[rows * 32];\n"
    "        int8[] x #= heap int8[rows * 32];\n"
    "        R.fill(w, x);\n"
    "        KernelBuffer<int8> wb = heap KernelBuffer<int8>(0, (uint32) (rows * 32));\n"
    "        KernelBuffer<int8> xb = heap KernelBuffer<int8>(0, (uint32) (rows * 32));\n"
    "        KernelBuffer<int32> ob = heap KernelBuffer<int32>(0, (uint32) rows);\n"
    "        wb.allocate(); xb.allocate(); ob.allocate();\n"
    "        wb.upload(w); xb.upload(x);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        if (unsignedW != 0) {\n"
    "            dotU.launch(s, grid: [1], block: [64])(ob, wb, xb, (uint32) rows);\n"
    "        } else {\n"
    "            dotS.launch(s, grid: [1], block: [64])(ob, wb, xb, (uint32) rows);\n"
    "        }\n"
    "        s.sync();\n"
    "        int32[] got #= heap int32[rows];\n"
    "        ob.download(got);\n"
    "        int32 bad = 0;\n"
    "        int32 r = 0;\n"
    "        while (r < rows) {\n"
    "            int32 want = 0;\n"
    "            int32 k = 0;\n"
    "            while (k < 32) {\n"
    "                int32 wv = (int32) w[r * 32 + k];\n"
    "                if (unsignedW != 0) { wv = wv & 255; }\n"
    "                want = want + wv * (int32) x[r * 32 + k];\n"
    "                k = k + 1;\n"
    "            }\n"
    "            if (got[r] != want) { bad = bad + 1; }\n"
    "            r = r + 1;\n"
    "        }\n"
    "        wb.free(); xb.free(); ob.free();\n"
    "        return bad;\n"
    "    }\n"
    "    /**\n"
    "     * The same launch over a weight buffer sliced ONE BYTE in: its base is\n"
    "     * not word-aligned. Returns the launch failures it added, plus 10 when\n"
    "     * the launch threw an exception that says why.\n"
    "     */\n"
    "    public static int64 misalignedBaseFailures() {\n"
    "        int32 rows = R.rows();\n"
    "        KernelBuffer<int8> wb = heap KernelBuffer<int8>(0, (uint32) (rows * 32 + 64));\n"
    "        KernelBuffer<int8> xb = heap KernelBuffer<int8>(0, (uint32) (rows * 32));\n"
    "        KernelBuffer<int32> ob = heap KernelBuffer<int32>(0, (uint32) rows);\n"
    "        wb.allocate(); xb.allocate(); ob.allocate();\n"
    "        KernelBuffer<int8> odd #= wb.slice(1, (uint64) (rows * 32));\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Device.launchFailures();\n"
    "        int64 thrown = 0;\n"
    "        try {\n"
    "            dotU.launch(s, grid: [1], block: [64])(ob, odd, xb, (uint32) rows);\n"
    "        } catch (XpuLaunchException e) {\n"
    "            if (e.getMessage().contains(\"not aligned\")) { thrown = 10; }\n"
    "        }\n"
    "        s.sync();\n"
    "        int64 after = Device.launchFailures();\n"
    "        wb.free(); xb.free(); ob.free();\n"
    "        return after - before + thrown;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(NvptxWordForm, dotAccumIsDp4aForUnsignedWeights) {
    const Lowered l = lower("dotU");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_GE(countOf(l.ptx, "dp4a.u32.s32"), 8u)
        << "32 bytes of unsigned weights against signed activations are eight "
           "dp4a.u32.s32\n" << l.ptx;
    // One mad is the work-item id (ctaid * ntid + tid); the dot has none.
    EXPECT_LE(countOf(l.ptx, "mad.lo.s32"), 1u)
        << "the dot still multiplies byte by byte\n" << l.ptx;
}

TEST(NvptxWordForm, dotAccumIsDp4aForSignedWeights) {
    const Lowered l = lower("dotS");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_GE(countOf(l.ptx, "dp4a.s32.s32"), 8u) << l.ptx;
    EXPECT_EQ(countOf(l.ptx, "dp4a.u32.s32"), 0u)
        << "signed weights took the unsigned instruction, which reads -1 as 255\n"
        << l.ptx;
}

TEST(NvptxWordForm, aByteVectorAtAProvablyAlignedIndexLoadsWords) {
    const Lowered l = lower("dotU");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(countOf(l.ptx, "ld.global.b8"), 0u)
        << "an index of i * 32 is a multiple of four and the load is still "
           "byte by byte\n" << l.ptx;
    EXPECT_EQ(countOf(l.ptx, "prmt.b32"), 0u)
        << "bytes are still being reassembled into words\n" << l.ptx;
    // ... and a multiple of sixteen: 32 bytes of weights and 32 of activations
    // are four 16-byte vector loads, which move four times the bytes of a
    // word load per instruction.
    EXPECT_GE(countOf(l.ptx, "ld.global.v4.b32"), 4u)
        << "an index of i * 32 is a multiple of sixteen and the loads are not "
           "16-byte vectors\n" << l.ptx;
}

// An index that is a multiple of four but not of sixteen gets words, never
// vectors: the proof credits exactly the zeros the arithmetic has.
TEST(NvptxWordForm, anIndexAlignedToFourLoadsWordsNotVectors) {
    const Lowered l = lower("words");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(countOf(l.ptx, "ld.global.b8"), 0u) << l.ptx;
    EXPECT_EQ(countOf(l.ptx, "ld.global.v4.b32"), 0u)
        << "an index of i * 4 was credited with sixteen-byte alignment\n" << l.ptx;
    EXPECT_GE(countOf(l.ptx, "ld.global.b32"), 8u) << l.ptx;
}

// Does NOT fire: nothing proves `i + off` a multiple of four, and a word load
// at an unaligned address is a device fault, so the bytes stay bytes.
TEST(NvptxWordForm, anUnprovableIndexStaysAByteLoad) {
    const Lowered l = lower("loose");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_GE(countOf(l.ptx, "ld.global.b8"), 64u)
        << "a load at an index nothing proves aligned was widened to words\n"
        << l.ptx;
    // The dot itself does not depend on the load: still dp4a.
    EXPECT_GE(countOf(l.ptx, "dp4a.u32.s32"), 8u) << l.ptx;
}

// A loop-carried offset: provable when its start and its step both are.
TEST(NvptxWordForm, aLoopCarriedOffsetIsProvedFromItsStartAndStep) {
    const Lowered l = lower("loopAligned");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(countOf(l.ptx, "ld.global.b8"), 0u) << l.ptx;
}

// ... and NOT when only the step is. The value loaded at is `start + 32 k`, and
// an analysis that credits the incremented value with the step's zeros while
// the carried value is still assumed aligned would widen this load and fault
// the device on an odd `start`.
TEST(NvptxWordForm, aLoopCarriedOffsetFromAnUnprovenStartStaysBytes) {
    const Lowered l = lower("loopOdd");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_GE(countOf(l.ptx, "ld.global.b8"), 64u)
        << "a load at start + 32k was widened with nothing known about start\n"
        << l.ptx;
}

TEST(NvptxWordForm, theWordFormAgreesWithTheHostOnTheDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(kRunSource, "test.R", o);
    ASSERT_NE(jit, nullptr);
    auto mismatches = jit->lookup<int32_t (*)(int32_t)>("mismatches");
    ASSERT_NE(mismatches, nullptr);
    EXPECT_EQ(mismatches(1), 0) << "unsigned weights: the device dot disagrees with the host";
    EXPECT_EQ(mismatches(0), 0) << "signed weights: the device dot disagrees with the host";
}

// A kernel that reads a buffer in words needs the buffer's base word-aligned.
// A base that is not is refused at the launch, by name and counted, instead of
// faulting the context with a misaligned address.
TEST(NvptxWordForm, aMisalignedBaseIsRefusedAtTheLaunch) {
    CAJETA_SKIP_IF_NO_CUDA();
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(kRunSource, "test.R", o);
    ASSERT_NE(jit, nullptr);
    auto misaligned = jit->lookup<int64_t (*)()>("misalignedBaseFailures");
    auto mismatches = jit->lookup<int32_t (*)(int32_t)>("mismatches");
    ASSERT_TRUE(misaligned && mismatches);
    EXPECT_EQ(misaligned(), 11) << "a launch over a buffer based one byte off a word was not "
                                   "refused (1), or was refused without saying why (10)";
    // ... and the context is intact: the aligned launch still runs and agrees.
    EXPECT_EQ(mismatches(1), 0) << "the refused launch poisoned the context";
}
