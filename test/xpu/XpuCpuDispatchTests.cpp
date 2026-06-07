//
// CajetaXPU CPU backend (Increment 4) — the runtime backend dispatcher, end to
// end and GPU-FREE.
//
// The milestone of the whole CPU thread: a complete Cajeta program — @Kernel +
// the host orchestration (allocate / upload / kernel.launch / sync / download /
// free) — compiled `--xpu-backend=cpu` and run with NO GPU present, through the
// real launch path:
//
//   Buffer.allocate   -> __cajeta_xpu_buffer_alloc    -> malloc (CPU "device")
//   buf.upload        -> __cajeta_xpu_buffer_upload   -> memcpy
//   kernel.launch(..) -> __cajeta_xpu_launch          -> CPU launcher-thunk grid loop
//   buf.download      -> __cajeta_xpu_buffer_download -> memcpy
//
// The active backend is chosen once at first device touch among the bundled set
// (the manifest of __cajeta_xpu_register_backend ctors) and the available set
// (the runtime probes). With only `cpu` bundled, that's the CPU — the guaranteed
// terminal of the priority chain. A bundle whose backends are all unavailable
// degrades gracefully (precise diagnostic, no crash) rather than launching.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdlib>
#include <string>
#if defined(_WIN32)
// POSIX setenv/unsetenv are absent on mingw; shim onto _putenv_s.
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

using cajeta_test::CajetaJit;

namespace {

// A self-contained SAXPY: kernel + Cajeta host driver. 1024 elements, each
// 2*1 + 2 = 4  ->  sum = 4096 once the launch runs; left at 2*1024 = 2048 if no
// backend is available (nothing launched).
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

// A grid large enough (65536 work-items > the runtime's parallel threshold) to
// exercise the multi-core fan-out path (Inc 5A). RAII Buffers. y[i] = 2*1 + 2 = 4
// -> sum = 4 * 65536 = 262144.
const char* kSaxpyLargeSource =
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
    "        uint32 n = 65536;\n"
    "        float32[] hx = heap float32[n];\n"
    "        float32[] hy = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            hx[i] = 1.0f;\n"
    "            hy[i] = 2.0f;\n"
    "        }\n"
    "        Buffer<float32> x = heap Buffer<float32>(n);\n"
    "        Buffer<float32> y = heap Buffer<float32>(n);\n"
    "        x.upload(hx);\n"
    "        y.upload(hy);\n"
    "        Stream s = Stream.current();\n"
    "        saxpy.launch(s, grid: [(n + 63) / 64], block: [64])(y, x, 2.0f, n);\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        float32 sum = 0.0f;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            sum = sum + hy[i];\n"
    "        }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// 2-D launch grid (2D/3D launch, stage 1): a 4×3 grid of 1-D blocks. Each block
// writes its own (bx,by) index, so the result proves ctaid.x AND ctaid.y reach
// the kernel through the runtime's 3-D grid decode. out[by*4+bx] = by*10+bx.
const char* kGrid2dSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Workgroup;\n"
    "public class Grid2d {\n"
    "    @Kernel\n"
    "    public static void grid2d(Buffer<int32> out) {\n"
    "        uint32 bx = Workgroup.x();\n"
    "        uint32 by = Workgroup.y();\n"
    "        out[by * 4 + bx] = (int32)(by * 10 + bx);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 12;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        grid2d.launch(s, grid: [4, 3], block: [1])(out);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 by = 0; by < 3; by = by + 1) {\n"
    "            for (uint32 bx = 0; bx < 4; bx = bx + 1) {\n"
    "                if (hout[by * 4 + bx] != (int32)(by * 10 + bx)) {\n"
    "                    return (int32)(100 + by * 4 + bx);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// 2-D launch BLOCK (2D/3D launch, stage 3): a single block of 4×3 work-items.
// Each work-item writes its own (tx,ty), so a correct result proves the CPU
// per-block wrapper runs the full 3-D work-item loop nest (tid.x AND tid.y), not
// just tid.x. out[ty*4+tx] = ty*100+tx.
const char* kBlock2dSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class Block2d {\n"
    "    @Kernel\n"
    "    public static void block2d(Buffer<int32> out, uint32 w) {\n"
    "        uint32 tx = Thread.x();\n"
    "        uint32 ty = Thread.y();\n"
    "        out[ty * w + tx] = (int32)(ty * 100 + tx);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 w = 4;\n"
    "        uint32 h = 3;\n"
    "        uint32 n = w * h;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        block2d.launch(s, grid: [1], block: [4, 3])(out, w);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 ty = 0; ty < h; ty = ty + 1) {\n"
    "            for (uint32 tx = 0; tx < w; tx = tx + 1) {\n"
    "                if (hout[ty * w + tx] != (int32)(ty * 100 + tx)) {\n"
    "                    return (int32)(100 + ty * w + tx);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Item 6 Stage 2 — a grid-stride for-each kernel runs on CPU. The crux: count
// (1024) is far larger than the grid's total work-items gridSize = nctaid·ntid =
// 2·64 = 128, so every work-item must process 8 elements via the stride. That is
// only correct if gridSize() returns nctaid·ntid (the grid block-count threaded
// through the coord ABI), not just ntid — a stride of 64 would skip/overlap and
// leave most of `out` unwritten. in[i]=i, out[i]=v=in[i]; verifying out[i]==i for
// ALL i proves full coverage with the right per-element index.
const char* kGridStrideForEachSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "public class GridStride {\n"
    "    @Kernel\n"
    "    public static void copy(Buffer<float32> out, Buffer<float32> in,\n"
    "                            uint32 n) {\n"
    "        for (uint32 i, float32 v : in.range(n)) {\n"
    "            out[i] = v;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 1024;\n"
    "        float32[] hin = heap float32[n];\n"
    "        float32[] hout = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            hin[i] = (float32)i;\n"
    "            hout[i] = -1.0f;\n"
    "        }\n"
    "        Buffer<float32> in = heap Buffer<float32>(n);\n"
    "        Buffer<float32> out = heap Buffer<float32>(n);\n"
    "        in.upload(hin);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        copy.launch(s, grid: [2], block: [64])(out, in, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != (float32)i) { return (int32)i; }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Item 2 — a kernel calling a user-defined @Device helper (scalar arg + scalar
// return). The helper is lowered to a device function and inlined; out[i] = i*i.
const char* kDeviceHelperSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class DevH {\n"
    "    @Device\n"
    "    public static int32 square(int32 x) { return x * x; }\n"
    "    @Kernel\n"
    "    public static void k(Buffer<int32> out) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        out[i] = square((int32) i);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(out);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != (int32)(i * i)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Item 2 — a @Device helper that calls another @Device helper (exercises the
// shared function cache + nested lowering). twice(x) = inc(inc(x)) = x+2.
const char* kDeviceChainSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class DevChain {\n"
    "    @Device\n"
    "    public static int32 inc(int32 x) { return x + 1; }\n"
    "    @Device\n"
    "    public static int32 twice(int32 x) { return inc(inc(x)); }\n"
    "    @Kernel\n"
    "    public static void k(Buffer<int32> out) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        out[i] = twice((int32) i);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(out);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != (int32)(i + 2)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Item 2 follow-up — a @Device helper taking Buffer<T> params. `scale` reads one
// buffer and writes another (two buffer params, a read AND a write through the
// helper), proving buffer bases flow by value into the inlined helper. in[i]=i,
// out[i] = in[i]*3 = i*3.
const char* kDeviceBufferParamSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class DevBuf {\n"
    "    @Device\n"
    "    public static void scale(Buffer<int32> out, Buffer<int32> in,\n"
    "                             uint32 i) {\n"
    "        out[i] = in[i] * 3;\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void k(Buffer<int32> out, Buffer<int32> in) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        scale(out, in, i);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        int32[] hin = heap int32[n];\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            hin[i] = (int32)i;\n"
    "            hout[i] = -1;\n"
    "        }\n"
    "        Buffer<int32> in = heap Buffer<int32>(n);\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        in.upload(hin);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(out, in);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != (int32)(i * 3)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// A POD struct passed BY VALUE as a kernel arg (Item 7). `Params` is a plain
// class (two int32 fields, no marker interface); the kernel reads p.mul / p.add
// to compute out[i] = i*mul + add. Marshalled field-by-field through the
// kernelParams ABI; the CPU thunk loads the struct aggregate from argv[i].
const char* kPodStructArgSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Stream;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class Params {\n"
    "    int32 mul;\n"
    "    int32 add;\n"
    "    public Params(int32 mul, int32 add) { this.mul = mul; this.add = add; }\n"
    "}\n"
    "public class PodArg {\n"
    "    @Kernel\n"
    "    public static void k(Buffer<int32> out, Params p) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        out[i] = (int32)i * p.mul + p.add;\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        out.upload(hout);\n"
    "        Params p = heap Params(3, 7);\n"
    "        Stream s = Stream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(out, p);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != (int32)(i * 3 + 7)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Item 8 — a Texture2D sampled through a Sampler with bilinear filtering, end to
// end on CPU. A 2×2 float image {0,1,2,3} (row-major) is uploaded; each work-item
// samples at a per-lane (u,v) from coordinate buffers and writes the filtered
// texel. Texel-center coords return the exact texel (0,1,2,3); the dead-center
// (0.5,0.5) returns the 4-texel average 1.5. Proves the texture/sampler kernel
// args marshal (texture like a buffer handle, sampler by value), the `.sample()`
// lowering reaches __cajeta_xpu_cpu_tex_sample, and the C bilinear math is right.
const char* kTextureSampleSource =
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
    "        Buffer<float32> us = heap Buffer<float32>(n);\n"
    "        Buffer<float32> vs = heap Buffer<float32>(n);\n"
    "        Buffer<float32> out = heap Buffer<float32>(n);\n"
    "        us.upload(hus);\n"
    "        vs.upload(hvs);\n"
    "        out.upload(hout);\n"
    "        Stream s = Stream.current();\n"
    "        sample.launch(s, grid: [1], block: [n])(tex, samp, us, vs, out, n);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (hout[i] != hexp[i]) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

// Multi-channel texture on CPU (B3): an RGBA Texture2D sampled through the CPU
// `caj_v4f` sampler returns all four channels (sample() -> Vector<float32,4>).
// A 2x2 RGBA image (per texel R,G,B,A = .2t, +.05, +.1, +.15, all in [0,1] so
// the same source is UNORM-safe) is sampled at the four texel centers; every
// channel of every texel is checked within the 0.02 tol (UNORM quantization).
// Exercises the format-routed CPU upload (incl. the float->unorm8 quantize for
// RGBA8_UNORM) and the vec4 CPU sampler — the CPU twin of the Vulkan RGBA tests.
static const char* kRgbaSampleSrcCpu(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexRgbaCpu {\n"
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
        "        Buffer<float32> us = heap Buffer<float32>(n);\n"
        "        Buffer<float32> vs = heap Buffer<float32>(n);\n"
        "        Buffer<float32> out = heap Buffer<float32>(m);\n"
        "        us.upload(hus); vs.upload(hvs); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        sample.launch(s, grid: [1], block: [n])(tex, samp, us, vs, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < m; i = i + 1) {\n"
        "            float32 d = hout[i] - hexp[i];\n"
        "            if (d < -0.02f || d > 0.02f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// Single-channel float-storage format on CPU, parametrized by ordinal (for R16F).
// A 2x2 image {0,1,2,3} (all binary16-exact) sampled at the four texel centers
// returns the exact texels in .x; the dead-center returns the average 1.5.
static const char* kR1SampleSrcCpu(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Texture2D;\n"
        "import cajeta.xpu.core.TextureFormat;\n"
        "import cajeta.xpu.core.Sampler;\n"
        "import cajeta.xpu.core.Stream;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class TexR1Cpu {\n"
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
        "        Buffer<float32> us = heap Buffer<float32>(n);\n"
        "        Buffer<float32> vs = heap Buffer<float32>(n);\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        us.upload(hus); vs.upload(hvs); out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        sample.launch(s, grid: [1], block: [n])(tex, samp, us, vs, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != hexp[i]) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

} // namespace

// A large grid drives the runtime's multi-core fan-out (Inc 5A); the result must
// match the serial computation exactly — every work-item ran once, none twice.
TEST(XpuCpuDispatchTests, saxpyLargeGridParallelOnCpu) {
    auto jit = CajetaJit::compile(kSaxpyLargeSource, "test.Saxpy", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 262144.0f);   // 4 * 65536
}

// The headline: a host-source @Kernel program compiled --xpu-backend=cpu runs on
// the CPU with no GPU, through the real dispatcher + launcher-thunk grid loop.
TEST(XpuCpuDispatchTests, saxpyHostSourceRunsOnCpu) {
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 4096.0f);
}

// 2D/3D launch (stage 1): a multi-dim GRID of 1-D blocks runs on CPU — ctaid.x/y
// both reach the kernel via the runtime's linearized-block decode.
TEST(XpuCpuDispatchTests, multiDimGridOnCpu) {
    auto jit = CajetaJit::compile(kGrid2dSource, "test.Grid2d", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 777);
}

// 2D/3D launch (stage 3): a multi-dim BLOCK runs on CPU via the wrapper's 3-D
// work-item loop nest — tid.x and tid.y both drive the kernel.
TEST(XpuCpuDispatchTests, multiDimBlockOnCpu) {
    auto jit = CajetaJit::compile(kBlock2dSource, "test.Block2d", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 777);
}

// Item 6 Stage 2: a grid-stride for-each kernel runs on CPU. count > gridSize, so
// the stride (gridSize = nctaid·ntid, threaded through the CPU coord ABI) carries
// each work-item across multiple elements — full coverage proves it's correct.
TEST(XpuCpuDispatchTests, gridStrideForEachOnCpu) {
    auto jit = CajetaJit::compile(kGridStrideForEachSource, "test.GridStride",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail index " << r << " (out[i] != i: stride miscovered)";
}

// Item 2: a kernel calling a user-defined @Device helper runs on CPU.
TEST(XpuCpuDispatchTests, deviceHelperCallOnCpu) {
    auto jit = CajetaJit::compile(kDeviceHelperSource, "test.DevH", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != i*i)";
}

// Item 2: a @Device helper calling another @Device helper runs on CPU.
TEST(XpuCpuDispatchTests, deviceHelperChainOnCpu) {
    auto jit = CajetaJit::compile(kDeviceChainSource, "test.DevChain", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != i+2)";
}

// Item 2 follow-up: a @Device helper taking Buffer<T> params runs on CPU — buffer
// bases pass by value into the helper, which reads `in` and writes `out`.
TEST(XpuCpuDispatchTests, deviceHelperBufferParamOnCpu) {
    auto jit = CajetaJit::compile(kDeviceBufferParamSource, "test.DevBuf",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != i*3)";
}

// Item 7: a POD struct passed by value as a kernel arg runs on CPU. The struct
// is marshalled field-by-field; the kernel reads p.mul/p.add to compute
// out[i] = i*3 + 7 for every work-item.
TEST(XpuCpuDispatchTests, podStructArgOnCpu) {
    auto jit = CajetaJit::compile(kPodStructArgSource, "test.PodArg",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != i*3+7)";
}

// Item 8: a Texture2D sampled through a Sampler (bilinear) runs on CPU — the
// texture/sampler kernel args marshal, `.sample()` lowers to the C sampler, and
// the bilinear/texel-center math returns the expected filtered texels.
TEST(XpuCpuDispatchTests, textureSampleOnCpu) {
    auto jit = CajetaJit::compile(kTextureSampleSource, "test.TexSample",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: sampled texel != expected)";
}

// B3 multi-channel on CPU: an RGBA32F Texture2D sampled through the CPU vec4
// sampler returns all four channels (sample() -> Vector<float32,4>). Verifies the
// format-routed CPU alloc/upload + the `caj_v4f` sampler off the GPU.
TEST(XpuCpuDispatchTests, textureSampleRgba32fOnCpu) {
    auto jit = CajetaJit::compile(kRgbaSampleSrcCpu("TextureFormat.RGBA32F"),
                                  "test.TexRgbaCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA32F sample mismatch at i)";
}

// B3 8-bit normalized RGBA on CPU: bytes 0..255 stored, read back as float [0,1].
// Exercises the float->unorm8 quantize-on-upload path + the vec4 CPU sampler,
// within unorm8 quantization of the 0.02 tol.
TEST(XpuCpuDispatchTests, textureSampleRgba8UnormOnCpu) {
    auto jit = CajetaJit::compile(kRgbaSampleSrcCpu("TextureFormat.RGBA8_UNORM"),
                                  "test.TexRgbaCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA8_UNORM sample mismatch at i)";
}

// B3 half-float single channel on CPU (R16F): float uploaded, round-tripped
// through binary16 (the CPU emulation of the device's f16 storage), read back as
// float. Texel values {0,1,2,3} are f16-exact, so the same bit-exact expecteds.
TEST(XpuCpuDispatchTests, textureSampleR16fOnCpu) {
    auto jit = CajetaJit::compile(kR1SampleSrcCpu("TextureFormat.R16F"),
                                  "test.TexR1Cpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: R16F sample mismatch at i)";
}

// B3 half-float RGBA on CPU (RGBA16F): four-channel cheap HDR, binary16-emulated.
// Channel values within the 0.02 tol of their f16 round-trip.
TEST(XpuCpuDispatchTests, textureSampleRgba16fOnCpu) {
    auto jit = CajetaJit::compile(kRgbaSampleSrcCpu("TextureFormat.RGBA16F"),
                                  "test.TexRgbaCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA16F sample mismatch at i)";
}

// Explicit-only bundling is a build-time contract (locked decision #3): when the
// env forces a backend that wasn't bundled, no backend is available — the
// dispatcher emits its precise diagnostic and the program does NOT crash; the
// launch simply doesn't run (buffers stay un-launched: sum = 2*1024 = 2048).
TEST(XpuCpuDispatchTests, forcedUnavailableBackendDegradesGracefully) {
    // The backend is selected at first device touch INSIDE run(), so the env
    // must be set across fn(), not just across compile().
    setenv("CAJETA_XPU_BACKEND", "cuda", /*overwrite=*/1);
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", cpuOptions());
    auto fn = jit ? jit->lookup<float (*)()>("run") : nullptr;
    float result = fn ? fn() : 0.0f;
    unsetenv("CAJETA_XPU_BACKEND");

    ASSERT_NE(jit, nullptr);
    ASSERT_NE(fn, nullptr);
    // Only `cpu` is bundled; forcing `cuda` leaves nothing available, so the
    // kernel never launches and hy keeps its initial 2.0 per element.
    EXPECT_FLOAT_EQ(result, 2048.0f);
}

// CAJETA_XPU_BACKEND=cpu forcing the (bundled, always-available) CPU still runs
// — the debugging-superpower path (run a kernel deterministically on the CPU).
TEST(XpuCpuDispatchTests, envForcesCpuAndRuns) {
    setenv("CAJETA_XPU_BACKEND", "cpu", /*overwrite=*/1);
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", cpuOptions());
    auto fn = jit ? jit->lookup<float (*)()>("run") : nullptr;
    float result = fn ? fn() : 0.0f;
    unsetenv("CAJETA_XPU_BACKEND");

    ASSERT_NE(jit, nullptr);
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(result, 4096.0f);
}
