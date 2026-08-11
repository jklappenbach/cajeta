//
// NvptxCoopBf16Tests — the NVPTX native bf16 cooperative-matrix path.
//
// Found 2026-08-10 (via `Ewise.matmulBf16`'s first shape): the NVPTX lowering
// assumed every A/B fragment has the f16 shape `{<2 x half> x 8}`, but LLVM's
// bf16 WMMA fragments pack two values per .b32 register and are `{i32 x 4}`
// (IntrinsicsNVVM.td "m16n16k16:a:bf16"). Three sites broke on that:
//
//   - coopMatrixMulAdd cast fragment element 0 to FixedVectorType — for bf16
//     that element is a scalar i32, and the failed LLVM cast is an assert +
//     SIGABRT that killed the whole JIT-compiling process. Kernel lowering
//     runs for every registered backend at compile time, ptxas present or
//     not, so this aborted programs that never launched the kernel.
//   - nvFragScalar returned the raw i32, re-selecting the f16 load intrinsic
//     for a bf16 tile.
//   - coopMatrixSplat inserted an unpacked bfloat into the i32 register slot.
//
// These tests JIT-compile with Backend::Nvptx only and never launch — the
// defect (and fix) is at kernel-lowering time, so no NVIDIA device or ptxas
// is needed for the abort-vs-survive discriminator. On a machine without
// ptxas the kernel is skipped after LOWERING with the ptxas note; the test
// asserts the process survives and the host entry point runs.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Nvptx(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n";

} // namespace

// bf16 A/B operands with the f32 accumulator — the standard WMMA recipe and
// NVPTX's only native bf16 config (wmma.mma.row.row.bf16 accumulates f32).
// All three tiles are Native tier here, so lowering enters the fragment code
// that used to abort. Pre-fix: LLVM cast<FixedVectorType> assert + SIGABRT
// during CajetaJit::compile. Post-fix: lowers (and on a ptxas-less machine is
// then skipped at the ptxas step — gracefully).
TEST(NvptxCoopBf16Tests, bf16OperandsF32AccLowersWithoutAborting) {
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
    EXPECT_EQ(runI32Nvptx(src), 1);
}

// Splat on a bf16 OPERAND tile exercises the .b32 register-image packing
// (two bf16 per i32); the accumulator splat covers the plain f32 path.
TEST(NvptxCoopBf16Tests, bf16OperandSplatPacksRegisterImage) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void splatBf16(KernelBuffer<float32> c, KernelBuffer<bfloat16> a,\n"
        "                                 uint32 depth) {\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
        "        ma.splat((bfloat16) 1.0);\n"
        "        CooperativeMatrix<bfloat16,16,16,1> mb;\n"
        "        mb.load(a, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_EQ(runI32Nvptx(src), 1);
}

// The all-bf16 GEMM (bf16 accumulator) stays MIXED-tier on NVPTX — bf16 A/B
// are native but the only native accumulators are f32/i32 — and must keep
// taking the graceful `[xpu-kernel-skipped]` note (observed on the WSL/NVIDIA
// runner 2026-08-10), not regress into the native fragment path.
TEST(NvptxCoopBf16Tests, allBf16MixedTierStillSkipsGracefully) {
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
    int32_t rc = runI32Nvptx(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "expected the kernel-skipped note for the mixed-tier kernel; stderr was:\n"
        << err;
}
