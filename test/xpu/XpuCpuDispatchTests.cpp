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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Workgroup;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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

// Buffer.slice (Stage B4) — a non-owning sub-buffer view passed to a kernel.
// A 128-element parent is filled with -1; the tail half [64,128) is sliced and
// a kernel writes globalIdX (0..63) through the view. Proves (a) the slice base
// is offset-correct — writing view[i] lands at parent[64+i] — and (b) the head
// half is untouched (the offset isn't writing from 0). The view is non-owning,
// so only the parent frees at scope exit (no double free). On CPU/HIP/CUDA the
// offset is folded into the device pointer; this CPU run covers the pointer-fold
// path (identical code for HIP/CUDA).
const char* kBufferSliceSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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

// Buffer.slice upload visibility through a kernel: a distinct pattern is
// uploaded INTO a mid-buffer view [32,96) of a 128-element parent (filled 5),
// then a kernel doubles the WHOLE parent in place. Downloading the parent
// proves (a) the slice upload landed at the byte offset — the kernel reads
// 1000+i there, doubling to 2000+2i — and (b) the head/tail the slice didn't
// cover keep the parent fill (5 -> 10). A kernel is present so the module
// registers the CPU backend (a kernel-less module selects no backend, so its
// host buffer ops no-op — a pre-existing runtime fact, orthogonal to slice).
const char* kBufferSliceUploadSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class SliceIO {\n"
    "    @Kernel\n"
    "    public static void dbl(Buffer<int32> b, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { b[i] = b[i] * 2; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 128;\n"
    "        uint32 off = 32;\n"
    "        uint32 len = 64;\n"
    "        int32[] h = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = 5; }\n"
    "        Buffer<int32> all = heap Buffer<int32>(n);\n"
    "        all.upload(h);\n"
    "        int32[] mid = heap int32[len];\n"
    "        for (uint32 i = 0; i < len; i = i + 1) { mid[i] = (int32)(1000 + i); }\n"
    "        Buffer<int32> sub = all.slice(off, len);\n"
    "        sub.upload(mid);\n"
    "        Stream s = Stream.current();\n"
    "        dbl.launch(s, grid: [2], block: [64])(all, n);\n"
    "        s.sync();\n"
    "        all.download(h);\n"
    "        for (uint32 i = 0; i < off; i = i + 1) {\n"
    "            if (h[i] != 10) { return (int32)(100 + i); }\n"
    "        }\n"
    "        for (uint32 i = 0; i < len; i = i + 1) {\n"
    "            if (h[off + i] != (int32)(2000 + 2 * i)) { return (int32)(300 + i); }\n"
    "        }\n"
    "        for (uint32 i = off + len; i < n; i = i + 1) {\n"
    "            if (h[i] != 10) { return (int32)(200 + i); }\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Texture2D;\n"
    "import cajeta.gpu.core.Sampler;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
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
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
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

// texelFetch on CPU (B3): a 2x2 texture read by EXACT integer coordinate via
// `tex.fetch(x, y)` — unfiltered, no Sampler. Thread i decodes its (x, y) from
// the row-major index and reads the stored texel, which must come back exactly
// (no bilinear blend). RGBA variant: all four channels per texel; the per-texel
// (r, +.05, +.1, +.15) data is in [0,1] so the same source is UNORM-safe within
// the 0.02 tol. Exercises the format-routed CPU upload + the exact-read seam.
static const char* kRgbaFetchSrcCpu(const char* fmt) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class TexFetchCpu {\n"
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
        "        Buffer<float32> out = heap Buffer<float32>(m);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(tex, out, w, n);\n"
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

// texelFetch on CPU, single-channel R32F: a 2x2 image {0,1,2,3} read by integer
// coordinate returns the EXACT stored value in .x (no filtering); G/B = 0, A = 1.
static const char* kR1FetchSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class TexFetchR1Cpu {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture2D tex, Buffer<float32> out,\n"
        "                             uint32 w, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 y = i / w;\n"
        "            uint32 x = i - y * w;\n"
        "            Vector<float32,4> c = tex.fetch(x, y); out[i] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        float32[] pixels = heap float32[4];\n"
        "        pixels[0] = 0.0f; pixels[1] = 1.0f;\n"
        "        pixels[2] = 2.0f; pixels[3] = 3.0f;\n"
        "        Texture2D tex = heap Texture2D(w, h);\n"          // R32F
        "        tex.upload(pixels);\n"
        "        uint32 n = 4;\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(tex, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 Step 2b: integer texelFetch on CPU. A Texture2D<int32|uint32> with a raw
// integer format (RGBA32I/RGBA32UI) stores exact integers; fetch reads all four
// channels back by integer coordinate with NO conversion — the type-preserving,
// unfiltered twin of the float RGBA fetch. `elem` is the texel/buffer scalar,
// `fmt` the matching TextureFormat. Pixel (t) holds {10t+1, 10t+2, 10t+3, 10t+4}.
static const char* kRgbaIntFetchSrcCpu(const char* elem, const char* fmt) {
    static std::string s;
    std::string e(elem);
    s = std::string(
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class TexFetchIntCpu {\n"
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
        "        Buffer<" + e + "> out = heap Buffer<" + e + ">(m);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(tex, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
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

// Single-channel integer texelFetch on CPU (R32I): the stored int lands in .x;
// the missing channels expand G/B = 0, A = 1 (the integer-texture channel default,
// matching the float fetch). 2x2 image {10,20,30,40}.
static const char* kR32iFetchSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class TexFetchR32iCpu {\n"
        "    @Kernel\n"
        "    public static void fetch(Texture2D<int32> tex, Buffer<int32> out,\n"
        "                             uint32 w, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            uint32 y = i / w;\n"
        "            uint32 x = i - y * w;\n"
        "            Vector<int32,4> c = tex.fetch(x, y);\n"
        "            out[i*2 + 0] = c.x;\n"          // stored value
        "            out[i*2 + 1] = c.w;\n"          // alpha default = 1
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2;\n"
        "        uint32 h = 2;\n"
        "        int32[] pixels = heap int32[4];\n"
        "        pixels[0] = 10; pixels[1] = 20;\n"
        "        pixels[2] = 30; pixels[3] = 40;\n"
        "        Texture2D<int32> tex = heap Texture2D<int32>(w, h, TextureFormat.R32I);\n"
        "        tex.upload(pixels);\n"
        "        uint32 n = 4;\n"
        "        int32[] hout = heap int32[n*2];\n"
        "        for (uint32 i = 0; i < n*2; i = i + 1) { hout[i] = -1; }\n"
        "        Buffer<int32> out = heap Buffer<int32>(n*2);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(tex, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i*2 + 0] != (int32)(10 + i*10)) { return (int32)(100 + i); }\n"
        "            if (hout[i*2 + 1] != 1) { return (int32)(200 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 texture dims: Texture3D fetch on CPU. A 2x2x2 R32F volume holds the linear
// voxel index (0..7, row-major x→y→z); each voxel read back exactly by integer
// (x, y, z). The 3-D analogue of textureFetchOnCpu.
static const char* kTex3dFetchSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture3D;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex3dFetchCpu {\n"
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
        "        uint32 w = 2; uint32 h = 2; uint32 d = 2;\n"
        "        uint32 n = 8;\n"
        "        float32[] voxels = heap float32[8];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { voxels[i] = (float32)(i); }\n"
        "        Texture3D vol = heap Texture3D(w, h, d);\n"
        "        vol.upload(voxels);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(vol, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// Texture3D sample on CPU: NEAREST at each voxel center reads the exact stored
// value; one TRILINEAR midpoint (u=0.5 along x at y=z centers) blends voxel 0 and
// voxel 1 to 0.5. Exercises the 3-D trilinear path + the Sampler.
static const char* kTex3dSampleSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture3D;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex3dSampleCpu {\n"
        "    @Kernel\n"
        "    public static void samp(Texture3D vol, Sampler sn,\n"
        "                            Buffer<float32> out, uint32 w, uint32 h,\n"
        "                            uint32 d, uint32 n) {\n"
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
        "        uint32 w = 2; uint32 h = 2; uint32 d = 2;\n"
        "        uint32 n = 8;\n"
        "        float32[] voxels = heap float32[8];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { voxels[i] = (float32)(i); }\n"
        "        Texture3D vol = heap Texture3D(w, h, d);\n"
        "        vol.upload(voxels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"     // nearest, clamp
        "        Sampler sl = heap Sampler(1, 0);\n"     // linear, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [n])(vol, sn, out, w, h, d, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        float32[] hmid = heap float32[1]; hmid[0] = -1.0f;\n"
        "        Buffer<float32> mout = heap Buffer<float32>(1);\n"
        "        mout.upload(hmid);\n"
        "        mid.launch(s, grid: [1], block: [1])(vol, sl, mout);\n"
        "        s.sync();\n"
        "        mout.download(hmid);\n"
        "        float32 dm = hmid[0] - 0.5f;\n"
        "        if (dm < -0.02f || dm > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 texture dims + integer: Texture3D<int32|uint32> fetch on CPU. A 2x2x2
// RGBA32I/UI volume read back as exact integers across all four channels — the
// 3-D integer voxel read (the seam threads texelTy through fetchTexture3D).
static const char* kTex3dIntFetchSrcCpu(const char* elem, const char* fmt) {
    static std::string s;
    std::string e(elem);
    s = std::string(
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture3D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex3dIntFetchCpu {\n"
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
        "        Buffer<" + e + "> out = heap Buffer<" + e + ">(m);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(vol, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
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

// B3 texture dims: Texture1D fetch on CPU. A width-4 R32F row holds the linear
// texel index (0..3); each texel read back exactly by integer x. The 1-D
// analogue of textureFetchOnCpu / texture3dFetchOnCpu (single coord, no lod).
static const char* kTex1dFetchSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture1D;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex1dFetchCpu {\n"
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
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(row, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// Texture1D sample on CPU: NEAREST at each texel center reads the exact stored
// value; one LINEAR midpoint (u=0.5 between texel 0 and 1 of a width-2 row)
// blends them to 0.5. Exercises the 1-D filtered path (the CPU reuses the 2-D
// sampler with v=0.5) + the Sampler.
static const char* kTex1dSampleSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture1D;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex1dSampleCpu {\n"
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
        "        Sampler sn = heap Sampler(0, 0);\n"     // nearest, clamp
        "        Sampler sl = heap Sampler(1, 0);\n"     // linear, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [n])(row, sn, out, w, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        float32[] hmid = heap float32[1]; hmid[0] = -1.0f;\n"
        "        Buffer<float32> mout = heap Buffer<float32>(1);\n"
        "        mout.upload(hmid);\n"
        "        mid.launch(s, grid: [1], block: [1])(row, sl, mout);\n"
        "        s.sync();\n"
        "        mout.download(hmid);\n"
        "        float32 dm = hmid[0] - 0.5f;\n"
        "        if (dm < -0.02f || dm > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 texture dims: Texture2DArray fetch on CPU. A 2x2x3 R32F array holds the
// linear texel index (0..11, layer-major); each texel read back exactly by
// integer (x, y, layer). The layered analogue of texture3dFetchOnCpu — but the
// array fetch reuses the 3-D path with z = layer.
static const char* kTex2daFetchSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2DArray;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex2daFetchCpu {\n"
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
        "        uint32 w = 2; uint32 h = 2; uint32 layers = 3;\n"
        "        uint32 n = 12;\n"
        "        float32[] texels = heap float32[12];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture2DArray arr = heap Texture2DArray(w, h, layers);\n"
        "        arr.upload(texels);\n"
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        fetch.launch(s, grid: [1], block: [n])(arr, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// Texture2DArray sample on CPU: NEAREST at each texel center reads the exact
// stored value of that layer; a LINEAR midpoint within layer 1 (u=0.5 between
// texel 0 and 1) blends them. Exercises the per-layer bilinear path (NO
// cross-layer blend — the layer is an integer index) + the Sampler.
static const char* kTex2daSampleSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2DArray;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class Tex2daSampleCpu {\n"
        "    @Kernel\n"
        "    public static void samp(Texture2DArray arr, Sampler sn,\n"
        "                            Buffer<float32> out, uint32 w, uint32 h,\n"
        "                            uint32 n) {\n"
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
        // layer 1 holds {4,5,6,7}; bilinear at the (0.5,0.25) point between
        // texel 4 and 5 along x at the top row → 4.5.
        "            Vector<float32,4> c = arr.sample(sl, 0.5f, 0.25f, 1);\n"
        "            out[0] = c.x;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 w = 2; uint32 h = 2; uint32 layers = 3;\n"
        "        uint32 n = 12;\n"
        "        float32[] texels = heap float32[12];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { texels[i] = (float32)(i); }\n"
        "        Texture2DArray arr = heap Texture2DArray(w, h, layers);\n"
        "        arr.upload(texels);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"     // nearest, clamp
        "        Sampler sl = heap Sampler(1, 0);\n"     // linear, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [n])(arr, sn, out, w, h, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        float32[] hmid = heap float32[1]; hmid[0] = -1.0f;\n"
        "        Buffer<float32> mout = heap Buffer<float32>(1);\n"
        "        mout.upload(hmid);\n"
        "        mid.launch(s, grid: [1], block: [1])(arr, sl, mout);\n"
        "        s.sync();\n"
        "        mout.download(hmid);\n"
        "        float32 dm = hmid[0] - 4.5f;\n"
        "        if (dm < -0.02f || dm > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3 texture dims: TextureCube sample on CPU. Each of the 6 faces is filled with
// a constant equal to its face index (+X=0, -X=1, +Y=2, -Y=3, +Z=4, -Z=5);
// sampling the 6 axis directions must select the matching face (the direction →
// major-axis face projection). A constant-per-face fill makes the test depend
// only on FACE SELECTION (not within-face orientation), so it agrees across
// CPU/Vulkan/AMD.
static const char* kTexCubeSampleSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.TextureCube;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class TexCubeSampleCpu {\n"
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
        "        uint32 total = faceTexels * 6;\n"
        "        float32[] faces = heap float32[24];\n"
        "        for (uint32 f = 0; f < 6; f = f + 1) {\n"
        "            for (uint32 k = 0; k < faceTexels; k = k + 1) {\n"
        "                faces[f*faceTexels + k] = (float32)(f);\n"
        "            }\n"
        "        }\n"
        "        TextureCube cube = heap TextureCube(sz);\n"
        "        cube.upload(faces);\n"
        "        Sampler sn = heap Sampler(0, 0);\n"     // nearest, clamp
        "        float32[] hout = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(n);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        samp.launch(s, grid: [1], block: [n])(cube, sn, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != (float32)(i)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3: two Sampler params in one kernel — sample the SAME texture through a
// nearest sampler AND a linear sampler. A 2x2 R32F texture {0,1,2,3}; at the
// center (u=v=0.5) nearest picks texel (1,1)=3 while linear averages all four to
// 1.5 — so both descriptors are bound and consulted independently.
static const char* kTwoSamplersSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
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
        "        Sampler sn = heap Sampler(0, 0);\n"   // nearest, clamp
        "        Sampler sl = heap Sampler(1, 0);\n"   // linear, clamp
        "        float32[] hout = heap float32[2]; hout[0] = -1.0f; hout[1] = -1.0f;\n"
        "        Buffer<float32> out = heap Buffer<float32>(2);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        both.launch(s, grid: [1], block: [1])(tex, sn, sl, out);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        float32 dn = hout[0] - 3.0f;\n"        // nearest center -> texel (1,1) = 3
        "        float32 dl = hout[1] - 1.5f;\n"        // linear center -> avg = 1.5
        "        if (dn < -0.02f || dn > 0.02f) { return (int32)(100); }\n"
        "        if (dl < -0.02f || dl > 0.02f) { return (int32)(200); }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

// B3: mipmaps. A 2-level R32F texture — level 0 = 2x2 {0,1,2,3}, level 1 = 1x1
// {99}. fetchLod reads the exact texel of a chosen level; sampleLod filters at an
// explicit level. Confirms per-level upload + the LOD-threaded fetch/sample.
static const char* kMipSrcCpu() {
    static std::string s;
    s =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Texture2D;\n"
        "import cajeta.gpu.core.TextureFormat;\n"
        "import cajeta.gpu.core.Sampler;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class MipCpu {\n"
        "    @Kernel\n"
        "    public static void mip(Texture2D tex, Sampler sl, Buffer<float32> out) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < 1) {\n"
        "            Vector<float32,4> a = tex.fetchLod(1, 1, 0);\n"   // L0 texel (1,1) = 3
        "            Vector<float32,4> b = tex.fetchLod(0, 0, 1);\n"   // L1 texel (0,0) = 99
        "            Vector<float32,4> c = tex.sampleLod(sl, 0.5f, 0.5f, 1.0f);\n" // L1 -> 99
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
        "        Sampler sl = heap Sampler(1, 0);\n"   // linear, clamp
        "        float32[] hout = heap float32[3];\n"
        "        for (uint32 i = 0; i < 3; i = i + 1) { hout[i] = -1.0f; }\n"
        "        Buffer<float32> out = heap Buffer<float32>(3);\n"
        "        out.upload(hout);\n"
        "        Stream s = Stream.current();\n"
        "        mip.launch(s, grid: [1], block: [1])(tex, sl, out);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        if (hout[0] != 3.0f) { return (int32)(100); }\n"   // L0 fetch
        "        if (hout[1] != 99.0f) { return (int32)(200); }\n"  // L1 fetch
        "        float32 dc = hout[2] - 99.0f;\n"
        "        if (dc < -0.02f || dc > 0.02f) { return (int32)(300); }\n"  // L1 sample
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

// Stage 11: a kernel calls a @Device helper in ANOTHER class (a shared device-
// math library). MathLib.square lives in its own class; DevX's kernel resolves
// it cross-class (lowerDeviceFn inlines the foreign owner's body). out[i] = i*i.
const char* kCrossClassDeviceSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class MathLib {\n"
    "    @Device\n"
    "    public static int32 square(int32 x) { return x * x; }\n"
    "}\n"
    "public class DevX {\n"
    "    @Kernel\n"
    "    public static void k(Buffer<int32> out) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        out[i] = MathLib.square((int32) i);\n"
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

TEST(XpuCpuDispatchTests, crossClassDeviceHelperOnCpu) {
    auto jit = CajetaJit::compile(kCrossClassDeviceSource, "test.DevX",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i*i — cross-class @Device call)";
}

// Buffer.slice — a non-owning sub-view passed to a kernel writes through the
// parent's storage at the slice offset; the head half stays untouched.
TEST(XpuCpuDispatchTests, bufferSliceKernelOnCpu) {
    auto jit = CajetaJit::compile(kBufferSliceSource, "test.Slice", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: head touched; 200+i: tail[i] != i)";
}

// Buffer.slice — upload/download through a mid-buffer view honor the byte
// offset; only the sliced range changes, the surrounding parent is preserved.
TEST(XpuCpuDispatchTests, bufferSliceUploadDownloadOnCpu) {
    auto jit = CajetaJit::compile(kBufferSliceUploadSource, "test.SliceIO",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: head; 300+i: mid; 200+i: tail clobbered)";
}

// Buffer MemoryKind (Stage B4) — a host-accessible (Unified) buffer with
// zero-copy hostStore/hostLoad on the CPU. On the CPU "device" memory IS host
// memory, so a Unified buffer's hostStore writes straight into the storage the
// kernel reads, and hostLoad reads the kernel's results back — no upload /
// download device transfer anywhere. Exercises the MemoryKind constructor, the
// kind-threaded alloc/free (Unified ordinal 2), and the host-copy path end to
// end. (The HIP twin proves genuine zero-copy managed memory on gfx1151.)
const char* kMemKindUnifiedSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.MemoryKind;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "        u.allocate(MemoryKind.Unified);\n"
    "        u.hostStore(h);\n"                 // zero-copy host write (no upload)
    "        Stream s = Stream.current();\n"
    "        inc.launch(s, grid: [1], block: [64])(u, n);\n"
    "        s.sync();\n"
    "        int32[] out = heap int32[n];\n"
    "        u.hostLoad(out);\n"                // zero-copy host read (no download)
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (out[i] != (int32)(i + 1)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuCpuDispatchTests, memoryKindUnifiedHostCopyOnCpu) {
    auto jit = CajetaJit::compile(kMemKindUnifiedSource, "test.MemKind",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — host<->device sharing broke)";
}

// Async copies / transfer queues (Stage B4) — the full async pipeline on the
// CPU (portability). Stream.create() is the default stream (handle 0) on the CPU
// rung, and async copies + the launch run synchronously, but the SAME source —
// uploadAsync / launch(s) / downloadAsync / one sync — compiles and is correct
// everywhere. The HIP twin proves a real per-stream async queue on gfx1151.
const char* kAsyncPipelineSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "        b.uploadAsync(h, s);\n"
    "        inc.launch(s, grid: [4], block: [64])(b, n);\n"
    "        int32[] out = heap int32[n];\n"
    "        b.downloadAsync(out, s);\n"
    "        s.sync();\n"
    "        s.destroy();\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (out[i] != (int32)(i + 1)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuCpuDispatchTests, asyncCopyPipelineOnCpu) {
    auto jit = CajetaJit::compile(kAsyncPipelineSource, "test.Pipe",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — async pipeline broke)";
}

// Event / Fence (Stage B4 async follow-on) — cross-stream + host sync on the CPU.
// Two streams increment the same buffer; an Event records s1's tail and s2
// waitFor(e)s it, so s2's +1 is ordered after s1's +1 (out[i] == i+2). A Fence
// then signals s2 and the host waitHost()/query()s it. On CPU everything is
// synchronous (the event is an always-signaled sentinel) so this is the
// portability/compile proof; HIP exercises the real hipEvent ordering.
const char* kEventFenceSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Event;\n"
    "import cajeta.gpu.core.Fence;\n"
    "import cajeta.gpu.core.Thread;\n"
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
    "        e.recordOn(s1);\n"                                 // mark s1's tail
    "        s2.waitFor(e);\n"                                  // s2 waits on s1
    "        inc.launch(s2, grid: [4], block: [64])(b, n);\n"   // s2: +1, after s1
    "        int32[] out = heap int32[n];\n"
    "        b.downloadAsync(out, s2);\n"
    "        Fence f = Fence.create();\n"
    "        f.signal(s2);\n"                                   // fence at s2's tail
    "        f.waitHost();\n"                                   // host blocks
    "        boolean done = f.query();\n"
    "        s1.sync();\n"                                      // release borrows
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

TEST(XpuCpuDispatchTests, eventFenceSyncOnCpu) {
    auto jit = CajetaJit::compile(kEventFenceSource, "test.Sync", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (1: fence not signaled; 100+i: out[i] != i+2 — "
                         "event/fence sync broke)";
}

// Bindless / multi-buffer descriptor sets (Stage B4) — portability on the CPU.
// The same Buffer<int32>[] gather kernel: bufs[b][i] across `count` buffers
// indexed at runtime. On the CPU a buffer-array param is the [count, h…] handle
// array (the launcher thunk passes the slot pointer straight through; the device
// default bufferArrayElement loads the (1+idx)-th handle and inttoptrs it).
// Buffer b = (b+1)*10 + i, so out[i] = 60 + 3i for 3 buffers. (The real bindless
// descriptor array is the Vulkan path; the CPU shares the same source.)
const char* kBindlessSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class Bindless {\n"
    "    @Kernel\n"
    "    public static void gather(Buffer<int32>[] bufs, uint32 count,\n"
    "                              Buffer<int32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int32 s = 0;\n"
    "            for (uint32 b = 0; b < count; b = b + 1) {\n"
    "                s = s + bufs[b][i];\n"
    "            }\n"
    "            out[i] = s;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        uint32 k = 3;\n"
    "        Buffer<int32>[] bufs = heap Buffer<int32>[k];\n"
    "        for (uint32 b = 0; b < k; b = b + 1) {\n"
    "            int32[] h = heap int32[n];\n"
    "            for (uint32 i = 0; i < n; i = i + 1) {\n"
    "                h[i] = (int32)((b + 1) * 10 + i);\n"
    "            }\n"
    "            bufs[b] = heap Buffer<int32>(n);\n"
    "            bufs[b].upload(h);\n"
    "        }\n"
    "        Buffer<int32> out = heap Buffer<int32>(n);\n"
    "        Stream s = Stream.current();\n"
    "        gather.launch(s, grid: [1], block: [64])(bufs, k, out, n);\n"
    "        s.sync();\n"
    "        int32[] ho = heap int32[n];\n"
    "        out.download(ho);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (ho[i] != (int32)(60 + 3 * i)) { return (int32)(100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuCpuDispatchTests, bindlessBufferArrayOnCpu) {
    auto jit = CajetaJit::compile(kBindlessSource, "test.Bindless", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 60+3i — CPU bindless indexing wrong)";
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

// B3 texelFetch on CPU, single-channel R32F: `tex.fetch(x, y)` reads the exact
// stored texel by integer coord — no Sampler, no filtering. The 2x2 {0,1,2,3}
// image must return each value exactly in .x.
TEST(XpuCpuDispatchTests, textureFetchOnCpu) {
    auto jit = CajetaJit::compile(kR1FetchSrcCpu(), "test.TexFetchR1Cpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: R32F fetch mismatch at i)";
}

// B3 texelFetch on CPU, RGBA32F: all four channels of each texel read back
// exactly by integer coordinate (the unfiltered twin of textureSampleRgba32fOnCpu).
TEST(XpuCpuDispatchTests, textureFetchRgba32fOnCpu) {
    auto jit = CajetaJit::compile(kRgbaFetchSrcCpu("TextureFormat.RGBA32F"),
                                  "test.TexFetchCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: RGBA32F fetch mismatch at i)";
}

// B3 Step 2b: integer texelFetch on CPU, RGBA32I — a Texture2D<int32> read back
// as exact signed integers across all four channels (the int twin of RGBA32F).
TEST(XpuCpuDispatchTests, textureFetchRgba32iOnCpu) {
    auto jit = CajetaJit::compile(kRgbaIntFetchSrcCpu("int32", "TextureFormat.RGBA32I"),
                                  "test.TexFetchIntCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (RGBA32I fetch mismatch)";
}

// RGBA32UI — Texture2D<uint32>, exact unsigned integers across four channels.
TEST(XpuCpuDispatchTests, textureFetchRgba32uiOnCpu) {
    auto jit = CajetaJit::compile(kRgbaIntFetchSrcCpu("uint32", "TextureFormat.RGBA32UI"),
                                  "test.TexFetchIntCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (RGBA32UI fetch mismatch)";
}

// Single-channel R32I — the stored int lands in .x; missing channels expand
// G/B = 0, A = 1 (the integer-texture channel default).
TEST(XpuCpuDispatchTests, textureFetchR32iOnCpu) {
    auto jit = CajetaJit::compile(kR32iFetchSrcCpu(), "test.TexFetchR32iCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: R32I value; 200+i: alpha default)";
}

// B3 texture dims: Texture3D fetch on CPU — a 2x2x2 volume read voxel-exact.
TEST(XpuCpuDispatchTests, texture3dFetchOnCpu) {
    auto jit = CajetaJit::compile(kTex3dFetchSrcCpu(), "test.Tex3dFetchCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: voxel index mismatch at i)";
}

// Texture3D sample on CPU — nearest at voxel centers (exact) + a trilinear
// midpoint (voxel 0/1 blend = 0.5).
TEST(XpuCpuDispatchTests, texture3dSampleOnCpu) {
    auto jit = CajetaJit::compile(kTex3dSampleSrcCpu(), "test.Tex3dSampleCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: nearest; 200: trilinear midpoint)";
}

// B3 texture dims: Texture1D fetch on CPU — a width-4 row read texel-exact.
TEST(XpuCpuDispatchTests, texture1dFetchOnCpu) {
    auto jit = CajetaJit::compile(kTex1dFetchSrcCpu(), "test.Tex1dFetchCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: texel index mismatch at i)";
}

// Texture1D sample on CPU — nearest at texel centers (exact) + a linear midpoint
// (texel 0/1 blend = 0.5).
TEST(XpuCpuDispatchTests, texture1dSampleOnCpu) {
    auto jit = CajetaJit::compile(kTex1dSampleSrcCpu(), "test.Tex1dSampleCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: nearest; 200: linear midpoint)";
}

// B3 texture dims: Texture2DArray fetch on CPU — a 2x2x3 array read texel-exact.
TEST(XpuCpuDispatchTests, texture2dArrayFetchOnCpu) {
    auto jit = CajetaJit::compile(kTex2daFetchSrcCpu(), "test.Tex2daFetchCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: layer/texel mismatch at i)";
}

// Texture2DArray sample on CPU — nearest at texel centers per layer (exact) + a
// within-layer-1 linear midpoint (texel 4/5 blend = 4.5).
TEST(XpuCpuDispatchTests, texture2dArraySampleOnCpu) {
    auto jit = CajetaJit::compile(kTex2daSampleSrcCpu(), "test.Tex2daSampleCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: nearest; 200: layer-1 linear midpoint)";
}

// B3 texture dims: TextureCube sample on CPU — the 6 axis directions select the 6
// faces (constant-per-face fill, face f = f).
TEST(XpuCpuDispatchTests, textureCubeSampleOnCpu) {
    auto jit = CajetaJit::compile(kTexCubeSampleSrcCpu(), "test.TexCubeSampleCpu",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: cube face selection mismatch at dir i)";
}

// B3: mipmaps on CPU — fetchLod reads a chosen mip level exactly (L0=3, L1=99),
// sampleLod filters at an explicit level. Per-level upload + LOD threading.
TEST(XpuCpuDispatchTests, mipmapFetchAndSampleLodOnCpu) {
    auto jit = CajetaJit::compile(kMipSrcCpu(), "test.MipCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100: L0 fetch; 200: L1 fetch; 300: L1 sample)";
}

// B3: two Sampler params in one kernel both bind + work (nearest vs linear on the
// same texture give 3.0 vs 1.5 at the center). Confirms multiple samplers per
// kernel — a gap that turned out to already work once the KernelParam field-order
// trap (which mis-bound Sampler params) was fixed.
TEST(XpuCpuDispatchTests, twoSamplersInOneKernelOnCpu) {
    auto jit = CajetaJit::compile(kTwoSamplersSrcCpu(), "test.TwoSamp", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100: nearest=3.0; 200: linear=1.5)";
}

// Integer Texture3D fetch on CPU — a 2x2x2 RGBA32I volume read voxel-exact (the
// 3-D twin of textureFetchRgba32iOnCpu; fetchTexture3D threads the int texel type).
TEST(XpuCpuDispatchTests, texture3dFetchRgba32iOnCpu) {
    auto jit = CajetaJit::compile(kTex3dIntFetchSrcCpu("int32", "TextureFormat.RGBA32I"),
                                  "test.Tex3dIntFetchCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (3D RGBA32I fetch mismatch)";
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

// Image2D storage RMW on the CPU reference path — the writable twin of the CPU
// texture fetch. `fill` writes each texel via img.store(); `rmw` reads-modify-
// writes it (2v+1) via img.load()+img.store(); the host downloads and checks
// 2i+1. Exercises the CPU storeImage/loadImage seam (__cajeta_xpu_cpu_image_store
// /_load over the host float store) + the image alloc/download. Same kernel and
// closed-form (2i+1) as the Vulkan/AMD storage-image tests, so the in-process
// oracle now cross-checks the device path for storage images. CPU is the floor —
// always available, no skip.
TEST(XpuCpuDispatchTests, imageLoadStoreRmwOnCpu) {
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Image2D;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class ImgRmwCpu {\n"
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
    auto jit = CajetaJit::compile(src, "test.ImgRmwCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 2*i+1 — storeImage/loadImage RMW)";
}

// Stage 9: a kernel using scoped memory fences (Barrier.deviceMemory /
// .workgroupMemory) runs on CPU — the reference oracle. On CPU the fence
// lowers to a system acq_rel `fence`; a single-thread write→fence→read
// deterministically yields out[i] == 2i+1, proving the verb compiles to valid
// runnable code (cross-checks the VK/AMD device runs of the same kernel).
TEST(XpuCpuDispatchTests, memoryFenceOnCpu) {
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "import cajeta.gpu.core.Barrier;\n"
        "public class MFCpu {\n"
        "    @Kernel\n"
        "    public static void fence(Buffer<int32> data, Buffer<int32> out,\n"
        "                             uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            data[i] = (int32)(i * 2);\n"
        "            Barrier.deviceMemory();\n"
        "            Barrier.workgroupMemory();\n"
        "            out[i] = data[i] + 1;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        Buffer<int32> data = heap Buffer<int32>(n);\n"
        "        Buffer<int32> out = heap Buffer<int32>(n);\n"
        "        Stream s = Stream.current();\n"
        "        fence.launch(s, grid: [1], block: [64])(data, out, n);\n"
        "        s.sync();\n"
        "        int32[] ho = heap int32[n];\n"
        "        out.download(ho);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (ho[i] != (int32)(2 * i + 1)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.MFCpu", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 2i+1 — memory fence broke codegen)";
}

// Stage 9: the MemoryOrder surface — an explicit MemoryOrder.Relaxed on a kernel
// atomic. CPU is the oracle: N threads each atomicAdd(0, 1, Relaxed); relaxed
// atomics still guarantee atomicity (just not ordering), so the count is exact
// (out[0] == N). Proves enum constants resolve in kernels + the relaxed atomic
// path runs. (Cross-checks the VK/AMD device runs of the same kernel.)
TEST(XpuCpuDispatchTests, relaxedAtomicCounterOnCpu) {
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Stream;\n"
        "import cajeta.gpu.core.Thread;\n"
        "import cajeta.gpu.core.MemoryOrder;\n"
        "public class RAC {\n"
        "    @Kernel\n"
        "    public static void count(Buffer<int32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            out.atomicAdd(0, 1, MemoryOrder.Relaxed);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        Buffer<int32> out = heap Buffer<int32>(1);\n"
        "        int32[] z = heap int32[1];\n"
        "        z[0] = 0;\n"
        "        out.upload(z);\n"
        "        Stream s = Stream.current();\n"
        "        count.launch(s, grid: [1], block: [256])(out, n);\n"
        "        s.sync();\n"
        "        int32[] ho = heap int32[1];\n"
        "        out.download(ho);\n"
        "        if (ho[0] != 256) { return ho[0]; }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.RAC", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "got " << r << " (expected 256 — relaxed atomic count)";
}
