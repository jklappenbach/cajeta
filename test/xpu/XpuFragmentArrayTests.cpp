//
// Arrays of cooperative-matrix fragments live in registers
// (xpu-tile-shape-selection plan Unit 2, spec §2.2).
//
// A tile written over its shape holds its accumulators as an array,
// `CooperativeMatrix<float32,16,16,2>[2][4] acc;`, and walks it in `for` loops
// whose trip is a compile-time constant. The lowering unrolls those loops, so
// every index is a constant, and each element is one fragment slot: the array
// is eight named fragments by another spelling, and must cost exactly what
// they cost. An index that is not a compile-time constant would need the
// array in addressable memory, so it is refused by name instead.
//
// The fixture is a 32x64 GEMM tile over K = 32: 2x4 accumulators, one warp,
// a runtime K loop around the constant-trip fragment loops. `named` is the
// same tile written with eight named accumulators, generated below so the
// two cannot drift apart.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "KernelLoweringProbe.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using namespace cajeta::xpu::probe;

namespace {

const char* kHead =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "public final class D {\n";

// A is 32x32 row-major, B is 32x64 row-major, C is 32x64 row-major.
const char* kArrayKernel =
    "    @Kernel\n"
    "    public static void arr(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
    "                           KernelBuffer<float16> b, uint32 ktiles) {\n"
    "        CooperativeMatrix<float32,16,16,2>[2][4] acc;\n"
    "        for (uint32 i = 0; i < 2; i = i + 1) {\n"
    "            for (uint32 j = 0; j < 4; j = j + 1) { acc[i][j].splat(0.0f); }\n"
    "        }\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        uint32 kk = 0;\n"
    "        while (kk < ktiles) {\n"
    "            for (uint32 i = 0; i < 2; i = i + 1) {\n"
    "                ma.load(a, i * 16 * 32 + kk * 16, 0, 32);\n"
    "                for (uint32 j = 0; j < 4; j = j + 1) {\n"
    "                    mb.load(b, kk * 16 * 64 + j * 16, 0, 64);\n"
    "                    acc[i][j].mma(ma, mb);\n"
    "                }\n"
    "            }\n"
    "            kk = kk + 1;\n"
    "        }\n"
    "        for (uint32 i = 0; i < 2; i = i + 1) {\n"
    "            for (uint32 j = 0; j < 4; j = j + 1) {\n"
    "                acc[i][j].store(c, i * 16 * 64 + j * 16, 0, 64);\n"
    "            }\n"
    "        }\n"
    "    }\n";

std::string namedKernel() {
    std::string s =
        "    @Kernel\n"
        "    public static void named(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
        "                             KernelBuffer<float16> b, uint32 ktiles) {\n";
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 4; ++j) {
            const std::string n = "c" + std::to_string(i) + std::to_string(j);
            s += "        CooperativeMatrix<float32,16,16,2> " + n + ";\n";
            s += "        " + n + ".splat(0.0f);\n";
        }
    s += "        CooperativeMatrix<float16,16,16,0> ma;\n"
         "        CooperativeMatrix<float16,16,16,1> mb;\n"
         "        uint32 kk = 0;\n"
         "        while (kk < ktiles) {\n";
    for (int i = 0; i < 2; ++i) {
        s += "            ma.load(a, " + std::to_string(i * 16 * 32) +
             " + kk * 16, 0, 32);\n";
        for (int j = 0; j < 4; ++j) {
            s += "            mb.load(b, kk * 16 * 64 + " + std::to_string(j * 16) +
                 ", 0, 64);\n";
            s += "            c" + std::to_string(i) + std::to_string(j) +
                 ".mma(ma, mb);\n";
        }
    }
    s += "            kk = kk + 1;\n"
         "        }\n";
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 4; ++j)
            s += "        c" + std::to_string(i) + std::to_string(j) + ".store(c, " +
                 std::to_string(i * 16 * 64 + j * 16) + ", 0, 64);\n";
    s += "    }\n";
    return s;
}

// Runs both tiles and returns a mask: 1 the array tile is wrong against the
// host, 2 the named tile is, 4 the two differ anywhere.
const char* kRun =
    "    static int32 wrong(float32[] got, float32[] want) {\n"
    "        int32 bad = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < 2048) { if (got[i] != want[i]) { bad = bad + 1; } i = i + 1; }\n"
    "        return bad;\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float16[] ha = heap float16[1024];\n"
    "        float16[] hb = heap float16[2048];\n"
    "        float32[] ref = heap float32[2048];\n"
    "        int32 i = 0;\n"
    "        while (i < 1024) { ha[i] = (float16) (float32) ((i % 7) - 3); i = i + 1; }\n"
    "        i = 0;\n"
    "        while (i < 2048) { hb[i] = (float16) (float32) ((i % 5) - 2); i = i + 1; }\n"
    "        i = 0;\n"
    "        while (i < 32) {\n"
    "            int32 j = 0;\n"
    "            while (j < 64) {\n"
    "                float32 acc = 0.0f;\n"
    "                int32 k = 0;\n"
    "                while (k < 32) {\n"
    "                    acc = acc + (float32) (((i * 32 + k) % 7) - 3)\n"
    "                              * (float32) (((k * 64 + j) % 5) - 2);\n"
    "                    k = k + 1;\n"
    "                }\n"
    "                ref[i * 64 + j] = acc;\n"
    "                j = j + 1;\n"
    "            }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        KernelBuffer<float16> a = heap KernelBuffer<float16>(1024);\n"
    "        KernelBuffer<float16> b = heap KernelBuffer<float16>(2048);\n"
    "        KernelBuffer<float32> c = heap KernelBuffer<float32>(2048);\n"
    "        a.upload(ha);\n"
    "        b.upload(hb);\n"
    "        float32[] outA = heap float32[2048];\n"
    "        float32[] outN = heap float32[2048];\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        i = 0;\n"
    "        while (i < 2048) { outA[i] = -1.0f; i = i + 1; }\n"
    "        c.upload(outA);\n"
    "        arr.launch(s, grid: [1], block: [32])(c, a, b, 2);\n"
    "        s.sync();\n"
    "        c.download(outA);\n"
    "        i = 0;\n"
    "        while (i < 2048) { outN[i] = -1.0f; i = i + 1; }\n"
    "        c.upload(outN);\n"
    "        named.launch(s, grid: [1], block: [32])(c, a, b, 2);\n"
    "        s.sync();\n"
    "        c.download(outN);\n"
    "        int32 mask = 0;\n"
    "        if (D.wrong(outA, ref) > 0) { mask = mask + 1; }\n"
    "        if (D.wrong(outN, ref) > 0) { mask = mask + 2; }\n"
    "        if (D.wrong(outA, outN) > 0) { mask = mask + 4; }\n"
    "        return mask;\n"
    "    }\n"
    "}\n";

std::string program() {
    return std::string(kHead) + kArrayKernel + namedKernel() + kRun;
}

int runOn(cajeta::xpu::Backend backend) {
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(program(), "test.D", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

// One kernel over a fragment array, for the refusals.
std::string refusalSource(const std::string& body) {
    return std::string(kHead) +
        "    @Kernel\n"
        "    public static void bad(KernelBuffer<float32> c, uint32 n) {\n"
        "        CooperativeMatrix<float32,16,16,2>[2][4] acc;\n" + body +
        "    }\n"
        "}\n";
}

} // namespace

// 4.2.1.1: the array tile is the named tile, bit for bit, and both are right.
TEST(XpuFragmentArray, anArrayOfFragmentsMatchesNamedFragmentsOnSm89) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Nvptx), 0)
        << "1: array tile wrong, 2: named tile wrong, 4: the two differ";
}

// The software tile path takes the same slots.
TEST(XpuFragmentArray, anArrayOfFragmentsMatchesNamedFragmentsOnCpu) {
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Cpu), 0)
        << "1: array tile wrong, 2: named tile wrong, 4: the two differ";
}

// 4.2.1.2 and 4.2.3.1: no local frame, and the registers of the named tile.
TEST(XpuFragmentArray, anArrayOfFragmentsHasNoFrameAndTheNamedRegisterCount) {
    const Lowered arr = lowerForNvptx(program(), "arr", "test.D");
    ASSERT_TRUE(arr.ok) << arr.why;
    const Lowered named = lowerForNvptx(program(), "named", "test.D");
    ASSERT_TRUE(named.ok) << named.why;
    const PtxasFrame fa = ptxasFrame(arr.ptx, "arr");
    const PtxasFrame fn = ptxasFrame(named.ptx, "named");
    if (!fa.ok || !fn.ok) GTEST_SKIP() << "ptxas absent or too old on this box";
    EXPECT_EQ(fa.stackFrame, 0u) << "the fragment array went to the local frame";
    EXPECT_EQ(fa.spillStores, 0u);
    EXPECT_LE(fa.registers, fn.registers + 2)
        << "array " << fa.registers << " registers, named " << fn.registers;
    EXPECT_GE(fa.registers + 2, fn.registers)
        << "array " << fa.registers << " registers, named " << fn.registers;
}

// 4.2.1.3: a loop whose trip is not a compile-time constant cannot index a
// fragment array.
TEST(XpuFragmentArray, aLoopWithARuntimeTripIsRefusedByName) {
    const Lowered l = lowerForNvptx(refusalSource(
        "        for (uint32 i = 0; i < n; i = i + 1) { acc[i][0].splat(0.0f); }\n"),
        "bad", "test.D");
    ASSERT_FALSE(l.ok) << "a runtime index into a fragment array lowered";
    EXPECT_NE(l.why.find("fragment array 'acc'"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("compile-time constant"), std::string::npos) << l.why;
}

TEST(XpuFragmentArray, anIndexPastTheEndIsRefusedByName) {
    const Lowered l = lowerForNvptx(refusalSource(
        "        acc[2][0].splat(0.0f);\n"), "bad", "test.D");
    ASSERT_FALSE(l.ok) << "an out-of-range fragment index lowered";
    EXPECT_NE(l.why.find("fragment array 'acc'"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("out of range"), std::string::npos) << l.why;
}

// `T[2][4]` is two rows of four: [1][3] is in range, and the counter of an
// unrolled loop cannot be assigned in its body.
TEST(XpuFragmentArray, theLastIndexIsTheInnerDimension) {
    const Lowered ok = lowerForNvptx(refusalSource(
        "        acc[1][3].splat(0.0f);\n"
        "        acc[1][3].store(c, 0, 0, 16);\n"), "bad", "test.D");
    EXPECT_TRUE(ok.ok) << ok.why;
    const Lowered bad = lowerForNvptx(refusalSource(
        "        acc[3][1].splat(0.0f);\n"), "bad", "test.D");
    EXPECT_FALSE(bad.ok) << "[3][1] is out of range for [2][4]";
}

TEST(XpuFragmentArray, theCounterOfAnUnrolledLoopIsNotAssignable) {
    const Lowered l = lowerForNvptx(refusalSource(
        "        for (uint32 i = 0; i < 2; i = i + 1) {\n"
        "            acc[i][0].splat(0.0f);\n"
        "            i = 1;\n"
        "        }\n"), "bad", "test.D");
    ASSERT_FALSE(l.ok);
    EXPECT_NE(l.why.find("'i'"), std::string::npos) << l.why;
}
