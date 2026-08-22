//
// AmdgpuCoopI8Tests — the AMD compile-level pin for cajeta-llama 18.2.3:
// the all-int8-operand / int32-accumulator cooperative-matrix GEMM is
// NATIVE silicon on RDNA3 (`v_wmma_i32_16x16x16_iu8`) — it must LOWER,
// with no skip note and no abort (the NvptxCoopBf16Tests discipline:
// every dtype/tier combination either lowers or skips gracefully).
// Compile-time only: AMDGCN ISA emission needs no device or ROCm.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Amdgpu(const std::string& src, std::string* errOut) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    *errOut = testing::internal::GetCapturedStderr();
    return r;
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n";

} // namespace

TEST(AmdgpuCoopI8Tests, i8OperandsI32AccumulatorLowersNatively) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void gemmI8(KernelBuffer<int32> c, KernelBuffer<int8> a,\n"
        "                              KernelBuffer<int8> b, uint32 depth) {\n"
        "        CooperativeMatrix<int32,16,16,2> mc;\n"
        "        mc.splat(0);\n"
        "        CooperativeMatrix<int8,16,16,0> ma;\n"
        "        CooperativeMatrix<int8,16,16,1> mb;\n"
        "        ma.load(a, 0, 0, depth);\n"
        "        mb.load(b, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    std::string err;
    EXPECT_EQ(runI32Amdgpu(src, &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "int8/i32 is native RDNA3 WMMA - it must lower, not skip:\n" << err;
}
