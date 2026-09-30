//
// xpu-kernel-adaptor 4A.4.5: the col-major operand change took a signature
// change on AMD only (the lowering reorients at load), and this runs it on the
// device. The same logical B is stored row-major and col-major, multiplied by
// the same A on the native WMMA tier and on the portable tier, and every
// product must equal the host reference. Small exact integers in f16, so the
// comparison is exact.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kProgram =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "public final class D {\n"
    "    @Kernel\n"
    "    public static void mmRow(KernelBuffer<float16> a,\n"
    "            KernelBuffer<float16> b, KernelBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void mmCol(KernelBuffer<float16> a,\n"
    "            KernelBuffer<float16> b, KernelBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 1, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    static int32 wrong(float32[] got, float32[] want) {\n"
    "        int32 bad = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < 256) { if (got[i] != want[i]) { bad = bad + 1; } i = i + 1; }\n"
    "        return bad;\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float16[] ha = heap float16[256];\n"
    "        float16[] hbr = heap float16[256];\n"
    "        float16[] hbc = heap float16[256];\n"
    "        float32[] ref = heap float32[256];\n"
    "        int32 i = 0;\n"
    "        while (i < 16) {\n"
    "            int32 k = 0;\n"
    "            while (k < 16) {\n"
    "                ha[i * 16 + k] = (float16) (float32) ((i + 2 * k) % 5);\n"
    "                hbr[i * 16 + k] = (float16) (float32) ((3 * i + k) % 4);\n"
    "                hbc[k * 16 + i] = (float16) (float32) ((3 * i + k) % 4);\n"
    "                k = k + 1;\n"
    "            }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        i = 0;\n"
    "        while (i < 16) {\n"
    "            int32 j = 0;\n"
    "            while (j < 16) {\n"
    "                float32 acc = 0.0f;\n"
    "                int32 k = 0;\n"
    "                while (k < 16) {\n"
    "                    acc = acc + (float32) ((i + 2 * k) % 5) * (float32) ((3 * k + j) % 4);\n"
    "                    k = k + 1;\n"
    "                }\n"
    "                ref[i * 16 + j] = acc;\n"
    "                j = j + 1;\n"
    "            }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        KernelBuffer<float16> a = heap KernelBuffer<float16>(256);\n"
    "        KernelBuffer<float16> br = heap KernelBuffer<float16>(256);\n"
    "        KernelBuffer<float16> bc = heap KernelBuffer<float16>(256);\n"
    "        KernelBuffer<float32> c = heap KernelBuffer<float32>(256);\n"
    "        a.upload(ha);\n"
    "        br.upload(hbr);\n"
    "        bc.upload(hbc);\n"
    "        float32[] out = heap float32[256];\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 mask = 0;\n"
    "        i = 0;\n"
    "        while (i < 256) { out[i] = -1.0f; i = i + 1; }\n"
    "        c.upload(out);\n"
    "        mmRow.launch(s, grid: [1], block: [32])(a, br, c);\n"
    "        s.sync();\n"
    "        c.download(out);\n"
    "        if (D.wrong(out, ref) > 0) { mask = mask + 1; }\n"
    "        i = 0;\n"
    "        while (i < 256) { out[i] = -1.0f; i = i + 1; }\n"
    "        c.upload(out);\n"
    "        mmCol.launch(s, grid: [1], block: [32])(a, bc, c);\n"
    "        s.sync();\n"
    "        c.download(out);\n"
    "        if (D.wrong(out, ref) > 0) { mask = mask + 2; }\n"
    "        return mask;\n"
    "    }\n"
    "}\n";

struct Outcome { bool ran; int32_t mask; std::string err; };

Outcome runOnAmdgpu() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kProgram, "test.D", o);
    Outcome r{false, -1, ""};
    if (jit) {
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (fn) { r.mask = fn(); r.ran = true; }
    }
    r.err = testing::internal::GetCapturedStderr();
    return r;
}

} // namespace

// Bit 1 is the row-major control, bit 2 the col-major product.
TEST(AmdgpuCoopColMajorTests, colMajorProductMatchesRowMajorAndHostOnDevice) {
    CAJETA_SKIP_IF_NO_HIP();
    Outcome o = runOnAmdgpu();
    ASSERT_TRUE(o.ran) << o.err;
    EXPECT_EQ(o.err.find("[xpu-kernel-skipped]"), std::string::npos) << o.err;
    EXPECT_EQ(o.err.find("[mma-tiering]"), std::string::npos)
        << "f16 x f16 -> f32 must take the native WMMA tier:\n" << o.err;
    EXPECT_EQ(o.mask, 0) << o.err;
}

// The same kernels forced onto the portable tier: the host reference is exact,
// so equality with it on both tiers is element-for-element agreement between them.
TEST(AmdgpuCoopColMajorTests, colMajorPortableTierAgreesOnDevice) {
    CAJETA_SKIP_IF_NO_HIP();
    setenv("CAJETA_GPU_COOPMATRIX_IMPL", "software", 1);
    Outcome o = runOnAmdgpu();
    unsetenv("CAJETA_GPU_COOPMATRIX_IMPL");
    ASSERT_TRUE(o.ran) << o.err;
    EXPECT_NE(o.err.find("[mma-tiering]"), std::string::npos)
        << "the override did not reach the portable tier:\n" << o.err;
    EXPECT_EQ(o.mask, 0) << o.err;
}
