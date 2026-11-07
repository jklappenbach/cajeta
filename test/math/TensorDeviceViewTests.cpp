//
// TensorDeviceViewTests — cajeta-ml-gpu Unit 3 (3.1.1–3.1.3; spec §3.1, §3.5).
//
// A device op on a view reads the view's elements, not its buffer's from element
// 0; a strided view is refused by name; a view whose offset breaks the device's
// binding alignment is refused with the alignment in the message.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "../xpu/XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::unique_ptr<CajetaJit> compileOn(const std::string& src, cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    return CajetaJit::compile(src, "test.D", o);
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n"
    "import cajeta.math.PlacementException;\n"
    "import cajeta.error.Exception;\n";

// t = [1..8] on the device; v = t[2:6] = [3,4,5,6], w = t[4:8] = [5,6,7,8].
const char* VIEWS =
    "        float32[] d = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 8;\n"
    "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
    "        t.gpu();\n"
    "        Tensor<float32> v #= t.slice(0, 2, 6);\n"
    "        Tensor<float32> w #= t.slice(0, 4, 8);\n";

} // namespace

// 3.1.1 — every Ewise entry that takes a tensor reads an offset view's elements.
// Each check returns its own code, so a failure names the entry.
//
// RED until 3.2.1: the entries bind `deviceBuffer()` from element 0, so a view
// of [3,4,5,6] reads as [1,2,3,4].
TEST(TensorDeviceViewTests, offsetViewsReachEveryEwiseEntry) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n" + VIEWS +
        "        if (Ewise.sumF32(v) != 18.0f) { return 1; }\n"
        "        if (Ewise.minF32(v) != 3.0f) { return 2; }\n"
        "        if (Ewise.maxF32(v) != 6.0f) { return 3; }\n"
        "        float32 fro = Ewise.normFroF32(v);\n"
        "        if (fro * fro < 85.9f || fro * fro > 86.1f) { return 4; }\n"
        "        int64[] ix = [ 1L, 3L ];\n"
        "        int64[] is = heap int64[1]; is[0] = 2;\n"
        "        Tensor<int64> idx #= Tensor.of<int64>(ix, is);\n"
        "        idx.gpu();\n"
        "        Tensor<float32> g #= Ewise.takeF32(v, idx);\n"
        "        g.cpu();\n"
        "        if (g.get1(0) != 4.0f || g.get1(1) != 6.0f) { return 5; }\n"
        "        Tensor<float32> cs #= Ewise.cumsumF32Op(v);\n"
        "        cs.cpu();\n"
        "        if (cs.get1(3) != 18.0f || cs.get1(0) != 3.0f) { return 6; }\n"
        "        Tensor<float32> cat #= Ewise.concatF32Op(v, w);\n"
        "        cat.cpu();\n"
        "        if (cat.size() != 8 || cat.get1(0) != 3.0f || cat.get1(4) != 5.0f || cat.get1(7) != 8.0f) { return 7; }\n"
        "        Tensor<float32> sl #= Ewise.sliceF32Op(v, 1, 2);\n"
        "        sl.cpu();\n"
        "        if (sl.get1(0) != 4.0f || sl.get1(1) != 5.0f) { return 8; }\n"
        "        Tensor<float32> so #= Ewise.sortF32Op(w);\n"
        "        so.cpu();\n"
        "        if (so.get1(0) != 5.0f || so.get1(3) != 8.0f) { return 9; }\n"
        "        if (!t.isOnGpu()) { return 10; }\n"
        // matmul: rows 1..2 of a 4x2 matrix (a contiguous view at offset 2) times 2x2.
        "        float32[] md = [ 1.0f, 1.0f, 1.0f, 2.0f, 3.0f, 4.0f, 9.0f, 9.0f ];\n"
        "        int64[] ms = heap int64[2]; ms[0] = 4; ms[1] = 2;\n"
        "        Tensor<float32> m #= Tensor.of<float32>(md, ms);\n"
        "        m.gpu();\n"
        "        Tensor<float32> rows #= m.slice(0, 1, 3);\n"
        "        float32[] bd = [ 1.0f, 0.0f, 0.0f, 1.0f ];\n"
        "        int64[] bs = heap int64[2]; bs[0] = 2; bs[1] = 2;\n"
        "        Tensor<float32> b #= Tensor.of<float32>(bd, bs);\n"
        "        b.gpu();\n"
        "        Tensor<float32> p #= Ewise.matmulF32Op(rows, b);\n"
        "        p.cpu();\n"
        "        if (p.get2(0, 0) != 1.0f || p.get2(0, 1) != 2.0f || p.get2(1, 0) != 3.0f || p.get2(1, 1) != 4.0f) { return 11; }\n"
        "        if (!m.isOnGpu()) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = compileOn(src, cajeta::xpu::Backend::Cpu);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// 3.1.2 — a strided (transposed) view is refused by name, never read wrongly, and
// the caller's tensors keep their placement.
//
// RED until 3.2.1: matmul and sum bind the buffer and read the storage order.
TEST(TensorDeviceViewTests, aStridedViewIsRefusedByName) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] md = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] ms = heap int64[2]; ms[0] = 2; ms[1] = 2;\n"
        "        Tensor<float32> m #= Tensor.of<float32>(md, ms);\n"
        "        m.gpu();\n"
        "        Tensor<float32> mt #= m.transpose();\n"
        "        boolean named = false;\n"
        "        try {\n"
        "            Tensor<float32> p #= Ewise.matmulF32Op(mt, m);\n"
        "        } catch (PlacementException ex) {\n"
        "            named = ex.message.contains(\"matmulF32\") && ex.message.contains(\"strided\");\n"
        "        }\n"
        "        if (!named) { return 1; }\n"
        "        named = false;\n"
        "        try {\n"
        "            float32 x = Ewise.sumF32(mt);\n"
        "        } catch (PlacementException ex) {\n"
        "            named = ex.message.contains(\"sumF32\") && ex.message.contains(\"strided\");\n"
        "        }\n"
        "        if (!named) { return 2; }\n"
        "        if (!m.isOnGpu()) { return 3; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = compileOn(src, cajeta::xpu::Backend::Cpu);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// 3.1.3 — a view whose byte offset is not a multiple of the device's storage
// binding alignment is refused with the alignment in the message. Vulkan is the
// backend with such an alignment (4 bytes on RADV, so the view is float16 at
// element offset 1); the refusal happens before submission.
//
// RED until 3.2.1: the runtime refuses the launch on stderr and the op returns a
// value computed on nothing.
TEST(TensorDeviceViewTests, anUnalignedViewIsRefusedWithTheAlignment) {
    CAJETA_SKIP_IF_NO_VULKAN();
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float16[] d = heap float16[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) { d[i] = (float16) ((float32) i + 1.0f); i = i + 1; }\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        Tensor<float16> t #= Tensor.of<float16>(d, s);\n"
        "        t.gpu();\n"
        "        Tensor<float16> v #= t.slice(0, 1, 5);\n"
        "        boolean named = false;\n"
        "        try {\n"
        "            float32 x = Ewise.sumF16(v);\n"
        "        } catch (Exception ex) {\n"
        "            named = ex.message.contains(\"multiple of\");\n"
        "        }\n"
        "        if (!named) { return 1; }\n"
        "        if (!t.isOnGpu()) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = compileOn(src, cajeta::xpu::Backend::Spirv);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// 3.3.1 — the remaining entries: the f16/bf16 sums, scatter and bincount with
// int64 index views, and the f64, int8, f16, bf16 and bf16-wide matmuls on a
// 16x16 view at element offset 256 of a 32x16 matrix.
TEST(TensorDeviceViewTests, offsetViewsReachTheRemainingEntries) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n" + VIEWS +
        "        float16[] h = heap float16[8];\n"
        "        bfloat16[] bf = heap bfloat16[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) { h[i] = (float16) ((float32) i + 1.0f); bf[i] = (bfloat16) ((float32) i + 1.0f); i = i + 1; }\n"
        "        Tensor<float16> th #= Tensor.of<float16>(h, s);\n"
        "        th.gpu();\n"
        "        Tensor<float16> vh #= th.slice(0, 2, 6);\n"
        "        if (Ewise.sumF16(vh) != 18.0f) { return 1; }\n"
        "        Tensor<bfloat16> tb #= Tensor.of<bfloat16>(bf, s);\n"
        "        tb.gpu();\n"
        "        Tensor<bfloat16> vb #= tb.slice(0, 2, 6);\n"
        "        if (Ewise.sumBf16(vb) != 18.0f) { return 2; }\n"
        "        int64[] ix = [ 9L, 9L, 9L, 9L, 3L, 2L, 1L, 0L ];\n"
        "        Tensor<int64> it #= Tensor.of<int64>(ix, s);\n"
        "        it.gpu();\n"
        "        Tensor<int64> iv #= it.slice(0, 4, 8);\n"
        "        Tensor<float32> sc #= Ewise.scatterF32Op(v, iv, 4);\n"
        "        sc.cpu();\n"
        "        if (sc.get1(0) != 6.0f || sc.get1(3) != 3.0f) { return 3; }\n"
        "        int64[] cx = [ 7L, 7L, 1L, 1L, 2L, 2L, 3L, 3L ];\n"
        "        Tensor<int64> ct #= Tensor.of<int64>(cx, s);\n"
        "        ct.gpu();\n"
        "        Tensor<int64> cv #= ct.slice(0, 2, 8);\n"
        "        Tensor<int32> bc #= Ewise.bincountI32Op(cv, 4);\n"
        "        bc.cpu();\n"
        "        if (bc.get1(0) != 0 || bc.get1(1) != 2 || bc.get1(3) != 2) { return 4; }\n"
        "        float64[] mdD = heap float64[512];\n"
        "        int32 rD = 0;\n"
        "        while (rD < 32) { int32 c = 0; while (c < 16) { float32 val = 99.0f; if (rD >= 16) { val = (float32) (rD - 15); } mdD[rD * 16 + c] = (float64) val; c = c + 1; } rD = rD + 1; }\n"
        "        int64[] msD = heap int64[2]; msD[0] = 32; msD[1] = 16;\n"
        "        Tensor<float64> mD #= Tensor.of<float64>(mdD, msD);\n"
        "        mD.gpu();\n"
        "        Tensor<float64> rowsD #= mD.slice(0, 16, 32);\n"
        "        float64[] bdD = heap float64[256];\n"
        "        int32 qD = 0;\n"
        "        while (qD < 256) { float32 e = 0.0f; if (qD / 16 == qD % 16) { e = 1.0f; } bdD[qD] = (float64) e; qD = qD + 1; }\n"
        "        int64[] bsD = heap int64[2]; bsD[0] = 16; bsD[1] = 16;\n"
        "        Tensor<float64> bD #= Tensor.of<float64>(bdD, bsD);\n"
        "        bD.gpu();\n"
        "        Tensor<float64> pD #= Ewise.matmulF64Op(rowsD, bD);\n"
        "        pD.cpu();\n"
        "        if ((float32) pD.get2(0, 0) != 1.0f || (float32) pD.get2(15, 15) != 16.0f || (float32) pD.get2(5, 0) != 6.0f) { return 21; }\n"
        "        int8[] mdI = heap int8[512];\n"
        "        int32 rI = 0;\n"
        "        while (rI < 32) { int32 c = 0; while (c < 16) { float32 val = 99.0f; if (rI >= 16) { val = (float32) (rI - 15); } mdI[rI * 16 + c] = (int8) val; c = c + 1; } rI = rI + 1; }\n"
        "        int64[] msI = heap int64[2]; msI[0] = 32; msI[1] = 16;\n"
        "        Tensor<int8> mI #= Tensor.of<int8>(mdI, msI);\n"
        "        mI.gpu();\n"
        "        Tensor<int8> rowsI #= mI.slice(0, 16, 32);\n"
        "        int8[] bdI = heap int8[256];\n"
        "        int32 qI = 0;\n"
        "        while (qI < 256) { float32 e = 0.0f; if (qI / 16 == qI % 16) { e = 1.0f; } bdI[qI] = (int8) e; qI = qI + 1; }\n"
        "        int64[] bsI = heap int64[2]; bsI[0] = 16; bsI[1] = 16;\n"
        "        Tensor<int8> bI #= Tensor.of<int8>(bdI, bsI);\n"
        "        bI.gpu();\n"
        "        Tensor<int32> pI #= Ewise.matmulI8Op(rowsI, bI);\n"
        "        pI.cpu();\n"
        "        if ((float32) pI.get2(0, 0) != 1.0f || (float32) pI.get2(15, 15) != 16.0f || (float32) pI.get2(5, 0) != 6.0f) { return 22; }\n"
        "        float16[] mdH = heap float16[512];\n"
        "        int32 rH = 0;\n"
        "        while (rH < 32) { int32 c = 0; while (c < 16) { float32 val = 99.0f; if (rH >= 16) { val = (float32) (rH - 15); } mdH[rH * 16 + c] = (float16) val; c = c + 1; } rH = rH + 1; }\n"
        "        int64[] msH = heap int64[2]; msH[0] = 32; msH[1] = 16;\n"
        "        Tensor<float16> mH #= Tensor.of<float16>(mdH, msH);\n"
        "        mH.gpu();\n"
        "        Tensor<float16> rowsH #= mH.slice(0, 16, 32);\n"
        "        float16[] bdH = heap float16[256];\n"
        "        int32 qH = 0;\n"
        "        while (qH < 256) { float32 e = 0.0f; if (qH / 16 == qH % 16) { e = 1.0f; } bdH[qH] = (float16) e; qH = qH + 1; }\n"
        "        int64[] bsH = heap int64[2]; bsH[0] = 16; bsH[1] = 16;\n"
        "        Tensor<float16> bH #= Tensor.of<float16>(bdH, bsH);\n"
        "        bH.gpu();\n"
        "        Tensor<float32> pH #= Ewise.matmulF16Op(rowsH, bH);\n"
        "        pH.cpu();\n"
        "        if ((float32) pH.get2(0, 0) != 1.0f || (float32) pH.get2(15, 15) != 16.0f || (float32) pH.get2(5, 0) != 6.0f) { return 23; }\n"
        "        bfloat16[] mdB = heap bfloat16[512];\n"
        "        int32 rB = 0;\n"
        "        while (rB < 32) { int32 c = 0; while (c < 16) { float32 val = 99.0f; if (rB >= 16) { val = (float32) (rB - 15); } mdB[rB * 16 + c] = (bfloat16) val; c = c + 1; } rB = rB + 1; }\n"
        "        int64[] msB = heap int64[2]; msB[0] = 32; msB[1] = 16;\n"
        "        Tensor<bfloat16> mB #= Tensor.of<bfloat16>(mdB, msB);\n"
        "        mB.gpu();\n"
        "        Tensor<bfloat16> rowsB #= mB.slice(0, 16, 32);\n"
        "        bfloat16[] bdB = heap bfloat16[256];\n"
        "        int32 qB = 0;\n"
        "        while (qB < 256) { float32 e = 0.0f; if (qB / 16 == qB % 16) { e = 1.0f; } bdB[qB] = (bfloat16) e; qB = qB + 1; }\n"
        "        int64[] bsB = heap int64[2]; bsB[0] = 16; bsB[1] = 16;\n"
        "        Tensor<bfloat16> bB #= Tensor.of<bfloat16>(bdB, bsB);\n"
        "        bB.gpu();\n"
        "        Tensor<bfloat16> pB #= Ewise.matmulBf16Op(rowsB, bB);\n"
        "        pB.cpu();\n"
        "        if ((float32) pB.get2(0, 0) != 1.0f || (float32) pB.get2(15, 15) != 16.0f || (float32) pB.get2(5, 0) != 6.0f) { return 24; }\n"
        "        bfloat16[] mdW = heap bfloat16[512];\n"
        "        int32 rW = 0;\n"
        "        while (rW < 32) { int32 c = 0; while (c < 16) { float32 val = 99.0f; if (rW >= 16) { val = (float32) (rW - 15); } mdW[rW * 16 + c] = (bfloat16) val; c = c + 1; } rW = rW + 1; }\n"
        "        int64[] msW = heap int64[2]; msW[0] = 32; msW[1] = 16;\n"
        "        Tensor<bfloat16> mW #= Tensor.of<bfloat16>(mdW, msW);\n"
        "        mW.gpu();\n"
        "        Tensor<bfloat16> rowsW #= mW.slice(0, 16, 32);\n"
        "        bfloat16[] bdW = heap bfloat16[256];\n"
        "        int32 qW = 0;\n"
        "        while (qW < 256) { float32 e = 0.0f; if (qW / 16 == qW % 16) { e = 1.0f; } bdW[qW] = (bfloat16) e; qW = qW + 1; }\n"
        "        int64[] bsW = heap int64[2]; bsW[0] = 16; bsW[1] = 16;\n"
        "        Tensor<bfloat16> bW #= Tensor.of<bfloat16>(bdW, bsW);\n"
        "        bW.gpu();\n"
        "        Tensor<float32> pW #= Ewise.matmulBf16WideOp(rowsW, bW);\n"
        "        pW.cpu();\n"
        "        if ((float32) pW.get2(0, 0) != 1.0f || (float32) pW.get2(15, 15) != 16.0f || (float32) pW.get2(5, 0) != 6.0f) { return 25; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = compileOn(src, cajeta::xpu::Backend::Cpu);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}
