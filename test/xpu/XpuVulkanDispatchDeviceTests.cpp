//
// CajetaXPU CPU backend (Increment 4.3) — the runtime dispatcher routing to
// Vulkan on a real device (RADV), and degrading to the CPU on demand.
//
// The Vulkan rung is the one whose launch ABI forks from the pointer-arg
// kernelParams model: Vulkan's compute entry has no params, only descriptor
// bindings. So the dispatcher, given the uniform argv, uses the per-kernel
// parameter metadata to bind buffer args to their storage buffers and wrap
// scalar args in transient single-element SSBOs — all inside the C runtime's
// ported descriptor-set launch (mirroring VulkanDriver.cpp).
//
// The Vulkan local size is baked into the SPIR-V (kVulkanLocalSizeX = 64), so
// the launch uses block:[64]; gridX work-groups cover n = grid*64.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"   // kVulkanLocalSizeX

#include <cstdlib>
#if defined(_WIN32)
// POSIX setenv/unsetenv are absent on mingw; shim onto _putenv_s.
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// n = 1024, block = kVulkanLocalSizeX (64), grid = 16. Each element 2*1 + 2 = 4
// -> sum 4096 once launched; 2*1024 = 2048 if nothing ran.
std::string saxpyHostSource() {
    const unsigned block = cajeta::xpu::vulkan::kVulkanLocalSizeX;  // 64
    const unsigned n = 1024;
    const unsigned grid = n / block;                               // 16
    return std::string(
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
        "        uint32 n = " + std::to_string(n) + ";\n"
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
        "        saxpy.launch(s, grid: [" + std::to_string(grid) +
        "], block: [" + std::to_string(block) + "])(y, x, 2.0f, n);\n"
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
        "}\n");
}

} // namespace

// The dispatcher routes a host-source @Kernel program to Vulkan on a real
// device: buffers bind as storage buffers, the scalars (a, n) are wrapped in
// transient SSBOs, and the descriptor-set compute pipeline runs SAXPY.
TEST(XpuVulkanDispatchDeviceTests, saxpyRoutesToVulkanOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(saxpyHostSource(), "test.Saxpy", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 4096.0f);
}

// Bundle vulkan + cpu; CAJETA_XPU_BACKEND=cpu forces the fall to the CPU even
// with the Vulkan device present — the canonical degrade-to-CPU bundle.
TEST(XpuVulkanDispatchDeviceTests, bundledVulkanCpuForcedToCpu) {
    setenv("CAJETA_XPU_BACKEND", "cpu", /*overwrite=*/1);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv, cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(saxpyHostSource(), "test.Saxpy", o);
    auto fn = jit ? jit->lookup<float (*)()>("run") : nullptr;
    float result = fn ? fn() : 0.0f;
    unsetenv("CAJETA_XPU_BACKEND");

    ASSERT_NE(jit, nullptr);
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(result, 4096.0f);   // ran on the CPU rung
}
