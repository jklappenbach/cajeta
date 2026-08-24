//
// AmdgpuCoopEpilogueTests — xpu-coopmatrix-epilogue Unit 2 (2.1.1/2.1.2):
// the fused epilogue verbs' NATIVE amdgpu lowering and the loud-demote
// gate on backends without native epilogue support.
//
// Compile-level only (the AmdgpuCoopI8Tests discipline): AMDGCN ISA
// emission needs no device; the SPIR-V half needs no Vulkan device. The
// on-silicon exactness ride is the gpu-parity case (2.3.1) plus the
// cajeta-llama consumers (Unit 3).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32On(cajeta::xpu::Backend be, const std::string& src,
                 std::string* errOut) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    *errOut = testing::internal::GetCapturedStderr();
    return r;
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n";

// An iu8 GEMM whose epilogue runs on the verbs — the consumer shape.
std::string epiKernel(bool withVerbs) {
    std::string body = std::string() +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void epi(KernelBuffer<float32> y,\n"
        "            KernelBuffer<int8> a, KernelBuffer<int8> b,\n"
        "            KernelBuffer<float32> rf, KernelBuffer<float32> cf) {\n"
        "        Shared<float32> rowF = shared float32[16];\n"
        "        Shared<float32> colF = shared float32[16];\n"
        "        uint32 lane = KernelThread.x();\n"
        "        if (lane < 16) { rowF[lane] = rf[lane]; colF[lane] = cf[lane]; }\n"
        "        Barrier.workgroup();\n"
        "        CooperativeMatrix<int8,16,16,0> ma;\n"
        "        CooperativeMatrix<int8,16,16,1> mb;\n"
        "        CooperativeMatrix<int32,16,16,2> mc;\n"
        "        CooperativeMatrix<float32,16,16,2> facc;\n"
        "        facc.splat(0.0f);\n"
        "        mc.splat(0);\n"
        "        ma.load(a, 0, 0, 16);\n"
        "        mb.load(b, 0, 0, 16);\n"
        "        mc.mma(ma, mb);\n";
    if (withVerbs)
        body +=
        "        mc.scaledAccumInto(facc, rowF, colF);\n"
        "        facc.rank1Accum(rowF, colF);\n";
    body +=
        "        facc.store(y, 0, 0, 16);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    return std::string(PRE) + body;
}

} // namespace

// 2.1.1 — on amdgpu the verb kernel LOWERS NATIVELY: no skip note and
// no demote-to-portable note (int8 16x16 is native RDNA3 WMMA and the
// backend implements the epilogue seams, so nothing may fall back).
TEST(AmdgpuCoopEpilogueTests, verbsLowerNativelyOnAmdgpu) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Amdgpu, epiKernel(true), &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "the epilogue kernel must lower, not skip:\n" << err;
    EXPECT_EQ(err.find("[mma-tiering]"), std::string::npos)
        << "int8+epilogue is native on amdgpu - a tier note means it "
           "demoted:\n" << err;
}

// 2.1.2 (fires) — a backend whose native tier has NO epilogue lowering
// (SPIR-V in v1) demotes the kernel's tiles to the portable tile and
// SAYS SO, naming the epilogue op; the kernel still lowers.
TEST(AmdgpuCoopEpilogueTests, spirvDemotesLoudlyWithTheVerbs) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Spirv, epiKernel(true), &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "demote means LOWER on the portable tile, never skip:\n" << err;
    EXPECT_NE(err.find("[mma-epilogue]"), std::string::npos)
        << "the demote must be announced by the epilogue note:\n" << err;
    EXPECT_NE(err.find("scaledAccumInto"), std::string::npos)
        << "the note must NAME the op that forced the demotion:\n" << err;
}

// 2.1.2 (does not fire) — the same kernel minus the verbs stays on the
// SPIR-V native tier: no epilogue note, no tier note, no skip. The
// demote gate must not tax kernels that never use the verbs.
TEST(AmdgpuCoopEpilogueTests, spirvStaysNativeWithoutTheVerbs) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Spirv, epiKernel(false), &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    EXPECT_EQ(err.find("[mma-epilogue]"), std::string::npos)
        << "no verbs, no epilogue note - the gate over-fired:\n" << err;
    EXPECT_EQ(err.find("[mma-tiering]"), std::string::npos)
        << "int8 is native on SPIR-V here - a tier note means the gate "
           "demoted a verb-free kernel:\n" << err;
}
