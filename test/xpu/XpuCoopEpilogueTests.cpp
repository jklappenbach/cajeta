//
// XpuCoopEpilogueTests — xpu-coopmatrix-epilogue Unit 1 (1.1.1/1.1.2):
// the two fused epilogue verbs, `scaledAccumInto` and `rank1Accum`, on
// the in-process CPU backend's software tile. EXACT agreement against a
// scalar host reference (integer mma + one fma chain per element has no
// reassociation slack to hide behind), plus the named diagnostics for
// malformed calls — a wrong call must say what is wrong, never skip
// silently (the vacuous-green discipline).
//
// The amdgpu-native lowering is Unit 2 (`AmdgpuCoopEpilogueTests` at
// compile level + the gpu-parity runtime case); the semantics pinned
// here are the contract every tier shares:
//   scaledAccumInto: facc[r][c] += rowF[r] * colF[c] * mc[r][c]
//   rank1Accum:      facc[r][c] += rowF[r] * colF[c]
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Cpu(const std::string& src, std::string* errOut = nullptr) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    std::string err = testing::internal::GetCapturedStderr();
    if (errOut) *errOut = err;
    return r;
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n";

// One 16x16 int8 GEMM tile, then both verbs, then store. The Shared
// vectors are staged by the wave from KernelBuffers — the same idiom the
// consumer kernels use for their per-row/per-token scale state.
const char* KERNEL =
    "    @Kernel\n"
    "    public static void epi(KernelBuffer<float32> y,\n"
    "            KernelBuffer<int8> a, KernelBuffer<int8> b,\n"
    "            KernelBuffer<float32> rf, KernelBuffer<float32> cf,\n"
    "            KernelBuffer<float32> rg, KernelBuffer<float32> cg) {\n"
    "        Shared<float32> rowF = shared float32[16];\n"
    "        Shared<float32> colF = shared float32[16];\n"
    "        Shared<float32> rowG = shared float32[16];\n"
    "        Shared<float32> colG = shared float32[16];\n"
    "        uint32 lane = KernelThread.x();\n"
    "        uint32 i = lane;\n"
    "        while (i < 16) {\n"
    "            rowF[i] = rf[i]; colF[i] = cf[i];\n"
    "            rowG[i] = rg[i]; colG[i] = cg[i];\n"
    "            i = i + 32;\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        CooperativeMatrix<float32,16,16,2> facc;\n"
    "        facc.splat(0.0f);\n"
    "        mc.splat(0);\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.scaledAccumInto(facc, rowF, colF);\n"
    "        facc.rank1Accum(rowG, colG);\n"
    "        mc.scaledAccumInto2(facc, rowF, colF, rowG, colG);\n"
    "        facc.store(y, 0, 0, 16);\n"
    "    }\n";

// Host driver: distinct signed operands, non-uniform scale vectors, a
// scalar reference computed in the SAME per-element order, EXACT compare,
// and a liveness guard (an all-zero agreement proves the kernel never
// ran, not that it is right).
const char* RUN =
    "    public static int32 run() {\n"
    "        int8[] ha = heap int8[256];\n"
    "        int8[] hb = heap int8[256];\n"
    "        float32[] hy = heap float32[256];\n"
    "        float32[] hrf = heap float32[16];\n"
    "        float32[] hcf = heap float32[16];\n"
    "        float32[] hrg = heap float32[16];\n"
    "        float32[] hcg = heap float32[16];\n"
    "        int32 i = 0;\n"
    "        while (i < 256) {\n"
    "            ha[i] = (int8) ((i * 5) % 17 - 8);\n"
    "            hb[i] = (int8) ((i * 7) % 15 - 7);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        i = 0;\n"
    "        while (i < 16) {\n"
    "            hrf[i] = 0.25f * (float32) (i + 1);\n"
    "            hcf[i] = 0.5f - 0.0625f * (float32) i;\n"
    "            hrg[i] = 1.5f - 0.125f * (float32) i;\n"
    "            hcg[i] = 0.375f * (float32) (i - 7);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);\n"
    "        KernelBuffer<int8> b = heap KernelBuffer<int8>(256);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> rf = heap KernelBuffer<float32>(16);\n"
    "        KernelBuffer<float32> cf = heap KernelBuffer<float32>(16);\n"
    "        KernelBuffer<float32> rg = heap KernelBuffer<float32>(16);\n"
    "        KernelBuffer<float32> cg = heap KernelBuffer<float32>(16);\n"
    "        a.upload(ha); b.upload(hb);\n"
    "        rf.upload(hrf); cf.upload(hcf);\n"
    "        rg.upload(hrg); cg.upload(hcg);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        epi.launch(s, grid: [1], block: [32])(y, a, b, rf, cf, rg, cg);\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        boolean live = false;\n"
    "        int32 r = 0;\n"
    "        while (r < 16) {\n"
    "            int32 c = 0;\n"
    "            while (c < 16) {\n"
    "                int32 acc = 0;\n"
    "                int32 k = 0;\n"
    "                while (k < 16) {\n"
    "                    acc = acc + (int32) ha[r * 16 + k]\n"
    "                              * (int32) hb[k * 16 + c];\n"
    "                    k = k + 1;\n"
    "                }\n"
    "                float32 t1 = hrf[r] * hcf[c] * (float32) acc\n"
    "                            + hrg[r] * hcg[c];\n"
    "                float32 t2 = hrf[r] * hcf[c] * (float32) acc\n"
    "                            + hrg[r] * hcg[c];\n"
    "                float32 want = t1 + t2;\n"
    "                float32 got = hy[r * 16 + c];\n"
    "                if (got != want) { return -(r * 16 + c + 1); }\n"
    "                if (got != 0.0f) { live = true; }\n"
    "                c = c + 1;\n"
    "            }\n"
    "            r = r + 1;\n"
    "        }\n"
    "        if (!live) { return -1000; }\n"
    "        return 1;\n"
    "    }\n";

} // namespace

// 1.1.1 — both verbs on the software tile agree with the scalar
// reference EXACTLY, and the output is live.
TEST(XpuCoopEpilogueTests, verbsMatchScalarReferenceExactlyOnCpu) {
    std::string src = std::string(PRE) +
        "public final class D {\n" + KERNEL + RUN + "}\n";
    std::string err;
    int32_t r = runI32Cpu(src, &err);
    EXPECT_EQ(r, 1) << "element " << (-r - 1)
        << " disagrees (or -1000 = all-zero output; kernel never ran):\n"
        << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "the epilogue kernel must LOWER on the cpu software tile:\n"
        << err;
}

// 1.1.2 — a malformed call names its defect instead of skipping
// silently: wrong arity on scaledAccumInto.
TEST(XpuCoopEpilogueTests, wrongArityIsANamedDiagnostic) {
    std::string bad = std::string(KERNEL);
    std::string from = "mc.scaledAccumInto(facc, rowF, colF);";
    std::string to = "mc.scaledAccumInto(facc, rowF);";
    bad.replace(bad.find(from), from.size(), to);
    std::string src = std::string(PRE) +
        "public final class D {\n" + bad +
        "    public static int32 run() { return 1; }\n}\n";
    std::string err;
    EXPECT_EQ(runI32Cpu(src, &err), 1);
    EXPECT_NE(err.find("scaledAccumInto"), std::string::npos)
        << "the diagnostic must NAME the verb:\n" << err;
    EXPECT_NE(err.find("expects"), std::string::npos)
        << "the diagnostic must state the expected shape:\n" << err;
}
