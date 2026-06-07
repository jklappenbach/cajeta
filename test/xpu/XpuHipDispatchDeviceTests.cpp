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
#if defined(_WIN32)
// POSIX setenv/unsetenv are absent on mingw; shim onto _putenv_s.
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

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

// Item 4 (user path): --xpu-arch=gfx1100,gfx1151 builds a MULTI-ARCH bundle for
// the kernel; the runtime loads it on the real device (gfx1151) and runs SAXPY —
// proving the compiler's arch-list plumbing -> assembleHsacoBundle -> dispatch.
TEST(XpuHipDispatchDeviceTests, multiArchBundleRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    o.xpuArch = "gfx1100,gfx1151";
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 4096.0f);
}

// Item 6: a grid-stride for-each on the real AMD device. The stride comes from
// the HSA dispatch packet's grid_size field. Grid is deliberately smaller than n
// (grid=4, block=64 ⇒ 256 work-items < n=1024), so the loop must stride 4× to
// cover all of out[i]=in[i]*2 (in[i]=1) ⇒ sum 2048. One element per thread would
// give 512 — distinguishing.
TEST(XpuHipDispatchDeviceTests, gridStrideForEachRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
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
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.Grid", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 2048.0f);   // all 1024 ran via grid-stride
}

// Item 2 follow-up: a @Device helper taking Buffer<T> params, on the real AMD
// device. The kernel passes its two buffer bases (addrspace(1) pointers) into the
// inlined helper, which reads `in` and writes `out`. out[i]=in[i]*3, in[i]=i ⇒
// sum over [0,256) of 3i = 3·(255·256/2) = 97920.
TEST(XpuHipDispatchDeviceTests, deviceBufferParamHelperRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
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
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.DevBuf", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 97920.0f);   // sum 3i over [0,256)
}

// Item 7: a POD struct passed BY VALUE as a kernel arg, on the real AMD device.
// `Params { float32 scale; float32 bias; }` rides the hipModuleLaunch kernelParams
// ABI by value; the kernel reads p.scale/p.bias to compute out[i] = i*scale+bias.
// scale=2, bias=1, n=256 ⇒ Σ(2i+1) = 2·(255·256/2)+256 = 65536.
TEST(XpuHipDispatchDeviceTests, podStructArgRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
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
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.PodArg", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 65536.0f);   // Σ(2i+1) over [0,256)
}

// Item 8 Stage C: a Texture2D sampled through a Sampler with bilinear filtering,
// on the real AMD device (gfx1151). The Texture2D is a hipArray; at launch the
// runtime builds a hipTextureObject from it + the Sampler's modes, and the
// kernel samples it via __ockl_image_sample_2D (linked from ROCm's device lib)
// → a hardware image_sample. A 2×2 image {0,1,2,3} sampled at the four texel
// centers returns the exact texels; the dead-center (0.5,0.5) returns the
// 4-texel average 1.5. Epsilon compare guards GPU interpolation-weight precision.
TEST(XpuHipDispatchDeviceTests, textureSampleRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
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
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.TexSample", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: sampled texel != expected)";
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
