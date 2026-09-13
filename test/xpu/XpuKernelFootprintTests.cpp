//
// XpuKernelFootprintTests — the MEASURED per-kernel footprint.
//
// The manifest's numbers are modelled by the compiler for a target it cannot
// see, and on a part no arch table knows they are absent entirely. These read
// what the driver actually allocated for the loaded module, so a kernel can
// size itself on hardware the toolchain has never met.
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/HipDriver.h"
#include <string>
using cajeta_test::CajetaJit;

namespace {
const char* kProgram =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.KernelManifest;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.lang.Optional;\n"
    "public final class P {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        KernelBuffer<float32> y #= heap KernelBuffer<float32>((uint64) 256);\n"
    "        KernelBuffer<float32> x #= heap KernelBuffer<float32>((uint64) 256);\n"
    "        y.allocate();\n"
    "        x.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        saxpy.launch(s, grid: [4], block: [64])(y, x, 2.0f, (uint32) 256);\n"
    "        s.sync();\n"
    "        Optional<int32> r = KernelManifest.measuredRegsPerThread(\"saxpy\");\n"
    "        Optional<int32> t = KernelManifest.measuredMaxThreads(\"saxpy\");\n"
    "        Optional<int32> sp = KernelManifest.measuredSpillBytes(\"saxpy\");\n"
    "        if (!r.isPresent()) { return 1; }\n"
    "        if (r.get() <= 0 || r.get() > 512) { return 2; }\n"
    "        if (!t.isPresent()) { return 3; }\n"
    "        if (t.get() < 64) { return 4; }\n"
    "        if (sp.isPresent()) { return 5; }\n"
    "        Optional<int32> u = KernelManifest.measuredRegsPerThread(\"nosuchkernel\");\n"
    "        if (u.isPresent()) { return 6; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";
} // namespace

// The footprint comes back from the loaded code object: a real register count,
// a group-size ceiling, no spill for a kernel this small, and nothing at all
// for a name the runtime never registered.
TEST(XpuKernelFootprint, amdgpuReportsWhatTheDriverAllocated) {
    if (!cajeta::xpu::amd::HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kProgram, "test.P", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// A backend with no such query answers absent rather than zero-as-a-number, so
// a caller keeps whatever it modelled instead of dividing by a guess.
TEST(XpuKernelFootprint, cpuBackendReportsAbsent) {
    const char* program =
        "package test;\n"
        "import cajeta.xpu.KernelManifest;\n"
        "import cajeta.lang.Optional;\n"
        "public final class Q {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> r = KernelManifest.measuredRegsPerThread(\"anything\");\n"
        "        if (r.isPresent()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program, "test.Q", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}
