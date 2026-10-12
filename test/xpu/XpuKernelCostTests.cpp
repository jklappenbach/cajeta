// xpu-tile-manifest Unit 4: the work model derived from the kernel body (spec §4).
#include "gtest/gtest.h"

#include "KernelLoweringProbe.h"

#include "cajeta/xpu/core/KernelCost.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::MethodPtr;
using cajeta::xpu::KernelBufferCost;
using cajeta::xpu::KernelCost;
using cajeta::xpu::KernelManifest;
using cajeta::xpu::evaluateCost;
using cajeta::xpu::probe::compileForInspection;

namespace {

const char* kImports =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Group;\n"
    "import cajeta.xpu.GroupOp;\n"
    "import cajeta.xpu.Tile;\n"
    "public class M {\n";

std::string src(const std::string& body) { return std::string(kImports) + body + "}\n"; }

// Witness B (XpuCoopWitnessDeviceTests): one group per row, the lanes stripe the columns.
const char* kWitnessB =
    "    @Kernel\n"
    "    public static void ssq(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
    "                           uint32 rows, int32 cols) {\n"
    "        int32 r = Group.rowId();\n"
    "        if ((uint32) r < rows) {\n"
    "            float32 acc = 0.0f;\n"
    "            for (int32 j : Group.stripe(cols)) {\n"
    "                float32 v = a[(int64) r * (int64) cols + (int64) j];\n"
    "                acc = acc + v * v;\n"
    "            }\n"
    "            float32 total = Group.reduce(GroupOp.Add, acc);\n"
    "            if (Group.laneId() == 0) { out[r] = total; }\n"
    "        }\n"
    "    }\n";

// The Tile f16 GEMM (XpuTileTypeDeviceTests): one 16x16x16 tile per group.
const char* kTileGemm =
    "    @Kernel\n"
    "    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
    "                            KernelBuffer<float32> c) {\n"
    "        Tile<float16,16,16> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        Tile<float16,16,16> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        Tile<float32,16,16> mc;\n"
    "        mc.splat(0.0f);\n"
    "        Group.mac(mc, ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n";

// Witness A (cajeta-llm QuantKernel.mxfp4MatVecKernelCoopQ8Surface), its helpers inlined
// as statics of this class: 17-byte MXFP4 blocks against Q8 activations.
const char* kWitnessA =
    "    static float32 scaleOf(int32 e) {\n"
    "        int64 bits;\n"
    "        if (e < 2) {\n"
    "            bits = 0x00200000L << (int64) e;\n"
    "        } else {\n"
    "            bits = (int64) (e - 1) << 23;\n"
    "        }\n"
    "        return Cajeta.bitsToF32(bits);\n"
    "    }\n"
    "    static Vector<int8,16> kvOf() {\n"
    "        return stack Vector<int8,16>(0, 1, 2, 3, 4, 6, 8, 12,\n"
    "                                     0, -1, -2, -3, -4, -6, -8, -12);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void mxfp4(KernelBuffer<float32> y,\n"
    "            @Stored(bits = 4) KernelBuffer<int8> packed, KernelBuffer<int8> xq,\n"
    "            KernelBuffer<float32> xd,\n"
    "            uint32 rows, int64 blocksPerRow, int64 yOff) {\n"
    "        uint32 row = KernelThread.globalIdX() / (uint32) Group.width();\n"
    "        if (row < rows) {\n"
    "            int64 i = (int64) row;\n"
    "            int64 rowBytes = blocksPerRow * 17L;\n"
    "            Vector<int8,16> kv = M.kvOf();\n"
    "            float32 acc = 0.0f;\n"
    "            for (int32 bb : Group.stripe((int32) blocksPerRow)) {\n"
    "                int64 b = (int64) bb;\n"
    "                int64 ro = i * rowBytes + b * 17L;\n"
    "                float32 dw = M.scaleOf((int32) packed[ro] & 255);\n"
    "                float32 dx = xd[b];\n"
    "                Vector<int8,16> v = packed.vload<16>(ro + 1L);\n"
    "                Vector<int8,16> dl = (v & 15).lut4(kv);\n"
    "                Vector<int8,16> dh = ((v >> 4) & 15).lut4(kv);\n"
    "                int64 xb = b * 32L;\n"
    "                Vector<int8,16> aLo = xq.vload<16>(xb);\n"
    "                Vector<int8,16> aHi = xq.vload<16>(xb + 16L);\n"
    "                int32 s = 0;\n"
    "                s = Group.mac(s, dl, aLo);\n"
    "                s = Group.mac(s, dh, aHi);\n"
    "                acc = acc + (float32) s * dw * dx;\n"
    "            }\n"
    "            float32 total = Group.reduce(GroupOp.Add, acc);\n"
    "            if (Group.laneId() == 0) { y[yOff + i] = total; }\n"
    "        }\n"
    "    }\n";

// A loop bound loaded from a buffer: the trip count is data, so the work is measured.
const char* kDataBound =
    "    @Kernel\n"
    "    public static void dataBound(KernelBuffer<float32> x, KernelBuffer<int32> len,\n"
    "                                 KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            float32 acc = 0.0f;\n"
    "            for (int32 j = 0; j < len[0]; j = j + 1) { acc = acc + x[j] * x[j]; }\n"
    "            out[i] = acc;\n"
    "        }\n"
    "    }\n";

// A hand kernel: no Group, a constant bound and an argument bound.
const char* kHand =
    "    @Kernel\n"
    "    public static void hand(KernelBuffer<float32> x, KernelBuffer<float32> y,\n"
    "                            float32 a, uint32 n, int32 k) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i >= n) { return; }\n"
    "        for (int32 j = 0; j < 8; j = j + 1) {\n"
    "            y[i * 8 + (uint32) j] = a * x[i * 8 + (uint32) j];\n"
    "        }\n"
    "        float32 acc = 0.0f;\n"
    "        for (int32 j = 0; j < k; j = j + 1) { acc = acc + a * a; }\n"
    "        y[i] = acc;\n"
    "    }\n";

MethodPtr kernelOf(Compiler& compiler, const std::string& source, const std::string& name) {
    auto module = compileForInspection(compiler, source, "test.M", "cost");
    auto klass = module->getStructures()["test.M"];
    if (!klass) return nullptr;
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

const KernelBufferCost* bufferOf(const KernelCost& c, const std::string& param) {
    for (const auto& b : c.buffers)
        if (b.param == param) return &b;
    return nullptr;
}

double eval(const std::string& expr, const std::map<std::string, double>& args) {
    bool ok = false;
    double v = evaluateCost(expr, args, &ok);
    EXPECT_TRUE(ok) << "not an evaluable cost expression: '" << expr << "'";
    return v;
}

} // namespace

// 4.1.1: per group, cols multiply-adds and 4*cols bytes of `a`, as expressions in cols.
TEST(XpuKernelCost, witnessBCountsTheStripePerGroup) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kWitnessB), "ssq");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 32u);
    EXPECT_EQ(c.costUnit, "group");
    EXPECT_EQ(c.vectorMacs, "cols");
    EXPECT_EQ(c.matrixMacs, "0");
    EXPECT_EQ(c.intOps, "0");
    const KernelBufferCost* a = bufferOf(c, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->bytesRead, "4*cols");
    EXPECT_EQ(a->bytesWritten, "0");
    EXPECT_EQ(a->storedBits, 32u);
    const KernelBufferCost* out = bufferOf(c, "out");
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->bytesRead, "0");
    EXPECT_EQ(out->bytesWritten, "4") << "the lane-0 commit runs once per group";
    ASSERT_TRUE(c.workAxisParam.has_value());
    EXPECT_EQ(*c.workAxisParam, "rows");
    EXPECT_EQ(eval(a->bytesRead, {{"cols", 48}}), 192.0);
}

// 4.1.1: the group count does not depend on the width the target folds the group to.
TEST(XpuKernelCost, witnessBIsTheSameAtEveryWidth) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kWitnessB), "ssq");
    ASSERT_NE(k, nullptr);
    for (unsigned w : {1u, 8u, 32u, 64u}) {
        KernelCost c = cajeta::xpu::deriveKernelCost(k, w);
        EXPECT_EQ(c.vectorMacs, "cols") << "width " << w;
        EXPECT_EQ(bufferOf(c, "out")->bytesWritten, "4") << "width " << w;
    }
}

// 4.1.1 through the manifest: the cpu record carries the work model and its JSON round-trips.
TEST(XpuKernelCost, theManifestCarriesTheWorkModel) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kWitnessB), "ssq");
    ASSERT_NE(k, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_cost_host_cpu", ctx);
    std::vector<KernelManifest> ms;
    cajeta::xpu::cpu::emitKernelRegistration({k}, host, "", &ms);
    ASSERT_EQ(ms.size(), 1u);
    ASSERT_TRUE(ms[0].work.has_value());
    EXPECT_EQ(ms[0].work->vectorMacs, "cols");
    std::string json = cajeta::xpu::toJson(ms[0]);
    EXPECT_NE(json.find("\"work\""), std::string::npos) << json;
    KernelManifest back;
    std::string err;
    ASSERT_TRUE(cajeta::xpu::fromJson(json, back, &err)) << err;
    ASSERT_TRUE(back.work.has_value());
    EXPECT_EQ(back.work->costUnit, "group");
    EXPECT_EQ(back.work->vectorMacs, "cols");
    EXPECT_EQ(back.work->workAxisParam.value_or(""), "rows");
    ASSERT_EQ(back.work->buffers.size(), 2u);
    EXPECT_EQ(back.work->buffers[1].param, "a");
    EXPECT_EQ(back.work->buffers[1].bytesRead, "4*cols");
    EXPECT_EQ(back.work->buffers[1].storedBits, 32u);
}

// 4.1.2: matrix multiply-adds are 16*16*16 per group and kept apart from vector ones.
TEST(XpuKernelCost, tileGemmSeparatesMatrixFromVector) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kTileGemm), "wmma");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 32u);
    EXPECT_EQ(c.costUnit, "group");
    EXPECT_EQ(eval(c.matrixMacs, {}), 16.0 * 16.0 * 16.0);
    EXPECT_EQ(c.vectorMacs, "0");
    EXPECT_EQ(eval(bufferOf(c, "a")->bytesRead, {}), 16.0 * 16.0 * 2.0);
    EXPECT_EQ(eval(bufferOf(c, "b")->bytesRead, {}), 16.0 * 16.0 * 2.0);
    EXPECT_EQ(eval(bufferOf(c, "c")->bytesWritten, {}), 16.0 * 16.0 * 4.0);
    EXPECT_EQ(bufferOf(c, "c")->bytesRead, "0");
}

// 4.1.3: the packed weights are stored at 4 bits, the activations at 8, the bytes are
// the bytes the body moves, and the work axis is rows.
TEST(XpuKernelCost, witnessARecordsStoredWidthsAndTheWorkAxis) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kWitnessA), "mxfp4");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 32u);
    EXPECT_EQ(c.costUnit, "group");
    const KernelBufferCost* packed = bufferOf(c, "packed");
    const KernelBufferCost* xq = bufferOf(c, "xq");
    const KernelBufferCost* xd = bufferOf(c, "xd");
    const KernelBufferCost* y = bufferOf(c, "y");
    ASSERT_TRUE(packed && xq && xd && y);
    EXPECT_EQ(packed->storedBits, 4u);
    EXPECT_EQ(xq->storedBits, 8u);
    EXPECT_EQ(xd->storedBits, 32u);
    EXPECT_EQ(packed->bytesRead, "17*blocksPerRow");
    EXPECT_EQ(xq->bytesRead, "32*blocksPerRow");
    EXPECT_EQ(xd->bytesRead, "4*blocksPerRow");
    EXPECT_EQ(y->bytesWritten, "4");
    EXPECT_EQ(c.intOps, "32*blocksPerRow") << "two 16-lane dp4a macs per block, no index math";
    EXPECT_EQ(c.vectorMacs, "2*blocksPerRow");
    EXPECT_EQ(c.matrixMacs, "0");
    ASSERT_TRUE(c.workAxisParam.has_value());
    EXPECT_EQ(*c.workAxisParam, "rows");
}

// 4.1.4: a loop bound loaded from a buffer leaves the fields it feeds measured.
TEST(XpuKernelCost, aDataDependentBoundIsMeasured) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kDataBound), "dataBound");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 1u);
    EXPECT_EQ(c.costUnit, "item");
    EXPECT_EQ(c.vectorMacs, "measured");
    EXPECT_EQ(bufferOf(c, "x")->bytesRead, "measured");
    EXPECT_EQ(bufferOf(c, "out")->bytesWritten, "4") << "the store after the loop is not in it";
    EXPECT_EQ(bufferOf(c, "len")->bytesRead, "measured") << "the bound is read once per trip";
    EXPECT_EQ(c.matrixMacs, "0");
}

// 4.1.4: a hand kernel is counted per work item from constant and argument bounds.
TEST(XpuKernelCost, aHandKernelStillGetsExpressions) {
    Compiler compiler;
    auto k = kernelOf(compiler, src(kHand), "hand");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 1u);
    EXPECT_EQ(c.costUnit, "item");
    EXPECT_EQ(c.vectorMacs, "k+8");
    EXPECT_EQ(c.intOps, "0") << "i * 8 is only an index";
    EXPECT_EQ(bufferOf(c, "x")->bytesRead, "32");
    EXPECT_EQ(bufferOf(c, "y")->bytesWritten, "36");
    ASSERT_TRUE(c.workAxisParam.has_value());
    EXPECT_EQ(*c.workAxisParam, "n");
}

// The runtime's reading of a cost string: + - * / and parentheses over the arguments.
TEST(XpuKernelCost, costExpressionsEvaluateAgainstArguments) {
    EXPECT_EQ(eval("4*cols", {{"cols", 10}}), 40.0);
    EXPECT_EQ(eval("k+8", {{"k", 3}}), 11.0);
    EXPECT_EQ(eval("(rows+1)*cols/2", {{"rows", 3}, {"cols", 6}}), 12.0);
    EXPECT_EQ(eval("0", {}), 0.0);
    bool ok = true;
    evaluateCost("measured", {}, &ok);
    EXPECT_FALSE(ok);
    ok = true;
    evaluateCost("4*missing", {{"cols", 1}}, &ok);
    EXPECT_FALSE(ok) << "an unbound name is not zero";
}

// 4.0.1: straight-line code in a group kernel runs once per lane, so it counts the folded width.
TEST(XpuKernelCost, straightLineGroupCodeCountsTheWidth) {
    const char* body =
        "    @Kernel\n"
        "    public static void lanes(KernelBuffer<float32> out, KernelBuffer<float32> x, float32 a) {\n"
        "        int32 i = Group.rowId() * Group.width() + Group.laneId();\n"
        "        out[i] = a * x[i];\n"
        "    }\n";
    Compiler compiler;
    auto k = kernelOf(compiler, src(body), "lanes");
    ASSERT_NE(k, nullptr);
    KernelCost c32 = cajeta::xpu::deriveKernelCost(k, 32u);
    EXPECT_EQ(c32.costUnit, "group");
    EXPECT_EQ(c32.vectorMacs, "32");
    EXPECT_EQ(bufferOf(c32, "x")->bytesRead, "128");
    EXPECT_EQ(c32.intOps, "0") << "rowId * width is only an index";
    KernelCost c8 = cajeta::xpu::deriveKernelCost(k, 8u);
    EXPECT_EQ(c8.vectorMacs, "8");
    KernelCost none = cajeta::xpu::deriveKernelCost(k, std::nullopt);
    EXPECT_EQ(none.vectorMacs, "measured") << "a target with no folded width cannot count lanes";
}

// 4.2.1: a static final constant of the kernel's class is a tile constant, folded into the count.
TEST(XpuKernelCost, aStaticFinalBoundIsATileConstant) {
    const char* body =
        "    static final int32 STEP = 16;\n"
        "    @Kernel\n"
        "    public static void sumsq(KernelBuffer<float32> x, KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            float32 acc = 0.0f;\n"
        "            for (int32 j = 0; j < STEP; j = j + 1) { acc = acc + x[j] * x[j]; }\n"
        "            out[i] = acc;\n"
        "        }\n"
        "    }\n";
    Compiler compiler;
    auto k = kernelOf(compiler, src(body), "sumsq");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 1u);
    EXPECT_EQ(c.vectorMacs, "16");
    EXPECT_EQ(bufferOf(c, "x")->bytesRead, "128");
}

// 4.2.1: a class template's non-type parameter is the instantiation's constant.
TEST(XpuKernelCost, aTemplateParameterBoundIsItsInstantiationsConstant) {
    const char* source =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Tile<uint32 TM> {\n"
        "    public Tile() { return; }\n"
        "    @Kernel\n"
        "    public static void fill(KernelBuffer<float32> out, float32 a) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        for (uint32 j = 0; j < TM; j = j + 1) { out[i * TM + j] = a * (float32) j; }\n"
        "    }\n"
        "    public void run() {\n"
        "        KernelBuffer<float32> out = heap KernelBuffer<float32>(0, 64);\n"
        "        out.allocate();\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        fill.launch(s, grid: [1], block: [1])(out, 2.0f);\n"
        "        s.sync();\n"
        "        out.free();\n"
        "    }\n"
        "}\n"
        "public class M {\n"
        "    public static void go() { Tile<4> a #= heap Tile<4>(); a.run(); }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, source, "test.M", "cost");
    MethodPtr k;
    for (auto& [canon, klass] : module->getStructures()) {
        if (canon != "test.Tile<4>" || !klass) continue;
        for (auto& [key, m] : klass->getMethods())
            if (m && m->getName() == "fill") k = m;
    }
    ASSERT_NE(k, nullptr) << "no test.Tile<4>.fill instantiation";
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 1u);
    EXPECT_EQ(c.vectorMacs, "4");
    EXPECT_EQ(bufferOf(c, "out")->bytesWritten, "16");
}

// 4.2.1: a counted while (one declaration, one top-level step) has a trip count like a for.
TEST(XpuKernelCost, aCountedWhileHasATripCount) {
    const char* body =
        "    @Kernel\n"
        "    public static void mv(KernelBuffer<float32> y, KernelBuffer<float32> w,\n"
        "                          KernelBuffer<float32> x, uint32 rows, int64 cols) {\n"
        "        uint32 gi = KernelThread.globalIdX();\n"
        "        if (gi < rows) {\n"
        "            int64 i = (int64) gi;\n"
        "            int64 base = i * cols;\n"
        "            Vector<float32,4> acc = x.vload<4>(0L) * 0.0f;\n"
        "            int64 c #= 0;\n"
        "            while (c < cols) {\n"
        "                acc = acc + w.vload<4>(base + c) * x.vload<4>(c);\n"
        "                c = c + 4L;\n"
        "            }\n"
        "            y[i] = acc[0] + acc[1] + acc[2] + acc[3];\n"
        "        }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void stepTwice(KernelBuffer<float32> y, int64 n) {\n"
        "        int64 c = 0;\n"
        "        while (c < n) { y[c] = 1.0f; c = c + 1L; c = c + 1L; }\n"
        "    }\n";
    Compiler compiler;
    auto k = kernelOf(compiler, src(body), "mv");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 1u);
    EXPECT_EQ(c.vectorMacs, "cols+4") << "four lanes per step over cols/4 steps, plus the zeroing";
    EXPECT_EQ(bufferOf(c, "w")->bytesRead, "4*cols");
    EXPECT_EQ(bufferOf(c, "x")->bytesRead, "4*cols+16");
    EXPECT_EQ(c.intOps, "0") << "i * cols is only a base address";
    auto k2 = kernelOf(compiler, src(body), "stepTwice");
    ASSERT_NE(k2, nullptr);
    EXPECT_EQ(bufferOf(cajeta::xpu::deriveKernelCost(k2, 1u), "y")->bytesWritten, "measured")
        << "a counter stepped twice is not a counted loop";
}

// §4.3a: a branch that is not the bound guard or the lane-0 guard leaves what it costs measured,
// since one expression cannot say which arm ran. A branch that costs nothing changes nothing.
TEST(XpuKernelCost, aBranchThatIsNotAGuardIsMeasured) {
    const char* body =
        "    @Kernel\n"
        "    public static void lanes(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        uint32 lane = KernelThread.x();\n"
        "        uint32 j = i;\n"
        "        if (j > n) { j = n; }\n"
        "        if (i < n) {\n"
        "            y[i] = 2.0f * x[j];\n"
        "            if (lane < n) { y[i] = y[i] * 3.0f; } else { y[i] = 0.0f; }\n"
        "        }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void pick(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n, uint32 k) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { y[i] = k > 4 ? x[i] * 2.0f : 1.0f; }\n"
        "    }\n";
    Compiler compiler;
    auto k = kernelOf(compiler, src(body), "lanes");
    ASSERT_NE(k, nullptr);
    KernelCost c = cajeta::xpu::deriveKernelCost(k, 1u);
    EXPECT_EQ(bufferOf(c, "x")->bytesRead, "4") << "the clamp costs nothing, the guarded read is exact";
    EXPECT_EQ(bufferOf(c, "y")->bytesWritten, "measured") << "a lane-divergent arm writes y";
    EXPECT_EQ(c.vectorMacs, "measured");
    ASSERT_TRUE(c.workAxisParam.has_value());
    EXPECT_EQ(*c.workAxisParam, "n");
    auto k2 = kernelOf(compiler, src(body), "pick");
    ASSERT_NE(k2, nullptr);
    KernelCost c2 = cajeta::xpu::deriveKernelCost(k2, 1u);
    EXPECT_EQ(bufferOf(c2, "x")->bytesRead, "measured") << "a ternary arm reads x";
    EXPECT_EQ(bufferOf(c2, "y")->bytesWritten, "4");
}
