//
// XpuSpirvMixedTierTests — SPIR-V's mixed-tier cooperative-matrix handling.
//
// A cooperative-matrix kernel whose tiles straddle implementation tiers on the
// SPIR-V backend (e.g. bf16 operands -> Portable, f32 accumulator -> Native)
// is SKIPPED with the `[xpu-kernel-skipped]` note. These tests pin that in
// both straddle directions (KernelLowering.cpp guards both):
//   - native accumulator over software operands  (f32 acc, bf16 A/B)
//   - software accumulator over native operands  (f64 acc, f16 A/B — f64 has
//     no SPIR-V coop-matrix config, f16 is the advertised native one)
//
// History: the 2026-08-10 process abort at a bf16 coop-matrix kernel was
// first attributed to this backend, and these tests were written as its
// repro — they PASSED, proving SPIR-V's guard path graceful and relocating
// the real defect to the NVPTX native bf16 fragment layout (where the same
// shape is all-Native and enters the fragment code): see
// NvptxCoopBf16Tests.cpp. They stay as the pin that keeps SPIR-V graceful.
//
// The tests compile with Backend::Spirv only and never launch: the behaviour
// under test is at kernel-*lowering* time, so no Vulkan device is required.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Spirv(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n";

} // namespace

// Direction 1: native accumulator (f32) consuming software-tier operands
// (bf16) — the exact `Ewise.matmulBf16` first shape from the spec's 1.7 repro.
TEST(XpuSpirvMixedTierTests, nativeAccOverSoftwareOperandsSkipsNotAborts) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void bad(KernelBuffer<float32> c, KernelBuffer<bfloat16> a,\n"
        "                           KernelBuffer<bfloat16> b, uint32 depth) {\n"
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
    int32_t rc = runI32Spirv(src);          // pre-fix: SIGABRT here, in codegen
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "expected the kernel-skipped note for the mixed-tier kernel; stderr was:\n"
        << err;
    EXPECT_NE(err.find("bad"), std::string::npos)
        << "the note must name the skipped kernel; stderr was:\n" << err;
}

// Direction 2: software accumulator (f64 — no SPIR-V coop-matrix config) fed
// by native-tier operands (f16 — the advertised native config).
TEST(XpuSpirvMixedTierTests, softwareAccOverNativeOperandsSkipsNotAborts) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void bad2(KernelBuffer<float64> c, KernelBuffer<float16> a,\n"
        "                            KernelBuffer<float16> b, uint32 depth) {\n"
        "        CooperativeMatrix<float64,16,16,2> mc;\n"
        "        mc.splat(0.0);\n"
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
    int32_t rc = runI32Spirv(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "expected the kernel-skipped note for the mixed-tier kernel; stderr was:\n"
        << err;
}

// Control: an all-one-tier kernel on the same backend still compiles — the fix
// must not widen into skipping consistent kernels (spec 2.4). f16 A/B with an
// f32 accumulator is the advertised native config; all-bf16 is all-Portable.
TEST(XpuSpirvMixedTierTests, consistentTierKernelsStillCompile) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void goodNative(KernelBuffer<float32> c, KernelBuffer<float16> a,\n"
        "                                  KernelBuffer<float16> b, uint32 depth) {\n"
        "        CooperativeMatrix<float32,16,16,2> mc;\n"
        "        mc.splat(0.0f);\n"
        "        CooperativeMatrix<float16,16,16,0> ma;\n"
        "        CooperativeMatrix<float16,16,16,1> mb;\n"
        "        ma.load(a, 0, 0, depth);\n"
        "        mb.load(b, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void goodPortable(KernelBuffer<bfloat16> c, KernelBuffer<bfloat16> a,\n"
        "                                    KernelBuffer<bfloat16> b, uint32 depth) {\n"
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
    int32_t rc = runI32Spirv(src);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "consistent-tier kernels must not be skipped; stderr was:\n" << err;
}
