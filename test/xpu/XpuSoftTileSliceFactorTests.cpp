// The software tile's epilogue factors read from a Shared slice: an int8 mma,
// scaledAccumI32 by WaveVector.ofSlice column scales, then scaledAccumInto by a
// slice row factor and ofSlice column factors, exact against a host reference.
// On Vulkan the slice reads lost their per-element index (cajeta-llm 15.2.19.6.3).

#include <gtest/gtest.h>

#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::Backend;

namespace {

const char* SRC =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.WaveVector;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public final class D {\n"
    "    @Kernel @Occupancy(maxThreads = 256)\n"
    "    public static void sliced(KernelBuffer<int32> outI, KernelBuffer<float32> outF,\n"
    "            KernelBuffer<int8> a, KernelBuffer<int8> b, KernelBuffer<int32> sc,\n"
    "            KernelBuffer<float32> dv) {\n"
    "        Shared<int32> scS = shared int32[256];\n"
    "        Shared<float32> dS = shared float32[256];\n"
    "        uint32 tid = KernelThread.x();\n"
    "        uint32 wid = tid / 32;\n"
    "        scS[tid] = sc[tid];\n"
    "        dS[tid] = dv[tid];\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        CooperativeMatrix<int32,16,16,2> iacc;\n"
    "        CooperativeMatrix<float32,16,16,2> facc;\n"
    "        ma.load(a, wid * 256, 0, 16);\n"
    "        mb.load(b, wid * 256, 1, 16);\n"
    "        iacc.splat(0);\n"
    "        facc.splat(0.0f);\n"
    "        mc.splat(0);\n"
    "        mc.mma(ma, mb);\n"
    "        WaveVector<int32, 16> sv = WaveVector.ofSlice(scS, wid * 32, 2);\n"
    "        mc.scaledAccumI32(iacc, sv);\n"
    "        iacc.store(outI, wid * 256, 0, 16);\n"
    "        WaveVector<float32, 16> dw = WaveVector.ofSlice(dS, wid * 32, 1);\n"
    "        iacc.scaledAccumInto(facc, dS[wid * 32 + 16], dw);\n"
    "        facc.store(outF, wid * 256, 0, 16);\n"
    "    }\n"
    "    static int32 av(int32 i) { return (i * 5 + (i / 16) * 3) % 17 - 8; }\n"
    "    static int32 bv(int32 i) { return (i * 7 + (i / 16) * 11) % 15 - 7; }\n"
    "    static int32 mm(int32 w, int32 r, int32 c) {\n"
    "        int32 s = 0; int32 k = 0;\n"
    "        while (k < 16) { s = s + D.av(w * 256 + r * 16 + k) * D.bv(w * 256 + c * 16 + k); k = k + 1; }\n"
    "        return s;\n"
    "    }\n"
    // 1 when every cell of both outputs matches; -(e + 1) at the first wrong
    // int32 cell, -(e + 1) - 10000 at the first wrong float cell; -20000 when
    // the launch raised.
    "    static int32 check(uint32 blk) {\n"
    "        int8[] ha = heap int8[2048]; int8[] hb = heap int8[2048];\n"
    "        int32[] hs = heap int32[256]; float32[] hd = heap float32[256];\n"
    "        int32 i = 0;\n"
    "        while (i < 2048) { ha[i] = (int8) D.av(i); hb[i] = (int8) D.bv(i); i = i + 1; }\n"
    "        i = 0;\n"
    "        while (i < 256) { hs[i] = (i % 9) - 4; hd[i] = 0.25f * (float32) (1 + i % 5); i = i + 1; }\n"
    "        KernelBuffer<int8> a = heap KernelBuffer<int8>(2048); a.upload(ha);\n"
    "        KernelBuffer<int8> b = heap KernelBuffer<int8>(2048); b.upload(hb);\n"
    "        KernelBuffer<int32> sc = heap KernelBuffer<int32>(256); sc.upload(hs);\n"
    "        KernelBuffer<float32> dv = heap KernelBuffer<float32>(256); dv.upload(hd);\n"
    "        KernelBuffer<int32> oi = heap KernelBuffer<int32>(2048);\n"
    "        KernelBuffer<float32> of = heap KernelBuffer<float32>(2048);\n"
    "        int32[] gi = heap int32[2048]; float32[] gf = heap float32[2048];\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        try {\n"
    "            sliced.launch(s, grid: [1], block: [blk])(oi, of, a, b, sc, dv);\n"
    "            s.sync();\n"
    "        } catch (XpuLaunchException e) { return -20000; }\n"
    "        oi.download(gi); of.download(gf);\n"
    "        int32 waves = (int32) blk / 32;\n"
    "        int32 e = 0;\n"
    "        while (e < waves * 256) {\n"
    "            int32 w = e / 256; int32 r = (e % 256) / 16; int32 c = e % 16;\n"
    "            int32 iv = D.mm(w, r, c) * hs[w * 32 + 2 * c];\n"
    "            if (gi[e] != iv) { return -(e + 1); }\n"
    "            float32 want = (hd[w * 32 + 16 + r] * hd[w * 32 + c]) * (float32) iv;\n"
    "            if (gf[e] != want) { return -(e + 1) - 10000; }\n"
    "            e = e + 1;\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int32 r = D.check(32);\n"
    "        if (r != 1) { return r; }\n"
    "        return D.check(256);\n"
    "    }\n"
    "}\n";

void expectExact(Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(SRC, "test.D", o);
    int32_t r = -9999;
    if (jit) {
        auto f = jit->lookup<int32_t (*)()>("run");
        if (f) r = f();
    }
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 1) << "-(e+1): first wrong int32 cell e; less 10000: first wrong "
                       "float cell; -20000: the launch raised:\n" << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped] sliced"), std::string::npos) << err;
}

} // namespace

TEST(XpuSoftTileSliceFactor, matchesTheHostOnCpu) {
    expectExact(Backend::Cpu);
}

TEST(XpuSoftTileSliceFactor, matchesTheHostOnVulkan) {
    CAJETA_SKIP_IF_NO_VULKAN();
    expectExact(Backend::Spirv);
}
