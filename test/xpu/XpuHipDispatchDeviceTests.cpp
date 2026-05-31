//
// CajetaXPU CPU backend (Increment 4.2) — the runtime dispatcher routing to HIP
// on a real AMD device, and degrading to the CPU on demand.
//
// Same host-source SAXPY as the GPU-free CPU dispatch test, but compiled
// --xpu-backend=amdgpu (or amdgpu,cpu). At first device touch the dispatcher
// picks HIP (bundled + available) and routes the whole orchestration
// (allocate/upload/launch/sync/download) through the in-C HIP path
// (hipMalloc / hipMemcpyHtoD / hipModuleLaunchKernel / hipDeviceSynchronize).
// With both amdgpu and cpu bundled, CAJETA_XPU_BACKEND=cpu forces the fall to
// the CPU even on a box that HAS the GPU — the degrade-to-CPU contract, proven
// against a real accelerator. Skips when no ROCm/HIP device is present.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include <cstdlib>

using cajeta_test::CajetaJit;
using cajeta::xpu::amd::HipDriver;

namespace {

const char* kSaxpyHostSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class Saxpy {\n"
    "    @Kernel\n"
    "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 1024;\n"
    "        float32[] hx = new float32[n];\n"
    "        float32[] hy = new float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            hx[i] = 1.0f;\n"
    "            hy[i] = 2.0f;\n"
    "        }\n"
    "        Buffer<float32> x = heap Buffer<float32>(0, n);\n"
    "        Buffer<float32> y = heap Buffer<float32>(0, n);\n"
    "        x.allocate();\n"
    "        y.allocate();\n"
    "        x.upload(hx);\n"
    "        y.upload(hy);\n"
    "        Stream s = Stream.current();\n"
    "        saxpy.launch(s, grid: [4], block: [256])(y, x, 2.0f, n);\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        x.free();\n"
    "        y.free();\n"
    "        float32 sum = 0.0f;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            sum = sum + hy[i];\n"
    "        }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

} // namespace

// The dispatcher routes a host-source @Kernel program to HIP on the real AMD
// device — allocate/upload/launch/sync/download all through the in-C HIP path.
TEST(XpuHipDispatchDeviceTests, saxpyRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 4096.0f);   // 1024 * (2*1 + 2)
}

// Bundle BOTH amdgpu and cpu; CAJETA_XPU_BACKEND=cpu forces the fall to the CPU
// even on a box with the GPU present — the explicit-bundle degrade-to-CPU
// contract, validated against real hardware. GPU-independent (forced to CPU),
// but builds the amdgpu hsaco too, exercising the multi-target bundle.
TEST(XpuHipDispatchDeviceTests, bundledAmdgpuCpuForcedToCpu) {
    setenv("CAJETA_XPU_BACKEND", "cpu", /*overwrite=*/1);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu, cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", o);
    auto fn = jit ? jit->lookup<float (*)()>("run") : nullptr;
    float result = fn ? fn() : 0.0f;
    unsetenv("CAJETA_XPU_BACKEND");

    ASSERT_NE(jit, nullptr);
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(result, 4096.0f);   // ran on the CPU rung
}
