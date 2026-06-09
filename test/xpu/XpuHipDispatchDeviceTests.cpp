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
#include <string>
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

// Multi-channel texture on the real AMD device (B3): an RGBA Texture2D sampled
// through __ockl_image_sample_2D returns all four channels (sample() ->
// Vector<float32,4>). A 2x2 RGBA image (per texel R,G,B,A = .2t, +.05, +.1, +.15,
// all in [0,1] so the same source is UNORM-safe) is sampled at the four texel
// centers; every channel of every texel is checked within the 0.02 tol. Exercises
// the format-routed HIP channel descriptor (4-channel float / unorm8) + the AMD
// vec4 sampler — the gfx1151 twin of the Vulkan RGBA tests.
const char* kHipRgbaSampleSrc(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexRgbaHip {\n"
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
        "            float32 r = (float32)(t) * 0.2f;\n"
        "            pixels[t*4 + 0] = r;\n"
        "            pixels[t*4 + 1] = r + 0.05f;\n"
        "            pixels[t*4 + 2] = r + 0.1f;\n"
        "            pixels[t*4 + 3] = r + 0.15f;\n"
        "        }\n"
        "        Texture2D tex = heap Texture2D(w, h, ") + fmt + ");\n"
        "        tex.upload(pixels);\n"
        "        Sampler samp = heap Sampler(1, 0);\n"   // linear, clamp
        "        uint32 n = 4;\n"
        "        uint32 m = n * 4;\n"
        "        float32[] hus = heap float32[n];\n"
        "        float32[] hvs = heap float32[n];\n"
        "        hus[0] = 0.25f; hvs[0] = 0.25f;\n"        // texel (0,0)
        "        hus[1] = 0.75f; hvs[1] = 0.25f;\n"        // texel (1,0)
        "        hus[2] = 0.25f; hvs[2] = 0.75f;\n"        // texel (0,1)
        "        hus[3] = 0.75f; hvs[3] = 0.75f;\n"        // texel (1,1)
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

// Single-channel float-storage format on the real AMD device, parametrized by
// ordinal (for R16F). A 2x2 image {0,1,2,3} (all binary16-exact) sampled at the
// four texel centers returns the exact texels in .x; the dead-center returns 1.5.
const char* kHipR1SampleSrc(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexR1Hip {\n"
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

// texelFetch on the real AMD device: `tex.fetch(x, y)` reads the exact integer
// texel of the HIP texture object with NO sampler, lowering to
// __ockl_image_load_2D -> a gfx1151 image_load. One thread per texel decodes
// its (x, y); all four RGBA32F channels read back exactly (unfiltered).
const char* kHipRgbaFetchSrc(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexFetchHip {\n"
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

// B3 Step 2b: integer texelFetch on the AMD device. A Texture2D<int32|uint32>
// (RGBA32I/UI) — __ockl_image_load_2D returns the raw image_load v4f32 whose bits
// ARE the stored integers (no convert on a SINT/UINT image SRD); the lowerer
// bitcasts to <4 x i32>. All four channels read back as exact integers.
const char* kHipRgbaIntFetchSrc(const char* elem, const char* fmt) {
    static std::string s;
    std::string e(elem);
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexFetchIntHip {\n"
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

// B3 texture dims: Texture3D on the real AMD device. A 2x2x2 R32F volume
// (hipMalloc3DArray + hipMemcpy3D), fetched via __ockl_image_load_3D voxel-exact
// and trilinearly sampled via __ockl_image_sample_3D.
const char* kHipTex3dFetchSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture3D;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex3dFetchHip {\n"
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

// Integer Texture3D fetch on AMD — RGBA32I 3-D hipArray; __ockl_image_load_3D's
// raw v4f32 bitcast to <4 x i32>, voxel-exact.
const char* kHipTex3dIntFetchSrc(const char* elem, const char* fmt) {
    static std::string s;
    std::string e(elem);
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture3D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex3dIntFetchHip {\n"
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

const char* kHipTex3dSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture3D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex3dSampHip {\n"
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
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 d = 2; uint32 n = 8;\n"
        "        float32[] voxels = heap float32[8];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { voxels[i] = (float32)(i); }\n"
        "        Texture3D vol = heap Texture3D(w, h, d);\n"
        "        vol.upload(voxels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"   // nearest, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(vol, sn, out, w, h, d, n);\n"
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

// B3 texture dims: Texture1D on the real AMD device. A width-4 R32F row (a 1-D
// hipArray) fetched via __ockl_image_load_1D texel-exact (scalar coord).
const char* kHipTex1dFetchSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture1D;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex1dFetchHip {\n"
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

// Texture1D linear sample on the real AMD device — nearest at texel centers
// (exact) via __ockl_image_sample_1D (scalar coord) on a 1-D hipArray texobj.
const char* kHipTex1dSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture1D;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex1dSampHip {\n"
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
        "    public static int32 run() {\n"
        "        uint32 w = 4; uint32 n = 4;\n"
        "        float32[] texels = heap float32[4];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture1D row = heap Texture1D(w);\n"
        "        row.upload(texels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"   // nearest, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(row, sn, out, w, n);\n"
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

// B3 texture dims: Texture2DArray on the real AMD device. A 2x2x3 R32F layered
// hipArray fetched via __ockl_image_load_2Da texel-exact by (x, y, layer).
const char* kHipTex2daFetchSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2DArray;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex2daFetchHip {\n"
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

// Texture2DArray sample on the real AMD device — nearest at texel centers per
// layer (exact) via __ockl_image_sample_2Da on a layered hipArray texobj.
const char* kHipTex2daSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2DArray;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Tex2daSampHip {\n"
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
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 layers = 3; uint32 n = 12;\n"
        "        float32[] texels = heap float32[12];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture2DArray arr = heap Texture2DArray(w, h, layers);\n"
        "        arr.upload(texels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"   // nearest, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(arr, sn, out, w, h, n);\n"
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

// B3 texture dims: TextureCube sample on the real AMD device. A 2x2x6 R32F
// cubemap hipArray; each face holds a constant = its index; the 6 axis directions
// must select the 6 faces via __ockl_image_sample_CM.
const char* kHipTexCubeSampleSrc() {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.TextureCube;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexCubeSampHip {\n"
        "    @Kernel\n"
        "    public static void samp(TextureCube cube, Sampler sn,\n"
        "                            Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 axis = i / 2;\n"
        "            float32 sgn = 1.0f;\n"
        "            if (i % 2 == 1) { sgn = -1.0f; }\n"
        "            float32 x = 0.0f; float32 y = 0.0f; float32 z = 0.0f;\n"
        "            if (axis == 0) { x = sgn; }\n"
        "            if (axis == 1) { y = sgn; }\n"
        "            if (axis == 2) { z = sgn; }\n"
        "            Vector<float32,4> c = cube.sample(sn, x, y, z);\n"
        "            out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 sz = 2; uint32 n = 6;\n"
        "        uint32 faceTexels = sz * sz;\n"
        "        float32[] faces = heap float32[24];\n"
        "        for (uint32 f = 0; f < 6; f = f + 1) {\n"
        "            for (uint32 k = 0; k < faceTexels; k = k + 1) {\n"
        "                faces[f*faceTexels + k] = (float32)(f);\n"
        "            }\n"
        "        }\n"
        "        TextureCube cube = heap TextureCube(sz);\n"
        "        cube.upload(faces);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(0, n);\n"
        "        out.allocate(); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [64])(cube, sn, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout); out.free();\n"
        // If the cubemap array couldn't be created the kernel didn't launch and
        // out stays at its uploaded -1.0 — on gfx1151 ROCm rejects cubemap arrays
        // (hipMalloc3DArray → hipErrorInvalidValue). The C++ side skips on 555.
        "        if (hout[0] == -1.0f) { return (int32)(555); }\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n");
    return s.c_str();
}

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

// B3 mipmaps on the real AMD device (gfx1151): a 2-level R32F Texture2D backed by a
// hipMipmappedArray (hipMallocMipmappedArray + per-level hipGetMipmappedArrayLevel
// copy), read with explicit LOD via the ockl _lod ockl variants. L0 fetch=3, L1
// fetch=99, L1 sampleLod=99 — the AMD twin of mipmapFetchAndSampleLodOnDevice. The
// texobj's maxMipmapLevelClamp=levels-1 lets the explicit LOD reach level 1 (the
// AMD analog of the Vulkan sampler maxLod fix).
TEST(XpuHipDispatchDeviceTests, mipmapFetchAndSampleLodRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class MipHip {\n"
        "    @Kernel\n"
        "    public static void mip(Texture2D tex, Sampler sl, Buffer<float32> out) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < 1) {\n"
        "            Vector<float32,4> a = tex.fetchLod(1, 1, 0);\n"
        "            Vector<float32,4> b = tex.fetchLod(0, 0, 1);\n"
        "            Vector<float32,4> c = tex.sampleLod(sl, 0.5f, 0.5f, 1.0f);\n"
        "            out[0] = a.x; out[1] = b.x; out[2] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2;\n"
        "        Texture2D tex = heap Texture2D(w, h, TextureFormat.R32F, 2);\n"
        "        float32[] l0 = heap float32[4];\n"
        "        l0[0] = 0.0f; l0[1] = 1.0f; l0[2] = 2.0f; l0[3] = 3.0f;\n"
        "        float32[] l1 = heap float32[1]; l1[0] = 99.0f;\n"
        "        tex.uploadLevel(0, l0);\n"
        "        tex.uploadLevel(1, l1);\n"
        "        Sampler sl = heap Sampler(1, 0);\n"
        "        float32[] hout = heap float32[3];\n"
        "        for (uint32 i = 0; i < 3; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(3);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        mip.launch(s, grid: [1], block: [1])(tex, sl, out);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        // 555 = the mip texture never allocated (handle 0) so the kernel didn't
        // launch and out stays at its uploaded -1.0 — on gfx1151 ROCm returns
        // hipErrorNotSupported for mipmapped arrays. The C++ side skips on 555.
        "        if (hout[0] == -1.0f) { return (int32)(555); }\n"
        "        if (hout[0] != 3.0f) { return (int32)(100); }\n"
        "        if (hout[1] != 99.0f) { return (int32)(200); }\n"
        "        float32 dc = hout[2] - 99.0f;\n"
        "        if (dc < -0.02f || dc > 0.02f) { return (int32)(300); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.MipHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    // AMD mipmapped arrays are unsupported on gfx1151 / ROCm 7.2 (hipMallocMipmapped
    // Array → hipErrorNotSupported). The runtime + ockl _lod seam are correct and
    // will work on ROCm/hardware that supports mipmapped arrays; skip where it's
    // not implemented rather than fail.
    if (r == 555) {
        GTEST_SKIP() << "AMD mipmapped arrays unsupported on this device "
                        "(hipMallocMipmappedArray → hipErrorNotSupported)";
    }
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100: L0 fetch; 200: L1 fetch; 300: L1 sampleLod)";
}

// B3 multi-channel on the real AMD device: an RGBA32F Texture2D sampled on
// gfx1151 returns all four channels (sample() -> Vector<float32,4>). Verifies the
// 4-channel-float HIP channel descriptor + the AMD vec4 sampler path.
TEST(XpuHipDispatchDeviceTests, textureSampleRgba32fRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipRgbaSampleSrc("TextureFormat.RGBA32F"),
                                  "test.TexRgbaHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA32F sample mismatch at i)";
}

// B3 8-bit normalized RGBA on the real AMD device: bytes 0..255 stored, read back
// as float [0,1]. Exercises the float->unorm8 quantize-on-upload + the 4-channel
// unorm8 HIP channel descriptor (readMode NormalizedFloat), within the 0.02 tol.
TEST(XpuHipDispatchDeviceTests, textureSampleRgba8UnormRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipRgbaSampleSrc("TextureFormat.RGBA8_UNORM"),
                                  "test.TexRgbaHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA8_UNORM sample mismatch at i)";
}

// B3 half-float single channel on the real AMD device (R16F): float uploaded,
// binary16 stored in the 16-bit-float hipArray, read back as float. Texel values
// {0,1,2,3} are f16-exact.
TEST(XpuHipDispatchDeviceTests, textureSampleR16fRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipR1SampleSrc("TextureFormat.R16F"),
                                  "test.TexR1Hip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: R16F sample mismatch at i)";
}

// B3 half-float RGBA on the real AMD device (RGBA16F): four-channel cheap HDR via
// the 4-channel 16-bit-float HIP channel descriptor. Within the 0.02 tol.
TEST(XpuHipDispatchDeviceTests, textureSampleRgba16fRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipRgbaSampleSrc("TextureFormat.RGBA16F"),
                                  "test.TexRgbaHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA16F sample mismatch at i)";
}

// B3 texelFetch on the real AMD device: an RGBA32F Texture2D fetched by exact
// integer coord on gfx1151 (__ockl_image_load_2D -> image_load), all four
// channels bit-exact — the device twin of the CPU/Vulkan fetch tests, and proof
// the ockl image-load link gate fires for fetch as it does for sample.
TEST(XpuHipDispatchDeviceTests, textureFetchRgba32fRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipRgbaFetchSrc("TextureFormat.RGBA32F"),
                                  "test.TexFetchHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA32F fetch mismatch at i)";
}

// B3 Step 2b: integer texelFetch on the real AMD device (gfx1151) — an RGBA32I
// Texture2D<int32>; __ockl_image_load_2D's raw v4f32 result is bitcast to <4 x i32>
// and every channel reads back as the exact stored signed integer. Proof the raw
// image_load + bitcast recovers integers bit-for-bit on hardware.
TEST(XpuHipDispatchDeviceTests, textureFetchRgba32iRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipRgbaIntFetchSrc("int32", "TextureFormat.RGBA32I"),
                                  "test.TexFetchIntHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (RGBA32I device fetch mismatch)";
}

// B3 texture dims: Texture3D fetch on the real AMD device (gfx1151) — a 2x2x2
// hipMalloc3DArray volume, __ockl_image_load_3D voxel-exact.
TEST(XpuHipDispatchDeviceTests, texture3dFetchRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex3dFetchSrc(), "test.Tex3dFetchHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 3D voxel fetch mismatch)";
}

// Texture3D trilinear sample on the real AMD device — nearest at voxel centers
// (exact) via __ockl_image_sample_3D on a 3-D hipArray texture object.
TEST(XpuHipDispatchDeviceTests, texture3dSampleRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex3dSampleSrc(), "test.Tex3dSampHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 3D nearest sample mismatch)";
}

// B3 texture dims: Texture2DArray fetch on the real AMD device — a layered
// hipArray, __ockl_image_load_2Da texel-exact by (x, y, layer).
TEST(XpuHipDispatchDeviceTests, texture2dArrayFetchRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex2daFetchSrc(), "test.Tex2daFetchHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: layered texel fetch mismatch)";
}

// Texture2DArray sample on the real AMD device — nearest per layer (exact) via
// __ockl_image_sample_2Da on a layered hipArray texture object.
TEST(XpuHipDispatchDeviceTests, texture2dArraySampleRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex2daSampleSrc(), "test.Tex2daSampHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: layered nearest sample mismatch)";
}

// B3 texture dims: TextureCube sample on the real AMD device — a cubemap hipArray,
// __ockl_image_sample_CM by direction selects the 6 faces.
TEST(XpuHipDispatchDeviceTests, textureCubeSampleRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTexCubeSampleSrc(), "test.TexCubeSampHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    // AMD cubemap arrays are unsupported on gfx1151 / ROCm 7.2.2 (hipMalloc3DArray
    // with hipArrayCubemap → hipErrorInvalidValue). The runtime + ockl
    // __ockl_image_sample_CM seam are correct and will work on ROCm/hardware that
    // supports cubemap arrays; skip where it's not implemented rather than fail.
    if (r == 555) {
        GTEST_SKIP() << "AMD cubemap arrays unsupported on this device "
                        "(hipMalloc3DArray[cubemap] → hipErrorInvalidValue)";
    }
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: cube face selection mismatch at dir i)";
}

// B3 texture dims: Texture1D fetch on the real AMD device — a 1-D hipArray,
// __ockl_image_load_1D (scalar coord) texel-exact.
TEST(XpuHipDispatchDeviceTests, texture1dFetchRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex1dFetchSrc(), "test.Tex1dFetchHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 1D texel fetch mismatch)";
}

// Texture1D linear sample on the real AMD device — nearest at texel centers
// (exact) via __ockl_image_sample_1D on a 1-D hipArray texture object.
TEST(XpuHipDispatchDeviceTests, texture1dSampleRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex1dSampleSrc(), "test.Tex1dSampHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: 1D nearest sample mismatch)";
}

// Integer Texture3D fetch on the real AMD device (gfx1151) — RGBA32I 3-D hipArray,
// __ockl_image_load_3D + bitcast, voxel-exact across all channels.
TEST(XpuHipDispatchDeviceTests, texture3dFetchRgba32iRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(kHipTex3dIntFetchSrc("int32", "TextureFormat.RGBA32I"),
                                  "test.Tex3dIntFetchHip", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (3D RGBA32I device fetch mismatch)";
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

// Buffer.slice (Stage B4) on HIP/AMD — device-verify the pointer-fold path. On
// HIP the device handle is a pointer, so slice() folds the byte offset into it
// (handle + offset*elementBytes) at __cajeta_xpu_buffer_slice; the launch-arg
// and upload/download paths stay offset-unaware. Code-identical to the verified
// CPU pointer-fold (XpuCpuDispatchTests.bufferSliceKernelOnCpu), confirmed here
// on real gfx1151. A 128-element parent is filled -1; the tail half [64,128) is
// sliced and a kernel writes globalIdX (0..63) through the view → lands at
// parent[64+i], head untouched, the non-owning view double-free-safe.
TEST(XpuHipDispatchDeviceTests, bufferSliceKernelRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Slice {\n"
        "    @Kernel\n"
        "    public static void fill(Buffer<int32> b, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { b[i] = (int32) i; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 128;\n"
        "        uint32 half = 64;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = -1; }\n"
        "        Buffer<int32> all = heap Buffer<int32>(n);\n"
        "        all.upload(h);\n"
        "        Buffer<int32> tail = all.slice(half, half);\n"
        "        Stream s = Stream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(tail, half);\n"
        "        s.sync();\n"
        "        all.download(h);\n"
        "        for (uint32 i = 0; i < half; i = i + 1) {\n"
        "            if (h[i] != -1) { return (int32)(100 + i); }\n"
        "        }\n"
        "        for (uint32 i = 0; i < half; i = i + 1) {\n"
        "            if (h[half + i] != (int32) i) { return (int32)(200 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.Slice", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (1xx: head overwritten — bad offset; "
                         "2xx: tail wrong — view base off)";
}

// Buffer MemoryKind (Stage B4) on HIP/AMD — genuine zero-copy UNIFIED (managed)
// memory on gfx1151. hipMallocManaged gives one pointer the host AND device both
// address; on this APU (Strix Halo, shared physical RAM) there is no transfer at
// all. The host writes the buffer directly with hostStore (a memcpy into managed
// memory, NO hipMemcpyHtoD), a kernel increments every element reading that same
// memory, and the host reads results with hostLoad (NO hipMemcpyDtoH). That the
// kernel saw the host's writes and the host saw the kernel's writes — with no
// device-transfer call anywhere — is the zero-copy proof. `kind` ordinal 2.
static const char* hipMemKindSource(const char* kindExpr) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.MemoryKind;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class MemKind {\n"
        "    @Kernel\n"
        "    public static void inc(Buffer<int32> b, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        Buffer<int32> u = heap Buffer<int32>(0, n);\n"
        "        u.allocate(") + kindExpr + ");\n"
        "        u.hostStore(h);\n"             // zero-copy host write (no upload)
        "        Stream s = Stream.current();\n"
        "        inc.launch(s, grid: [1], block: [64])(u, n);\n"
        "        s.sync();\n"
        "        int32[] out = heap int32[n];\n"
        "        u.hostLoad(out);\n"            // zero-copy host read (no download)
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (out[i] != (int32)(i + 1)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

TEST(XpuHipDispatchDeviceTests, memoryKindUnifiedZeroCopyRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(hipMemKindSource("MemoryKind.Unified"),
                                  "test.MemKind", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — managed host<->device "
                         "sharing broke)";
}

// PINNED (page-locked, device-accessible host) memory on HIP/AMD: hipHostMalloc
// gives host memory the kernel reads directly through the unified address space.
// Same zero-copy hostStore→inc→hostLoad shape; also exercises the kind-aware
// free routing to hipHostFree (a Device/Unified free would be hipFree). kind 1.
TEST(XpuHipDispatchDeviceTests, memoryKindPinnedZeroCopyRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(hipMemKindSource("MemoryKind.Pinned"),
                                  "test.MemKind", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — pinned host<->device "
                         "sharing broke)";
}

// Async copies / transfer queues (Stage B4) on HIP/AMD — the full async pipeline
// on a REAL stream (hipStreamCreate). Three operations are queued on one stream:
// an async H2D upload, a kernel launch (now stream-ordered — the launch site
// threads the stream's handle through __cajeta_xpu_launch into
// hipModuleLaunchKernel), and an async D2H download. A SINGLE stream.sync()
// drains the whole pipeline. Correctness is the ordering proof: if the launch
// ran before the upload finished, or the download before the launch, out[i]
// would not be i+1. (gfx1151; uses a real per-stream queue, not the default.)
TEST(XpuHipDispatchDeviceTests, asyncCopyPipelineRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Pipe {\n"
        "    @Kernel\n"
        "    public static void inc(Buffer<int32> b, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        Buffer<int32> b = heap Buffer<int32>(n);\n"
        "        Stream s = Stream.create();\n"
        "        b.uploadAsync(h, s);\n"                     // async H2D on s
        "        inc.launch(s, grid: [4], block: [64])(b, n);\n"  // launch on s
        "        int32[] out = heap int32[n];\n"
        "        b.downloadAsync(out, s);\n"                 // async D2H on s
        "        s.sync();\n"                                // one sync drains all 3
        "        s.destroy();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (out[i] != (int32)(i + 1)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.Pipe", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — async pipeline mis-ordered "
                         "on the stream)";
}

// Event / Fence (Stage B4 async follow-on) — real hipEvent cross-stream ordering
// + host fence on the device. Two REAL hipStreams increment the same buffer; an
// Event records s1's tail and s2 hipStreamWaitEvent()s it, so s2's +1 runs only
// after s1's +1 fully completes — no concurrent read-modify-write race on a
// shared element (which would lose an update → i+1). out[i] == i+2 is therefore
// the ordering proof. A Fence then records at s2's tail and the host
// hipEventSynchronize/Query()s it. (Without the waitFor this races; the
// correctness of i+2 IS the cross-stream-sync correctness.)
TEST(XpuHipDispatchDeviceTests, eventFenceSyncRoutesToHipOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Event;\n"
        "import cajeta.xpu.core.Fence;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class Sync {\n"
        "    @Kernel\n"
        "    public static void inc(Buffer<int32> b, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        Buffer<int32> b = heap Buffer<int32>(n);\n"
        "        Stream s1 = Stream.create();\n"
        "        Stream s2 = Stream.create();\n"
        "        Event e = Event.create();\n"
        "        b.uploadAsync(h, s1);\n"
        "        inc.launch(s1, grid: [4], block: [64])(b, n);\n"   // s1: +1
        "        e.recordOn(s1);\n"                                 // mark s1 tail
        "        s2.waitFor(e);\n"                                  // s2 waits s1
        "        inc.launch(s2, grid: [4], block: [64])(b, n);\n"   // s2: +1 after
        "        int32[] out = heap int32[n];\n"
        "        b.downloadAsync(out, s2);\n"
        "        Fence f = Fence.create();\n"
        "        f.signal(s2);\n"
        "        f.waitHost();\n"
        "        boolean done = f.query();\n"
        "        s1.sync();\n"
        "        s2.sync();\n"
        "        e.destroy();\n"
        "        f.destroy();\n"
        "        s1.destroy();\n"
        "        s2.destroy();\n"
        "        if (!done) { return 1; }\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (out[i] != (int32)(i + 2)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(src, "test.Sync", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (1: fence not signaled; 100+i: out[i] != i+2 — "
                         "cross-stream event ordering broke on the device)";
}
