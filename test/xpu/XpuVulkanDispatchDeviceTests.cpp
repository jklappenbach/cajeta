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
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// n = 1024; each element 2*1 + 2 = 4 -> sum 4096 once launched (2048 if nothing
// ran). `block` sets the launch block dim (and, via the spec-constant workgroup
// size, the actual SPIR-V LocalSize); grid covers n = grid*block.
std::string saxpyHostSource(unsigned block = cajeta::xpu::vulkan::kVulkanLocalSizeX) {
    const unsigned n = 1024;
    const unsigned grid = n / block;
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

// Item 3: an ARBITRARY block size (128, not the baked 64) runs on Vulkan via the
// spec-constant workgroup size. The launch sets SpecId 0/1/2 = block.x/y/z at
// pipeline creation, so GlobalInvocationId spans grid*128 = n and all 1024
// elements are updated (sum 4096). If the workgroup were stuck at the baked 64,
// only the first n/2 would run (sum 3072) — so this distinguishes the feature.
TEST(XpuVulkanDispatchDeviceTests, arbitraryBlockSizeOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(saxpyHostSource(/*block=*/128), "test.Saxpy", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 4096.0f);
}

// Item 5: DYNAMIC (runtime-sized) shared memory on Vulkan. `shared int32[n]`
// (runtime n) is a concrete internal Workgroup array whose length is a spec
// constant (SpecId 3) set from the launch's sharedBytes: at pipeline creation.
// Stage in[t] into the tile, barrier, read tile[(n-1)-t] (a cross-lane read that
// needs the barrier) ⇒ out[t] = in[(n-1)-t]. n=64, sharedBytes=256.
TEST(XpuVulkanDispatchDeviceTests, dynamicSharedOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "import cajeta.xpu.core.Barrier;\n"
        "import cajeta.xpu.core.Shared;\n"
        "public class Dyn {\n"
        "    @Kernel\n"
        "    public static void dynstage(Buffer<int32> out, Buffer<int32> in,\n"
        "                                uint32 n) {\n"
        "        Shared<int32> tile = shared int32[n];\n"
        "        uint32 t = Thread.x();\n"
        "        if (t < n) { tile[t] = in[t]; }\n"
        "        Barrier.workgroup();\n"
        "        if (t < n) { out[t] = tile[(n - 1) - t]; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] hin = new int32[n];\n"
        "        int32[] hout = new int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; hout[i] = 0; }\n"
        "        Buffer<int32> in = heap Buffer<int32>(0, n);\n"
        "        Buffer<int32> out = heap Buffer<int32>(0, n);\n"
        "        in.allocate();\n"
        "        out.allocate();\n"
        "        in.upload(hin);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        dynstage.launch(s, grid: [1], block: [64], sharedBytes: [256])(out, in, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        in.free();\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (int32) ((n - 1) - i)) { return (int32) (100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.Dyn", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != (n-1)-i)";
}

// Item 6: a grid-stride for-each. `for (uint32 i, float32 v : in.range(n))`
// lowers to `for (i = globalId.x; i < n; i += gridSize.x) { v = in[i]; ... }`.
// The launch grid is deliberately SMALLER than n (grid=4, block=64 ⇒ 256
// work-items < n=1024), so the stride must iterate 4× to cover every element.
// out[i] = in[i]*2 with in[i]=1 ⇒ every out[i]=2 ⇒ sum 2048. If the stride were
// broken (one element per thread), only 256 would run ⇒ sum 512 — distinguishing.
TEST(XpuVulkanDispatchDeviceTests, gridStrideForEachOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Grid {\n"
        "    @Kernel\n"
        "    public static void scale(Buffer<float32> out, Buffer<float32> in,\n"
        "                             uint32 n) {\n"
        "        for (uint32 i, float32 v : in.range(n)) {\n"
        "            out[i] = v * 2.0f;\n"
        "        }\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 1024;\n"
        "        float32[] hx = new float32[n];\n"
        "        float32[] hy = new float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hx[i] = 1.0f; hy[i] = 0.0f; }\n"
        "        Buffer<float32> x = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> y = heap Buffer<float32>(0, n);\n"
        "        x.allocate();\n"
        "        y.allocate();\n"
        "        x.upload(hx);\n"
        "        y.upload(hy);\n"
        "        Stream s = Stream.current();\n"
        "        scale.launch(s, grid: [4], block: [64])(y, x, n);\n"
        "        s.sync();\n"
        "        y.download(hy);\n"
        "        x.free();\n"
        "        y.free();\n"
        "        float32 sum = 0.0f;\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + hy[i]; }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.Grid", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 2048.0f);   // all 1024 ran via grid-stride
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
