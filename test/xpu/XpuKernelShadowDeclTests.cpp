//
// XpuKernelShadowDeclTests — the kernel-body redeclaration gate.
//
// A kernel body has no block scope inside the device lowering: it keeps
// FLAT per-name maps (scalars, Shared/buffer bases, coop-matrix slots,
// …). A nested local reusing a kernel-level name at a DIFFERENT device
// type used to rebind one map over another and ship wrong device code
// silently — a `Shared<int8> at` plus an `int64 at` in a nested loop
// computed garbage. The lowering now rejects that pair by name.
//
// Sibling blocks reusing a name at the SAME type is what existing
// kernels do and must keep working, so the gate is measured both ways:
// one case that FIRES and one that does NOT.
//
// Compile-level only: no device is touched.
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
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n";

// The measured shape: `at` is a kernel-level Shared<int8> tile and a
// nested while-block declares an `int64 at`.
std::string shadowKernel() {
    return std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void shade(KernelBuffer<int32> out, uint32 n) {\n"
        "        Shared<int8> at = shared int8[64];\n"
        "        uint32 lane = KernelThread.x();\n"
        "        if (lane < 64) { at[lane] = 0; }\n"
        "        Barrier.workgroup();\n"
        "        uint32 i = 0;\n"
        "        while (i < n) {\n"
        "            int64 at = 0;\n"
        "            at = at + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        out[0] = 1;\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
}

// The control: two sibling blocks declaring `t` at the SAME type. The
// flat maps rebind the same kind and type, which is well defined, so
// this must keep compiling.
std::string siblingKernel() {
    return std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void sib(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 lane = KernelThread.x();\n"
        "        if (lane < n) { int32 t = 1; out[lane] = t; }\n"
        "        if (lane < n) { int32 t = 2; out[lane] = out[lane] + t; }\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
}

} // namespace

// FIRES — a kernel-level Shared<int8> and a nested int64 under one name
// is REJECTED at lowering, by name, instead of shipping wrong code.
TEST(XpuKernelShadowDeclTests, differentTypeRedeclarationIsRejected) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Amdgpu, shadowKernel(), &err), 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "the shadowing kernel must be rejected loudly:\n" << err;
    EXPECT_NE(err.find("redeclares 'at'"), std::string::npos)
        << "the rejection must NAME the shadowed local:\n" << err;
    EXPECT_NE(err.find("rename the inner local"), std::string::npos)
        << "the rejection must say what to do about it:\n" << err;
}

// DOES NOT FIRE — same kind, same type, sibling blocks: unchanged.
TEST(XpuKernelShadowDeclTests, sameTypeSiblingRedeclarationStillCompiles) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Amdgpu, siblingKernel(), &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a same-type sibling-block redeclaration must still lower:\n"
        << err;
    EXPECT_EQ(err.find("redeclares"), std::string::npos)
        << "the gate over-fired on a benign redeclaration:\n" << err;
}
