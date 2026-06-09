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
        "        float32[] hx = heap float32[n];\n"
        "        float32[] hy = heap float32[n];\n"
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
        "        int32[] hin = heap int32[n];\n"
        "        int32[] hout = heap int32[n];\n"
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
        "        float32[] hx = heap float32[n];\n"
        "        float32[] hy = heap float32[n];\n"
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

// Item 2 follow-up: a @Device helper taking Buffer<T> params on the real Vulkan
// device. This is the SPIR-V crux — a kernel buffer is a descriptor HANDLE
// (spirv.VulkanBuffer), so the helper takes that handle by value (not a pointer)
// and its bufferElementPtr routes through resource.getpointer. out[i]=in[i]*3,
// in[i]=i ⇒ sum over [0,256) of 3i = 97920.
TEST(XpuVulkanDispatchDeviceTests, deviceBufferParamHelperOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class DevBuf {\n"
        "    @Device\n"
        "    public static void scale(Buffer<float32> out, Buffer<float32> in,\n"
        "                             uint32 i) {\n"
        "        out[i] = in[i] * 3.0f;\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void k(Buffer<float32> out, Buffer<float32> in) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        scale(out, in, i);\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 256;\n"
        "        float32[] hx = heap float32[n];\n"
        "        float32[] hy = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hx[i] = (float32)i; hy[i] = 0.0f; }\n"
        "        Buffer<float32> x = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> y = heap Buffer<float32>(0, n);\n"
        "        x.allocate();\n"
        "        y.allocate();\n"
        "        x.upload(hx);\n"
        "        y.upload(hy);\n"
        "        Stream s = Stream.current();\n"
        "        k.launch(s, grid: [4], block: [64])(y, x);\n"
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
    auto jit = CajetaJit::compile(src, "test.DevBuf", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 97920.0f);   // sum 3i over [0,256)
}

// Item 7: a POD struct passed BY VALUE as a kernel arg, on the real Vulkan device
// (Strix Halo via RADV). On SPIR-V the struct rides its own descriptor-bound
// storage buffer (the scalar-SSBO mechanism, element type = the struct); the
// kernel reads p.scale/p.bias via OpCompositeExtract to compute
// out[i] = i*scale+bias. scale=2, bias=1, n=256 ⇒ Σ(2i+1) = 65536.
TEST(XpuVulkanDispatchDeviceTests, podStructArgOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Params {\n"
        "    float32 scale;\n"
        "    float32 bias;\n"
        "    public Params(float32 scale, float32 bias)"
        " { this.scale = scale; this.bias = bias; }\n"
        "}\n"
        "public class PodArg {\n"
        "    @Kernel\n"
        "    public static void k(Buffer<float32> out, Params p) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        out[i] = (float32)i * p.scale + p.bias;\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 256;\n"
        "        float32[] hy = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hy[i] = 0.0f; }\n"
        "        Buffer<float32> y = heap Buffer<float32>(0, n);\n"
        "        y.allocate();\n"
        "        y.upload(hy);\n"
        "        Params p = heap Params(2.0f, 1.0f);\n"
        "        Stream s = Stream.current();\n"
        "        k.launch(s, grid: [4], block: [64])(y, p);\n"
        "        s.sync();\n"
        "        y.download(hy);\n"
        "        y.free();\n"
        "        float32 sum = 0.0f;\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + hy[i]; }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.PodArg", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 65536.0f);   // Σ(2i+1) over [0,256)
}

// Item 8 Stage B: a Texture2D sampled through a Sampler with bilinear filtering,
// on the real Vulkan device (RADV / Strix Halo). The Texture2D binds a native
// SAMPLED_IMAGE descriptor, the Sampler a VkSampler (filter/address from its
// modes), and tex.sample(...) lowers to OpImageSampleExplicitLod. A 2×2 image
// {0,1,2,3} sampled at the four texel centers returns the exact texels; the
// dead-center (0.5,0.5) returns the 4-texel average 1.5 — bit-exact, so the
// in-kernel compare uses a small epsilon only to guard GPU weight precision.
TEST(XpuVulkanDispatchDeviceTests, textureSampleOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexSample {\n"
        "    @Kernel\n"
        "    public static void sample(Texture2D tex, Sampler s,\n"
        "                              Buffer<float32> us, Buffer<float32> vs,\n"
        "                              Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { Vector<float32,4> c = tex.sample(s, us[i], vs[i]); out[i] = c.x; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        float32[] pixels = heap float32[4];\n"
        "        pixels[0] = 0.0f; pixels[1] = 1.0f;\n"
        "        pixels[2] = 2.0f; pixels[3] = 3.0f;\n"
        "        Texture2D tex = heap Texture2D(w, h);\n"
        "        tex.upload(pixels);\n"
        "        Sampler samp = heap Sampler(1, 0);\n"   // linear, clamp
        "        uint32 n = 5;\n"
        "        float32[] hus = heap float32[n];\n"
        "        float32[] hvs = heap float32[n];\n"
        "        float32[] hexp = heap float32[n];\n"
        "        hus[0] = 0.25f; hvs[0] = 0.25f; hexp[0] = 0.0f;\n"
        "        hus[1] = 0.75f; hvs[1] = 0.25f; hexp[1] = 1.0f;\n"
        "        hus[2] = 0.25f; hvs[2] = 0.75f; hexp[2] = 2.0f;\n"
        "        hus[3] = 0.75f; hvs[3] = 0.75f; hexp[3] = 3.0f;\n"
        "        hus[4] = 0.5f;  hvs[4] = 0.5f;  hexp[4] = 1.5f;\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> us = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> vs = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        us.allocate();\n"
        "        vs.allocate();\n"
        "        out.allocate();\n"
        "        us.upload(hus);\n"
        "        vs.upload(hvs);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        sample.launch(s, grid: [1], block: [64])(tex, samp, us, vs, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        us.free();\n"
        "        vs.free();\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = hout[i] - hexp[i];\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.TexSample", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: sampled texel != expected)";
}

// Multi-channel float texture (B3): an RGBA32F Texture2D sampled on RADV returns
// all four channels (sample() -> Vector<float32,4>). Clones the R32F device test
// (2x2, 3 coord/out buffers, linear) but the 2x2 texel R channels are (0,1,2,3)
// — so reading .x at the four corners + center reproduces the R32F expecteds —
// and the G/B/A channels (R+10/+20/+30) are checked at one corner.
static const char* kRgbaSampleSrc(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexRgba {\n"
        "    @Kernel\n"
        "    public static void sample(Texture2D tex, Sampler s,\n"
        "                              Buffer<float32> us, Buffer<float32> vs,\n"
        "                              Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<float32,4> c = tex.sample(s, us[i], vs[i]);\n"
        "            out[i*4 + 0] = c.x;\n"
        "            out[i*4 + 1] = c.y;\n"
        "            out[i*4 + 2] = c.z;\n"
        "            out[i*4 + 3] = c.w;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        float32[] pixels = heap float32[16];\n"   // 2*2 RGBA
        "        for (uint32 t = 0; t < 4; t = t + 1) {\n"
        "            float32 r = (float32)(t) * 0.2f;\n"   // all channels in [0,1] (UNORM-safe)
        "            pixels[t*4 + 0] = r;\n"
        "            pixels[t*4 + 1] = r + 0.05f;\n"
        "            pixels[t*4 + 2] = r + 0.1f;\n"
        "            pixels[t*4 + 3] = r + 0.15f;\n"
        "        }\n"
        "        Texture2D tex = heap Texture2D(w, h, ") + fmt + ");\n"
        "        tex.upload(pixels);\n"
        "        Sampler samp = heap Sampler(1, 0);\n"   // linear, clamp
        "        uint32 n = 4;\n"                          // 4 query points (texel centers)
        "        uint32 m = n * 4;\n"                      // 4 RGBA channels each
        "        float32[] hus = heap float32[n];\n"
        "        float32[] hvs = heap float32[n];\n"
        "        hus[0] = 0.25f; hvs[0] = 0.25f;\n"        // texel (0,0)
        "        hus[1] = 0.75f; hvs[1] = 0.25f;\n"        // texel (1,0)
        "        hus[2] = 0.25f; hvs[2] = 0.75f;\n"        // texel (0,1)
        "        hus[3] = 0.75f; hvs[3] = 0.75f;\n"        // texel (1,1)
        "        float32[] hexp = heap float32[m];\n"
        "        for (uint32 t = 0; t < 4; t = t + 1) {\n" // texel t: R,G,B,A = .2t, .2t+.05, +.1, +.15
        "            float32 r = (float32)(t) * 0.2f;\n"
        "            hexp[t*4 + 0] = r;\n"
        "            hexp[t*4 + 1] = r + 0.05f;\n"
        "            hexp[t*4 + 2] = r + 0.1f;\n"
        "            hexp[t*4 + 3] = r + 0.15f;\n"
        "        }\n"
        "        float32[] hout = heap float32[m];\n"
        "        for (uint32 i = 0; i < m; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> us = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> vs = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, m);\n"
        "        us.allocate(); vs.allocate(); out.allocate();\n"
        "        us.upload(hus); vs.upload(hvs); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        sample.launch(s, grid: [1], block: [64])(tex, samp, us, vs, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        us.free(); vs.free(); out.free();\n"
        "        for (uint32 i = 0; i < m; i = i + 1) {\n"
        "            float32 d = hout[i] - hexp[i];\n"
        "            if (d < -0.02f || d > 0.02f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// Single-channel format parametrized by ordinal (R32F/R8_UNORM/R16F): a 2x2
// image {0,1,2,3} sampled at the four texel centers returns the exact texels in
// .x; the dead-center returns the 4-texel average 1.5. All values are f16- and
// unorm8-... no: 0..3 exceed [0,1], so this helper is for the FLOAT single-channel
// formats (R32F/R16F) — every value is exactly representable in binary16.
static const char* kR1SampleSrc(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexR1 {\n"
        "    @Kernel\n"
        "    public static void sample(Texture2D tex, Sampler s,\n"
        "                              Buffer<float32> us, Buffer<float32> vs,\n"
        "                              Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<float32,4> c = tex.sample(s, us[i], vs[i]);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        float32[] pixels = heap float32[4];\n"
        "        pixels[0] = 0.0f; pixels[1] = 1.0f;\n"
        "        pixels[2] = 2.0f; pixels[3] = 3.0f;\n"
        "        Texture2D tex = heap Texture2D(w, h, ") + fmt + ");\n"
        "        tex.upload(pixels);\n"
        "        Sampler samp = heap Sampler(1, 0);\n"   // linear, clamp
        "        uint32 n = 5;\n"
        "        float32[] hus = heap float32[n];\n"
        "        float32[] hvs = heap float32[n];\n"
        "        float32[] hexp = heap float32[n];\n"
        "        hus[0] = 0.25f; hvs[0] = 0.25f; hexp[0] = 0.0f;\n"
        "        hus[1] = 0.75f; hvs[1] = 0.25f; hexp[1] = 1.0f;\n"
        "        hus[2] = 0.25f; hvs[2] = 0.75f; hexp[2] = 2.0f;\n"
        "        hus[3] = 0.75f; hvs[3] = 0.75f; hexp[3] = 3.0f;\n"
        "        hus[4] = 0.5f;  hvs[4] = 0.5f;  hexp[4] = 1.5f;\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> us = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> vs = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        us.allocate(); vs.allocate(); out.allocate();\n"
        "        us.upload(hus); vs.upload(hvs); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        sample.launch(s, grid: [1], block: [64])(tex, samp, us, vs, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        us.free(); vs.free(); out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = hout[i] - hexp[i];\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

TEST(XpuVulkanDispatchDeviceTests, textureSampleRgba32fOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kRgbaSampleSrc("TextureFormat.RGBA32F"),
                                  "test.TexRgba", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA32F sample mismatch at i)";
}

// 8-bit normalized RGBA (the common color texture): bytes 0..255 stored, read
// back as float [0,1]. Same kernel/check as the RGBA32F test (all texel channel
// values are in [0,1]); this exercises the float->unorm8 quantize-on-upload path
// + the R8G8B8A8_UNORM device image, within unorm8 quantization of the 0.02 tol.
TEST(XpuVulkanDispatchDeviceTests, textureSampleRgba8UnormOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kRgbaSampleSrc("TextureFormat.RGBA8_UNORM"),
                                  "test.TexRgba", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA8_UNORM sample mismatch at i)";
}

// Half-float single channel (R16F): the cheap-HDR encoding — float uploaded,
// binary16 stored (VK_FORMAT_R16_SFLOAT), read back as float. Texel values
// {0,1,2,3} are all exactly representable in binary16, so the same texel-center
// + dead-center expecteds as the R32F test hold bit-exactly.
TEST(XpuVulkanDispatchDeviceTests, textureSampleR16fOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kR1SampleSrc("TextureFormat.R16F"),
                                  "test.TexR1", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: R16F sample mismatch at i)";
}

// Half-float RGBA (RGBA16F): four-channel cheap HDR (VK_FORMAT_R16G16B16A16_SFLOAT).
// Same kernel/check as the RGBA32F test; the channel values (.2t .. +.15) are
// within the 0.02 tol of their binary16 round-trip.
TEST(XpuVulkanDispatchDeviceTests, textureSampleRgba16fOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kRgbaSampleSrc("TextureFormat.RGBA16F"),
                                  "test.TexRgba", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA16F sample mismatch at i)";
}

// texelFetch on device (B3): `tex.fetch(x, y)` reads the exact integer texel of
// the sampled image with NO Sampler, lowering to OpImageFetch. One thread per
// texel decodes its (x, y) from the row-major index; all four channels must come
// back exactly as stored (unfiltered) — the device twin of the CPU fetch test.
static const char* kRgbaFetchSrc(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexFetch {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture2D tex, Buffer<float32> out,\n"
        "                             uint32 w, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 y = i / w;\n"
        "            uint32 x = i - y * w;\n"
        "            Vector<float32,4> c = tex.fetch(x, y);\n"
        "            out[i*4 + 0] = c.x;\n"
        "            out[i*4 + 1] = c.y;\n"
        "            out[i*4 + 2] = c.z;\n"
        "            out[i*4 + 3] = c.w;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        float32[] pixels = heap float32[16];\n"   // 2*2 RGBA
        "        for (uint32 t = 0; t < 4; t = t + 1) {\n"
        "            float32 r = (float32)(t) * 0.2f;\n"
        "            pixels[t*4 + 0] = r;\n"
        "            pixels[t*4 + 1] = r + 0.05f;\n"
        "            pixels[t*4 + 2] = r + 0.1f;\n"
        "            pixels[t*4 + 3] = r + 0.15f;\n"
        "        }\n"
        "        Texture2D tex = heap Texture2D(w, h, ") + fmt + ");\n"
        "        tex.upload(pixels);\n"
        "        uint32 n = 4;\n"
        "        uint32 m = n * 4;\n"
        "        float32[] hexp = heap float32[m];\n"
        "        for (uint32 t = 0; t < 4; t = t + 1) {\n"
        "            float32 r = (float32)(t) * 0.2f;\n"
        "            hexp[t*4 + 0] = r;\n"
        "            hexp[t*4 + 1] = r + 0.05f;\n"
        "            hexp[t*4 + 2] = r + 0.1f;\n"
        "            hexp[t*4 + 3] = r + 0.15f;\n"
        "        }\n"
        "        float32[] hout = heap float32[m];\n"
        "        for (uint32 i = 0; i < m; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, m);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [64])(tex, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < m; i = i + 1) {\n"
        "            float32 d = hout[i] - hexp[i];\n"
        "            if (d < -0.02f || d > 0.02f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 Step 2b: integer texelFetch source — a Texture2D<int32|uint32> RGBA32I/UI
// image whose four channels are read back as exact integers (OpImageFetch on an
// integer-sampled image: spirv.Image sampled-type = i32, result <4 x i32>).
static const char* kRgbaIntFetchSrc(const char* elem, const char* fmt) {
    static std::string s;
    std::string e(elem);
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexFetchInt {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture2D<") + e + "> tex, Buffer<" + e + "> out,\n"
        "                             uint32 w, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 y = i / w;\n"
        "            uint32 x = i - y * w;\n"
        "            Vector<" + e + ",4> c = tex.fetch(x, y);\n"
        "            out[i*4 + 0] = c.x;\n"
        "            out[i*4 + 1] = c.y;\n"
        "            out[i*4 + 2] = c.z;\n"
        "            out[i*4 + 3] = c.w;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        " + e + "[] pixels = heap " + e + "[16];\n"
        "        for (uint32 t = 0; t < 4; t = t + 1) {\n"
        "            " + e + " base = (" + e + ")(t) * 10;\n"
        "            pixels[t*4 + 0] = base + 1;\n"
        "            pixels[t*4 + 1] = base + 2;\n"
        "            pixels[t*4 + 2] = base + 3;\n"
        "            pixels[t*4 + 3] = base + 4;\n"
        "        }\n"
        "        Texture2D<" + e + "> tex = heap Texture2D<" + e + ">(w, h, " + fmt + ");\n"
        "        tex.upload(pixels);\n"
        "        uint32 n = 4;\n"
        "        uint32 m = n * 4;\n"
        "        " + e + "[] hout = heap " + e + "[m];\n"
        "        for (uint32 i = 0; i < m; i = i + 1) { hout[i] = 0; }\n"
        "        Buffer<" + e + "> out = heap Buffer<" + e + ">(0, m);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [64])(tex, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 t = 0; t < 4; t = t + 1) {\n"
        "            " + e + " base = (" + e + ")(t) * 10;\n"
        "            if (hout[t*4 + 0] != base + 1) { return (int32)(100 + t*4 + 0); }\n"
        "            if (hout[t*4 + 1] != base + 2) { return (int32)(100 + t*4 + 1); }\n"
        "            if (hout[t*4 + 2] != base + 3) { return (int32)(100 + t*4 + 2); }\n"
        "            if (hout[t*4 + 3] != base + 4) { return (int32)(100 + t*4 + 3); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 texture dims: Texture3D fetch on a real Vulkan device — a 2x2x2 R32F volume
// (VK_IMAGE_TYPE_3D, OpImageFetch on a Dim=3 image) read voxel-exact by integer
// (x, y, z). The device twin of texture3dFetchOnCpu.
static const char* kTex3dFetchSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture3D;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex3dFetch {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture3D vol, Buffer<float32> out,\n"
        "                             uint32 w, uint32 h, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 z = i / (w*h);\n"
        "            uint32 r = i - z*(w*h);\n"
        "            uint32 y = r / w;\n"
        "            uint32 x = r - y*w;\n"
        "            Vector<float32,4> c = vol.fetch(x, y, z);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 d = 2; uint32 n = 8;\n"
        "        float32[] voxels = heap float32[8];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { voxels[i] = (float32)(i); }\n"
        "        Texture3D vol = heap Texture3D(w, h, d);\n"
        "        vol.upload(voxels);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [64])(vol, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout); out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// Texture3D trilinear sample on a real Vulkan device: nearest at voxel centers is
// exact; a trilinear midpoint (u=0.5 between voxel 0 and 1) reads 0.5.
static const char* kTex3dSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture3D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex3dSamp {\n"
        "    @Kernel\n"
        "    public static void samp(Texture3D vol, Sampler sn, Buffer<float32> out,\n"
        "                            uint32 w, uint32 h, uint32 d, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 z = i / (w*h);\n"
        "            uint32 r = i - z*(w*h);\n"
        "            uint32 y = r / w;\n"
        "            uint32 x = r - y*w;\n"
        "            float32 u = ((float32)(x) + 0.5f) / (float32)(w);\n"
        "            float32 v = ((float32)(y) + 0.5f) / (float32)(h);\n"
        "            float32 ww = ((float32)(z) + 0.5f) / (float32)(d);\n"
        "            Vector<float32,4> c = vol.sample(sn, u, v, ww);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void mid(Texture3D vol, Sampler sl, Buffer<float32> out) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < 1) {\n"
        "            Vector<float32,4> c = vol.sample(sl, 0.5f, 0.25f, 0.25f);\n"
        "            out[0] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 d = 2; uint32 n = 8;\n"
        "        float32[] voxels = heap float32[8];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { voxels[i] = (float32)(i); }\n"
        "        Texture3D vol = heap Texture3D(w, h, d);\n"
        "        vol.upload(voxels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"
        "        Sampler sl = heap Sampler(1, 0);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(vol, sn, out, w, h, d, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 dv = hout[i] - (float32)(i);\n"
        "            if (dv < -0.02f || dv > 0.02f) { out.free(); return (int32)(100 + i); }\n"
        "        }\n"
        "        float32[] hmid = heap float32[1]; hmid[0] = -1.0f;\n"
        "        Buffer<float32> mo = heap Buffer<float32>(0, 1);\n"
        "        mo.allocate(); mo.upload(hmid);\n"
        "        mid.launch(s, grid: [1], block: [1])(vol, sl, mo);\n"
        "        s.sync();\n"
        "        mo.download(hmid); mo.free(); out.free();\n"
        "        float32 dm = hmid[0] - 0.5f;\n"
        "        if (dm < -0.02f || dm > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// B3 texture dims: Texture1D fetch on a real Vulkan device — a width-4 R32F row
// (VK_IMAGE_TYPE_1D, OpImageFetch on a Dim=1D image) read texel-exact by integer
// x. The device twin of texture1dFetchOnCpu.
static const char* kTex1dFetchSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture1D;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex1dFetch {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture1D row, Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vector<float32,4> c = row.fetch(i);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 4; uint32 n = 4;\n"
        "        float32[] texels = heap float32[4];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture1D row = heap Texture1D(w);\n"
        "        row.upload(texels);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [64])(row, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout); out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// Texture1D linear sample on a real Vulkan device: nearest at texel centers is
// exact; a linear midpoint (u=0.5 between texel 0 and 1 of a width-2 row) reads
// 0.5. Exercises the Dim=1D OpImageSampleExplicitLod (scalar coord) + a Sampler.
static const char* kTex1dSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture1D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex1dSamp {\n"
        "    @Kernel\n"
        "    public static void samp(Texture1D row, Sampler sn, Buffer<float32> out,\n"
        "                            uint32 w, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            float32 u = ((float32)(i) + 0.5f) / (float32)(w);\n"
        "            Vector<float32,4> c = row.sample(sn, u);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void mid(Texture1D row, Sampler sl, Buffer<float32> out) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < 1) {\n"
        "            Vector<float32,4> c = row.sample(sl, 0.5f);\n"
        "            out[0] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 n = 2;\n"
        "        float32[] texels = heap float32[2];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture1D row = heap Texture1D(w);\n"
        "        row.upload(texels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"
        "        Sampler sl = heap Sampler(1, 0);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(row, sn, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 dv = hout[i] - (float32)(i);\n"
        "            if (dv < -0.02f || dv > 0.02f) { out.free(); return (int32)(100 + i); }\n"
        "        }\n"
        "        float32[] hmid = heap float32[1]; hmid[0] = -1.0f;\n"
        "        Buffer<float32> mo = heap Buffer<float32>(0, 1);\n"
        "        mo.allocate(); mo.upload(hmid);\n"
        "        mid.launch(s, grid: [1], block: [1])(row, sl, mo);\n"
        "        s.sync();\n"
        "        mo.download(hmid); mo.free(); out.free();\n"
        "        float32 dm = hmid[0] - 0.5f;\n"
        "        if (dm < -0.02f || dm > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// B3 texture dims: Texture2DArray fetch on a real Vulkan device — a 2x2x3 R32F
// layered image (VK_IMAGE_VIEW_TYPE_2D_ARRAY, OpImageFetch on an Arrayed image)
// read texel-exact by (x, y, layer). The device twin of texture2dArrayFetchOnCpu.
static const char* kTex2daFetchSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2DArray;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex2daFetch {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture2DArray arr, Buffer<float32> out,\n"
        "                             uint32 w, uint32 h, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 layer = i / (w*h);\n"
        "            uint32 r = i - layer*(w*h);\n"
        "            uint32 y = r / w;\n"
        "            uint32 x = r - y*w;\n"
        "            Vector<float32,4> c = arr.fetch(x, y, layer);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 layers = 3; uint32 n = 12;\n"
        "        float32[] texels = heap float32[12];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture2DArray arr = heap Texture2DArray(w, h, layers);\n"
        "        arr.upload(texels);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [64])(arr, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout); out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// Texture2DArray sample on a real Vulkan device: nearest at texel centers per
// layer is exact; a within-layer-1 bilinear midpoint (u=0.5 between texel 4 and
// 5) reads 4.5. Exercises the Arrayed OpImageSampleExplicitLod (3-comp coord,
// layer as the 3rd component) + a Sampler.
static const char* kTex2daSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2DArray;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex2daSamp {\n"
        "    @Kernel\n"
        "    public static void samp(Texture2DArray arr, Sampler sn, Buffer<float32> out,\n"
        "                            uint32 w, uint32 h, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 layer = i / (w*h);\n"
        "            uint32 r = i - layer*(w*h);\n"
        "            uint32 y = r / w;\n"
        "            uint32 x = r - y*w;\n"
        "            float32 u = ((float32)(x) + 0.5f) / (float32)(w);\n"
        "            float32 v = ((float32)(y) + 0.5f) / (float32)(h);\n"
        "            Vector<float32,4> c = arr.sample(sn, u, v, layer);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void mid(Texture2DArray arr, Sampler sl, Buffer<float32> out) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < 1) {\n"
        "            Vector<float32,4> c = arr.sample(sl, 0.5f, 0.25f, 1);\n"
        "            out[0] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 layers = 3; uint32 n = 12;\n"
        "        float32[] texels = heap float32[12];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture2DArray arr = heap Texture2DArray(w, h, layers);\n"
        "        arr.upload(texels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"
        "        Sampler sl = heap Sampler(1, 0);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(arr, sn, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 dv = hout[i] - (float32)(i);\n"
        "            if (dv < -0.02f || dv > 0.02f) { out.free(); return (int32)(100 + i); }\n"
        "        }\n"
        "        float32[] hmid = heap float32[1]; hmid[0] = -1.0f;\n"
        "        Buffer<float32> mo = heap Buffer<float32>(0, 1);\n"
        "        mo.allocate(); mo.upload(hmid);\n"
        "        mid.launch(s, grid: [1], block: [1])(arr, sl, mo);\n"
        "        s.sync();\n"
        "        mo.download(hmid); mo.free(); out.free();\n"
        "        float32 dm = hmid[0] - 4.5f;\n"
        "        if (dm < -0.02f || dm > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// Integer Texture3D fetch source — a Texture3D<int32|uint32> RGBA32I/UI volume
// (VK_FORMAT_R32G32B32A32_SINT 3-D image, OpImageFetch → <4 x i32>).
static const char* kTex3dIntFetchSrc(const char* elem, const char* fmt) {
    static std::string s;
    std::string e(elem);
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture3D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex3dIntFetch {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture3D<") + e + "> vol, Buffer<" + e + "> out,\n"
        "                             uint32 w, uint32 h, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 z = i / (w*h);\n"
        "            uint32 r = i - z*(w*h);\n"
        "            uint32 y = r / w;\n"
        "            uint32 x = r - y*w;\n"
        "            Vector<" + e + ",4> c = vol.fetch(x, y, z);\n"
        "            out[i*4 + 0] = c.x; out[i*4 + 1] = c.y;\n"
        "            out[i*4 + 2] = c.z; out[i*4 + 3] = c.w;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 d = 2; uint32 n = 8;\n"
        "        " + e + "[] voxels = heap " + e + "[32];\n"
        "        for (uint32 t = 0; t < n; t = t + 1) {\n"
        "            " + e + " base = (" + e + ")(t) * 10;\n"
        "            voxels[t*4 + 0] = base + 1; voxels[t*4 + 1] = base + 2;\n"
        "            voxels[t*4 + 2] = base + 3; voxels[t*4 + 3] = base + 4;\n"
        "        }\n"
        "        Texture3D<" + e + "> vol = heap Texture3D<" + e + ">(w, h, d, " + fmt + ");\n"
        "        vol.upload(voxels);\n"
        "        uint32 m = n * 4;\n"
        "        " + e + "[] hout = heap " + e + "[m];\n"
        "        for (uint32 i = 0; i < m; i = i + 1) { hout[i] = 0; }\n"
        "        Buffer<" + e + "> out = heap Buffer<" + e + ">(0, m);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [64])(vol, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout); out.free();\n"
        "        for (uint32 t = 0; t < n; t = t + 1) {\n"
        "            " + e + " base = (" + e + ")(t) * 10;\n"
        "            if (hout[t*4 + 0] != base + 1) { return (int32)(100 + t*4 + 0); }\n"
        "            if (hout[t*4 + 1] != base + 2) { return (int32)(100 + t*4 + 1); }\n"
        "            if (hout[t*4 + 2] != base + 3) { return (int32)(100 + t*4 + 2); }\n"
        "            if (hout[t*4 + 3] != base + 4) { return (int32)(100 + t*4 + 3); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3: two Sampler descriptors in one kernel on a real Vulkan device — nearest +
// linear bound to distinct bindings, both sampling the same image. 2x2 {0,1,2,3}
// at the center: nearest -> 3.0, linear -> 1.5.
static const char* kTwoSamplersSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TwoSamp {\n"
        "    @Kernel\n"
        "    public static void both(Texture2D tex, Sampler sn, Sampler sl,\n"
        "                            Buffer<float32> out) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < 1) {\n"
        "            Vector<float32,4> a = tex.sample(sn, 0.5f, 0.5f);\n"
        "            Vector<float32,4> b = tex.sample(sl, 0.5f, 0.5f);\n"
        "            out[0] = a.x; out[1] = b.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2;\n"
        "        float32[] pixels = heap float32[4];\n"
        "        pixels[0] = 0.0f; pixels[1] = 1.0f; pixels[2] = 2.0f; pixels[3] = 3.0f;\n"
        "        Texture2D tex = heap Texture2D(w, h);\n"
        "        tex.upload(pixels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"
        "        Sampler sl = heap Sampler(1, 0);\n"
        "        float32[] hout = heap float32[2]; hout[0] = -1.0f; hout[1] = -1.0f;\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, 2);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        both.launch(s, grid: [1], block: [1])(tex, sn, sl, out);\n"
        "        s.sync();\n"
        "        out.download(hout); out.free();\n"
        "        float32 dn = hout[0] - 3.0f;\n"
        "        float32 dl = hout[1] - 1.5f;\n"
        "        if (dn < -0.02f || dn > 0.02f) { return (int32)(100); }\n"
        "        if (dl < -0.02f || dl > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

// OpImageFetch on a real Vulkan device (RADV / Strix Halo): the sampled image
// (Sampled=1) is fetched by exact integer coord — no sampler descriptor — and
// every RGBA32F channel reads back bit-exact.
// B3 mipmaps: the combined mip kernel on a real Vulkan device (RADV / Strix Halo)
// — the device twin of XpuCpuDispatchTests.mipmapFetchAndSampleLodOnCpu and the
// shape that originally appeared to "hang" (it was the pre-reboot bad build, not
// the mip path). One 2-level R32F Texture2D(2,2,R32F,2): L0 fetch=3, L1 fetch=99,
// L1 sampleLod=99 (the sampler's maxLod=VK_LOD_CLAMP_NONE lets explicit LOD reach
// level 1; maxLod=0 would clamp every sample to level 0).
static const char* kMipSrcDevice() {
    static std::string s;
    s = std::string("package test;\n")
        + "import cajeta.xpu.core.Buffer;\n"
        + "import cajeta.xpu.core.Texture2D;\n"
        + "import cajeta.xpu.core.TextureFormat;\n"
        + "import cajeta.xpu.core.Sampler;\n"
        + "import cajeta.xpu.core.Stream;\n"
        + "import cajeta.xpu.core.Thread;\n"
        + "public class MipDev {\n"
        + "    @Kernel\n"
        + "    public static void mip(Texture2D tex, Sampler sl, Buffer<float32> out) {\n"
        + "        uint32 i = Thread.globalIdX();\n"
        + "        if (i < 1) {\n"
        + "            Vector<float32,4> a = tex.fetchLod(1, 1, 0);\n"
        + "            Vector<float32,4> b = tex.fetchLod(0, 0, 1);\n"
        + "            Vector<float32,4> c = tex.sampleLod(sl, 0.5f, 0.5f, 1.0f);\n"
        + "            out[0] = a.x; out[1] = b.x; out[2] = c.x;\n"
        + "        }\n"
        + "    }\n"
        + "    public static int32 run() {\n"
        + "        uint32 w = 2; uint32 h = 2;\n"
        + "        Texture2D tex = heap Texture2D(w, h, TextureFormat.R32F, 2);\n"
        + "        float32[] l0 = heap float32[4];\n"
        + "        l0[0] = 0.0f; l0[1] = 1.0f; l0[2] = 2.0f; l0[3] = 3.0f;\n"
        + "        float32[] l1 = heap float32[1]; l1[0] = 99.0f;\n"
        + "        tex.uploadLevel(0, l0);\n"
        + "        tex.uploadLevel(1, l1);\n"
        + "        Sampler sl = heap Sampler(1, 0);\n"
        + "        float32[] hout = heap float32[3];\n"
        + "        for (uint32 i = 0; i < 3; i = i + 1) { hout[i] = -1.0f; }\n"
        + "        Buffer<float32> out = heap Buffer<float32>(3);\n"
        + "        out.upload(hout);\n"
        + "        Stream s = Stream.current();\n"
        + "        mip.launch(s, grid: [1], block: [1])(tex, sl, out);\n"
        + "        s.sync();\n"
        + "        out.download(hout);\n"
        + "        if (hout[0] != 3.0f) { return (int32)(100); }\n"
        + "        if (hout[1] != 99.0f) { return (int32)(200); }\n"
        + "        float32 dc = hout[2] - 99.0f;\n"
        + "        if (dc < -0.02f || dc > 0.02f) { return (int32)(300); }\n"
        + "        return 777;\n"
        + "    }\n"
        + "}\n";
    return s.c_str();
}

TEST(XpuVulkanDispatchDeviceTests, mipmapFetchAndSampleLodOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kMipSrcDevice(), "test.MipDev", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100: L0 fetch; 200: L1 fetch; 300: L1 sampleLod)";
}

TEST(XpuVulkanDispatchDeviceTests, textureFetchRgba32fOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kRgbaFetchSrc("TextureFormat.RGBA32F"),
                                  "test.TexFetch", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA32F fetch mismatch at i)";
}

// B3 Step 2b: integer OpImageFetch on a real Vulkan device (RADV / Strix Halo) —
// an RGBA32I sampled image (i32 sampled type, VK_FORMAT_R32G32B32A32_SINT) fetched
// by integer coord; every channel reads back as the exact stored signed integer.
TEST(XpuVulkanDispatchDeviceTests, textureFetchRgba32iOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kRgbaIntFetchSrc("int32", "TextureFormat.RGBA32I"),
                                  "test.TexFetchInt", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (RGBA32I device fetch mismatch)";
}

// B3 texture dims: Texture3D fetch on a real Vulkan device — VK_IMAGE_TYPE_3D
// sampled image, OpImageFetch on a Dim=3 image, 2x2x2 volume voxel-exact.
TEST(XpuVulkanDispatchDeviceTests, texture3dFetchOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex3dFetchSrc(), "test.Tex3dFetch", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 3D voxel fetch mismatch)";
}

// Texture3D trilinear sample on a real Vulkan device — nearest at voxel centers
// (exact) + a trilinear midpoint (voxel 0/1 blend = 0.5).
TEST(XpuVulkanDispatchDeviceTests, texture3dSampleOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex3dSampleSrc(), "test.Tex3dSamp", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: nearest; 200: trilinear midpoint)";
}

// B3 texture dims: Texture1D fetch on a real Vulkan device — VK_IMAGE_TYPE_1D
// sampled image, OpImageFetch on a Dim=1D image (Sampled1D cap), width-4 row
// texel-exact.
TEST(XpuVulkanDispatchDeviceTests, texture1dFetchOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex1dFetchSrc(), "test.Tex1dFetch", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 1D texel fetch mismatch)";
}

// Texture1D linear sample on a real Vulkan device — nearest at texel centers
// (exact) + a linear midpoint (texel 0/1 blend = 0.5).
TEST(XpuVulkanDispatchDeviceTests, texture1dSampleOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex1dSampleSrc(), "test.Tex1dSamp", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: nearest; 200: linear midpoint)";
}

// B3 texture dims: Texture2DArray fetch on a real Vulkan device — a layered 2-D
// image (VIEW_TYPE_2D_ARRAY), OpImageFetch on an Arrayed image, 2x2x3 texel-exact.
TEST(XpuVulkanDispatchDeviceTests, texture2dArrayFetchOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex2daFetchSrc(), "test.Tex2daFetch", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: layered texel fetch mismatch)";
}

// Texture2DArray sample on a real Vulkan device — nearest per layer (exact) + a
// within-layer-1 bilinear midpoint (texel 4/5 blend = 4.5).
TEST(XpuVulkanDispatchDeviceTests, texture2dArraySampleOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex2daSampleSrc(), "test.Tex2daSamp", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: nearest; 200: layer-1 midpoint)";
}

// Integer Texture3D fetch on a real Vulkan device — RGBA32I 3-D image
// (VK_FORMAT_R32G32B32A32_SINT), integer OpImageFetch on a Dim=3 image, voxel-exact.
TEST(XpuVulkanDispatchDeviceTests, texture3dFetchRgba32iOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTex3dIntFetchSrc("int32", "TextureFormat.RGBA32I"),
                                  "test.Tex3dIntFetch", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (3D RGBA32I device fetch mismatch)";
}

// B3: two Sampler descriptors in one kernel on a real Vulkan device (RADV) — both
// bound to distinct set-0 bindings, sampling the same image (nearest -> 3.0,
// linear -> 1.5). Confirms multiple sampler descriptors per dispatch.
TEST(XpuVulkanDispatchDeviceTests, twoSamplersInOneKernelOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kTwoSamplersSrc(), "test.TwoSamp", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100: nearest=3.0; 200: linear=1.5)";
}


// Writable images: an Image2D bound as a STORAGE_IMAGE that a compute kernel
// WRITES with img.store(x, y, value) (OpImageWrite), then the host reads back
// with img.download(...). On the real Vulkan device (RADV / Strix Halo) the
// Image2D binds a STORAGE_IMAGE descriptor in GENERAL layout; the store lowers
// to a single OpImageWrite (the cajeta-spirv llvm.spv.resource.store.2d
// intrinsic), and StorageImageWriteWithoutFormat is auto-added for the Unknown
// R32 format. One thread per texel writes its linear index; the readback must
// be the exact 0..w*h-1 ramp — bit-exact (integers held in f32).
TEST(XpuVulkanDispatchDeviceTests, imageStoreOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Image2D;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class ImgStore {\n"
        "    @Kernel\n"
        "    public static void fill(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < w * h) {\n"
        "            uint32 x = i % w;\n"
        "            uint32 y = i / w;\n"
        "            img.store(x, y, (float32)(y * w + x));\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 4;\n"
        "        uint32 h = 4;\n"
        "        uint32 n = w * h;\n"
        "        Image2D img = heap Image2D(w, h);\n"
        "        Stream s = Stream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        float32[] out = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { out[i] = -1.0f; }\n"
        "        img.download(out);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = out[i] - (float32) i;\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.ImgStore", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: stored texel != expected)";
}

// Image read (the read twin of imageStore): a storage image is filled by one
// dispatch, then a SECOND dispatch does a read-modify-write — v = img.load(x, y)
// then img.store(x, y, 2v + 1). This exercises OpImageRead (the cajeta-spirv
// llvm.spv.resource.load.2d intrinsic) AND the read-after-write memory barrier
// between consecutive dispatches on the same storage image. The readback must be
// 2*idx + 1 per texel — bit-exact on the real Vulkan device (RADV / Strix Halo).
TEST(XpuVulkanDispatchDeviceTests, imageLoadStoreRmwOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan device/driver available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Image2D;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class ImgRmw {\n"
        "    @Kernel\n"
        "    public static void fill(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < w * h) { img.store(i % w, i / w, (float32)(i / w * w + i % w)); }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void rmw(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < w * h) {\n"
        "            uint32 x = i % w;\n"
        "            uint32 y = i / w;\n"
        "            float32 v = img.load(x, y);\n"
        "            img.store(x, y, 2.0f * v + 1.0f);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 4;\n"
        "        uint32 h = 4;\n"
        "        uint32 n = w * h;\n"
        "        Image2D img = heap Image2D(w, h);\n"
        "        Stream s = Stream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        rmw.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        float32[] out = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { out[i] = -1.0f; }\n"
        "        img.download(out);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = out[i] - (float32)(2 * i + 1);\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.ImgRmw", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 2*loaded+1 != expected)";
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

// Part C inc 3b — the full ray-query path on a real RT device: a host-built
// bottom-level AS over one AABB ([0,1]^3), bound as a descriptor, walked by a
// RayQuery inside the kernel. For each query point we cast a degenerate
// near-zero-length ray and count AABB candidates (the RTNN spatial-index
// pattern): a point inside the box reports 1 candidate, a point outside 0.
// Gated on rayQueryAvailable() — skips on a plain compute device (the build
// natives no-op there, so there is nothing meaningful to run).
TEST(XpuVulkanDispatchDeviceTests, rayQuerySpatialIndexOnDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query (acceleration-structure) device";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.AccelerationStructure;\n"
        "import cajeta.xpu.core.RayQuery;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class RQ {\n"
        "    @Kernel\n"
        "    public static void query(AccelerationStructure scene,\n"
        "                             Buffer<float32> px, Buffer<float32> py,\n"
        "                             Buffer<float32> pz, Buffer<uint32> out,\n"
        "                             uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            RayQuery rq;\n"
        "            rq.initialize(scene, 0, 255,\n"
        "                          px[i], py[i], pz[i], 0.0f,\n"
        "                          0.0f, 0.0f, 1.0f, 0.001f);\n"
        "            uint32 c = 0;\n"
        "            while (rq.proceed()) {\n"
        "                if (rq.candidateType() == 1) { c = c + 1; }\n"
        "            }\n"
        "            out[i] = c;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float32[] boxes = heap float32[6];\n"
        "        boxes[0] = 0.0f; boxes[1] = 0.0f; boxes[2] = 0.0f;\n"
        "        boxes[3] = 1.0f; boxes[4] = 1.0f; boxes[5] = 1.0f;\n"
        "        AccelerationStructure scene = heap AccelerationStructure(boxes, 1);\n"
        "        uint32 n = 2;\n"
        "        float32[] hpx = heap float32[n];\n"
        "        float32[] hpy = heap float32[n];\n"
        "        float32[] hpz = heap float32[n];\n"
        "        hpx[0] = 0.5f; hpy[0] = 0.5f; hpz[0] = 0.5f;\n"   // inside box
        "        hpx[1] = 5.0f; hpy[1] = 5.0f; hpz[1] = 5.0f;\n"   // outside box
        "        uint32[] hout = heap uint32[n];\n"
        "        hout[0] = 99; hout[1] = 99;\n"
        "        Buffer<float32> px = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> py = heap Buffer<float32>(0, n);\n"
        "        Buffer<float32> pz = heap Buffer<float32>(0, n);\n"
        "        Buffer<uint32> out = heap Buffer<uint32>(0, n);\n"
        "        px.allocate(); py.allocate(); pz.allocate(); out.allocate();\n"
        "        px.upload(hpx); py.upload(hpy); pz.upload(hpz); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        query.launch(s, grid: [1], block: [64])(scene, px, py, pz, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        px.free(); py.free(); pz.free(); out.free();\n"
        "        if (hout[0] != 1) { return 101; }\n"  // inside: exactly 1 candidate (one box)
        "        if (hout[1] != 0) { return 102; }\n"  // outside: no candidate
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(src, "test.RQ", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (101: inside-box query saw no candidate; "
                         "102: outside-box query saw a candidate)";
}
