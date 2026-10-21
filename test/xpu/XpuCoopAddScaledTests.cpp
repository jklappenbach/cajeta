// vulkan-native-coop-epilogue Unit 2 and 3: `acc.addScaledPow2(src, e)` is
// acc + src * 2^e, bit for bit the same on every tier, and on Vulkan it keeps a
// kernel's tiles native instead of demoting them to the software tile.

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#ifdef __linux__
#include <unistd.h>
#endif

using cajeta_test::CajetaJit;
using cajeta::xpu::Backend;

namespace {

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelManifest;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.XpuLaunchException;\n";

const char* KERNEL =
    "    @Kernel @FastMath\n"
    "    public static void as(KernelBuffer<float32> out, KernelBuffer<float32> y,\n"
    "            KernelBuffer<float32> src, uint32 which) {\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        CooperativeMatrix<float32,16,16,2> m;\n"
    "        acc.load(y, 0, 0, 16);\n"
    "        m.load(src, 0, 0, 16);\n"
    "        if (which == 0) { acc.addScaledPow2(m, -10); } else { acc.addScaledPow2(m, 3); }\n"
    "        acc.store(out, 0, 0, 16);\n"
    "    }\n";

// 1 when both launches (e = -10 and e = 3) match acc + src * 2^e bit for bit;
// -(i + 1) at the first mismatch (less 2000 for the second), -1000 if it never ran.
const char* RUN =
    "    static int32 check(uint32 which, float32 s) {\n"
    "        float32[] hy = heap float32[256];\n"
    "        float32[] hs = heap float32[256];\n"
    "        float32[] want = heap float32[256];\n"
    "        int32 i = 0;\n"
    "        while (i < 256) {\n"
    "            hy[i] = 0.75f * (float32) (i % 13) - 4.0f;\n"
    "            hs[i] = 0.3125f * (float32) (i % 7) + 0.1f;\n"
    "            i = i + 1;\n"
    "        }\n"
    "        hy[0] = -1.00048828125f;\n"
    "        hs[0] = 1.000244140625f;\n"
    "        i = 0;\n"
    "        while (i < 256) {\n"
    "            float32 m = s * hs[i];\n"
    "            want[i] = hy[i] + m;\n"
    "            i = i + 1;\n"
    "        }\n"
    "        KernelBuffer<float32> o = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> sb = heap KernelBuffer<float32>(256);\n"
    "        y.upload(hy); sb.upload(hs);\n"
    "        KernelStream st #= KernelStream.current();\n"
    "        try {\n"
    "            as.launch(st, grid: [1], block: [32])(o, y, sb, which);\n"
    "            st.sync();\n"
    "        } catch (XpuLaunchException e) { return -1000; }\n"
    "        o.download(hy);\n"
    "        i = 0;\n"
    "        while (i < 256) {\n"
    "            if (hy[i] != want[i]) { return -(i + 1); }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int32 r = D.check(0, 0.0009765625f);\n"
    "        if (r != 1) { return r; }\n"
    "        r = D.check(1, 8.0f);\n"
    "        if (r != 1) { return r - 2000; }\n"
    "        return 1;\n"
    "    }\n"
    "    public static int32 tierOfMma() {\n"
    "        KernelManifest m = mmaAs.manifest();\n"
    "        if (m == null) { return -1; }\n"
    "        int32 i = 0;\n"
    "        while (i < m.nativeOpCount()) {\n"
    "            if (m.nativeOp(i).equals(\"mma\")) {\n"
    "                return m.nativeOpTier(i).equals(\"native\") ? 1 : 0;\n"
    "            }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return -2;\n"
    "    }\n";

// An f16 mma into an f32 accumulator, then addScaled: on Vulkan the mma must stay native.
const char* MMA_KERNEL =
    "    @Kernel\n"
    "    public static void mmaAs(KernelBuffer<float32> y, KernelBuffer<float16> a,\n"
    "            KernelBuffer<float16> b) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        CooperativeMatrix<float32,16,16,2> lo;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        acc.splat(0.0f);\n"
    "        lo.splat(0.0f);\n"
    "        lo.mma(ma, mb);\n"
    "        acc.addScaledPow2(lo, -10);\n"
    "        acc.store(y, 0, 0, 16);\n"
    "    }\n";

std::string source(const std::string& kernel) {
    return std::string(PRE) + "public final class D {\n" + kernel + MMA_KERNEL + RUN + "}\n";
}

int32_t runOn(Backend b, const std::string& src, const char* fn, std::string* err) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    int32_t r = -9999;
    if (jit) {
        auto f = jit->lookup<int32_t (*)()>(fn);
        if (f) r = f();
    }
    *err = testing::internal::GetCapturedStderr();
    return r;
}

void expectExact(Backend b) {
    std::string err;
    int32_t r = runOn(b, source(KERNEL), "run", &err);
    EXPECT_EQ(r, 1) << "-(i+1) mismatch at element i; -2000 marks the second exponent; "
                       "-1000 the launch raised:\n" << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped] as"), std::string::npos) << err;
}

// The kernel with one wrong call substituted for the first addScaledPow2 call.
std::string withCall(const std::string& call) {
    std::string k = KERNEL;
    const std::string from = "acc.addScaledPow2(m, -10);";
    k.replace(k.find(from), from.size(), call);
    return k;
}

} // namespace

TEST(XpuCoopAddScaled, matchesBitForBitOnCpu) {
    expectExact(Backend::Cpu);
}

TEST(XpuCoopAddScaled, matchesBitForBitOnAmdgpu) {
    CAJETA_SKIP_IF_NO_HIP();
    expectExact(Backend::Amdgpu);
}

TEST(XpuCoopAddScaled, matchesBitForBitOnVulkan) {
    CAJETA_SKIP_IF_NO_VULKAN();
    expectExact(Backend::Spirv);
}

TEST(XpuCoopAddScaled, keepsAVulkanKernelsMmaNative) {
    CAJETA_SKIP_IF_NO_VULKAN();
    std::string err;
    EXPECT_EQ(runOn(Backend::Spirv, source(KERNEL), "tierOfMma", &err), 1)
        << "1 = native, 0 = demoted, -1 = no manifest, -2 = no mma entry:\n" << err;
}

TEST(XpuCoopAddScaled, anEpilogueVerbStillDemotesOnVulkan) {
    CAJETA_SKIP_IF_NO_VULKAN();
    std::string k = MMA_KERNEL;
    const std::string from = "        acc.addScaledPow2(lo, -10);\n";
    k.replace(k.find(from), from.size(),
              "        lo.rank1Accum(rf, rf);\n        acc.addScaledPow2(lo, -10);\n");
    k.replace(k.find("        CooperativeMatrix<float16,16,16,0> ma;\n"), 0,
              "        Shared<float32> rf = shared float32[16];\n");
    std::string src = std::string(PRE) + "import cajeta.xpu.Shared;\n" +
                      "public final class D {\n" + KERNEL + k + RUN + "}\n";
    std::string err;
    EXPECT_EQ(runOn(Backend::Spirv, src, "tierOfMma", &err), 0) << err;
}

TEST(XpuCoopAddScaled, aWrongShapeIsNamed) {
    std::string k = withCall("acc.addScaledPow2(n, -10);");
    k.replace(k.find("        acc.load(y, 0, 0, 16);\n"), 0,
              "        CooperativeMatrix<float32,16,8,2> n;\n        n.splat(1.0f);\n");
    std::string err;
    runOn(Backend::Cpu, source(k), "run", &err);
    EXPECT_NE(err.find("addScaledPow2"), std::string::npos) << err;
    EXPECT_NE(err.find("Rows/Cols"), std::string::npos) << err;
}

TEST(XpuCoopAddScaled, aNonAccumulatorSourceIsNamed) {
    std::string k = withCall("acc.addScaledPow2(h, -10);");
    k.replace(k.find("        acc.load(y, 0, 0, 16);\n"), 0,
              "        CooperativeMatrix<float32,16,16,0> h;\n        h.splat(1.0f);\n");
    std::string err;
    runOn(Backend::Cpu, source(k), "run", &err);
    EXPECT_NE(err.find("addScaledPow2"), std::string::npos) << err;
    EXPECT_NE(err.find("Use=2"), std::string::npos) << err;
}

TEST(XpuCoopAddScaled, anIntegerTileIsNamed) {
    std::string k = withCall("ia.addScaledPow2(ib, -10);");
    k.replace(k.find("        acc.load(y, 0, 0, 16);\n"), 0,
              "        CooperativeMatrix<int32,16,16,2> ia;\n        CooperativeMatrix<int32,16,16,2> ib;\n"
              "        ia.splat(1);\n        ib.splat(2);\n");
    std::string err;
    runOn(Backend::Cpu, source(k), "run", &err);
    EXPECT_NE(err.find("addScaledPow2"), std::string::npos) << err;
    EXPECT_NE(err.find("float32"), std::string::npos) << err;
}

TEST(XpuCoopAddScaled, aNonConstantExponentIsNamed) {
    std::string err;
    runOn(Backend::Cpu, source(withCall("acc.addScaledPow2(m, (int32) which);")), "run", &err);
    EXPECT_NE(err.find("addScaledPow2"), std::string::npos) << err;
    EXPECT_NE(err.find("constant"), std::string::npos) << err;
}

TEST(XpuCoopAddScaled, anOutOfRangeExponentIsNamed) {
    std::string err;
    runOn(Backend::Cpu, source(withCall("acc.addScaledPow2(m, -127);")), "run", &err);
    EXPECT_NE(err.find("addScaledPow2"), std::string::npos) << err;
    EXPECT_NE(err.find("[-126, 127]"), std::string::npos) << err;
}

// spec 4.1: under the validation layer, the Vulkan run of a cooperative-matrix kernel reports
// nothing. The layer must be on when the device is created, so this reruns that test in a child.
TEST(XpuCoopAddScaled, theValidationLayerReportsNothingOnVulkan) {
#ifdef __linux__
    CAJETA_SKIP_IF_NO_VULKAN();
    struct stat st;
    if (stat("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json", &st) != 0)
        GTEST_SKIP() << "the Khronos validation layer is not installed";
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    ASSERT_GT(n, 0);
    exe[n] = 0;
    std::string cmd = std::string("VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation '") + exe +
                      "' --gtest_filter=XpuCoopAddScaled.matchesBitForBitOnVulkan 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    ASSERT_NE(f, nullptr);
    std::string out;
    char buf[4096];
    while (size_t k = fread(buf, 1, sizeof buf, f)) out.append(buf, k);
    int rc = pclose(f);
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("[       OK ] XpuCoopAddScaled.matchesBitForBitOnVulkan"), std::string::npos) << out;
    EXPECT_EQ(out.find("Validation Error"), std::string::npos) << out;
#else
    GTEST_SKIP() << "Linux only";
#endif
}

// NVIDIA has no device on every box, so its lowering is checked here without one: the kernel
// lowers to PTX on native fragments (no local-memory frame), as on the other native tiers.
TEST(XpuCoopAddScaled, lowersOnNativeNvptxFragments) {
    using namespace cajeta::xpu::probe;
    const Lowered l = lowerForNvptx(source(KERNEL).c_str(), "as", "test.D", "sm_89", "addscaled");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::None) << "depot=" << r.depotBytes << " local=" << r.localOps;
}

