//
// AmdgpuCoopBf16Tests — the AMD half of the coop-matrix dtype/tier matrix
// (companion to NvptxCoopBf16Tests and XpuSpirvMixedTierTests, written after
// the NVPTX bf16 fragment abort: specs/nvptx-coop-bf16-fragment-abort).
//
// AMD's tier table is role-aware like NVPTX's — f16/bf16/int8 are native as
// A/B OPERAND tiles, f32/i32 as accumulators (`AmdgpuKernelLowering
// .coopMatrixTier`) — and its lowering has its own fragment-shape code, so the
// NVPTX lesson gets pinned here too: every combination must either lower or
// skip gracefully; none may abort the compiling process.
//
// Compile-time only (Backend::Amdgpu, never launches): AMDGCN ISA emission
// runs without a device or ROCm, which is exactly where the NVPTX sibling
// defect lived. Real-silicon execution rides the device-tests workflow.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Amdgpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n";

} // namespace

// bf16 A/B + f32 accumulator: all-Native on AMD (wmma.f32.16x16x16.bf16 is
// real RDNA3 silicon) — the shape that aborted NVPTX. Must lower, not abort.
TEST(AmdgpuCoopBf16Tests, bf16OperandsF32AccLowersWithoutAborting) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void gemmBf16(KernelBuffer<float32> c, KernelBuffer<bfloat16> a,\n"
        "                                KernelBuffer<bfloat16> b, uint32 depth) {\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
        "        CooperativeMatrix<bfloat16,16,16,1> mb;\n"
        "        ma.load(a, 0, 0, depth);\n"
        "        mb.load(b, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    testing::internal::CaptureStderr();
    int32_t rc = runI32Amdgpu(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "bf16(A/B)+f32(acc) is all-native on AMD and must lower, not skip; "
        << "stderr was:\n" << err;
}

// The all-bf16 GEMM (bf16 accumulator — `Ewise.matmulBf16`'s shape) is
// mixed-tier on AMD and must take the graceful skip note, same as on NVPTX.
TEST(AmdgpuCoopBf16Tests, allBf16MixedTierSkipsGracefully) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void gemmAllBf16(KernelBuffer<bfloat16> c, KernelBuffer<bfloat16> a,\n"
        "                                   KernelBuffer<bfloat16> b, uint32 depth) {\n"
        "        CooperativeMatrix<bfloat16,16,16,2> mc;\n"
        "        mc.splat((bfloat16) 0.0);\n"
        "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
        "        CooperativeMatrix<bfloat16,16,16,1> mb;\n"
        "        ma.load(a, 0, 0, depth);\n"
        "        mb.load(b, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    testing::internal::CaptureStderr();
    int32_t rc = runI32Amdgpu(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "expected the kernel-skipped note for the mixed-tier kernel; stderr was:\n"
        << err;
}

// Control: the f16(A/B)+f32(acc) native shape — `Ewise.matmulF16`'s — still
// lowers (the XpuTorchConfigProbeTests recipe, pinned at this seam).
TEST(AmdgpuCoopBf16Tests, f16OperandsF32AccStillLowers) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void gemmF16(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
        "                               KernelBuffer<float16> b, uint32 depth) {\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        CooperativeMatrix<float16,16,16,0> ma;\n"
        "        CooperativeMatrix<float16,16,16,1> mb;\n"
        "        ma.load(a, 0, 0, depth);\n"
        "        mb.load(b, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    testing::internal::CaptureStderr();
    int32_t rc = runI32Amdgpu(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "f16(A/B)+f32(acc) is the native config and must lower; stderr was:\n"
        << err;
}
