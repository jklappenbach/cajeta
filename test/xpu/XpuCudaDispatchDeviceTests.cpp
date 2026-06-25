//
// CajetaXPU — the runtime dispatcher routing host-source @Kernel programs to
// CUDA on a real NVIDIA device (the NVPTX twin of XpuHipDispatchDeviceTests).
//
// Every test here compiles BOTH the @Kernel AND the host orchestration from
// Cajeta source through the LLJIT with --xpu-backend=nvptx. At first device
// touch the runtime dispatcher (cajeta_runtime.c) picks CUDA (bundled +
// available) and routes the whole pipeline — allocate / upload / launch / sync /
// download / free, plus streams, events, atomics, fences, bindless arrays,
// managed/pinned memory and spec-constant override — through the in-C CUDA path
// (cuMemAlloc / cuMemcpyHtoD / cuLaunchKernel / cuCtxSynchronize / …). The
// device cubin is built by the NVPTX registration pass (ptxas) and registered
// via an llvm.global_ctors entry LLJIT runs at initialize().
//
// These exercise the SAME backend-agnostic Cajeta sources the HIP/Vulkan/CPU
// dispatch tests run, so all backends cross-check to the same observable result
// — the variance-discipline payoff: one source, one memory model, every device.
//
// Each test skips cleanly when no CUDA device/driver is present.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

#if defined(_WIN32)
// POSIX setenv/unsetenv are absent on mingw; shim onto _putenv_s.
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cudaOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    return o;
}

const char* kSaxpyHostSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Saxpy {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
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
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        x.allocate();\n"
    "        y.allocate();\n"
    "        x.upload(hx);\n"
    "        y.upload(hy);\n"
    "        KernelStream s = KernelStream.current();\n"
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

// The dispatcher routes a host-source @Kernel SAXPY to CUDA on the real NVIDIA
// device — allocate/upload/launch/sync/download all through the in-C CUDA path.
TEST(XpuCudaDispatchDeviceTests, saxpyRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 4096.0f);   // 1024 * (2*1 + 2)
}

// A grid-stride for-each on the real NVIDIA device. The stride comes from the
// launch grid_size; grid is deliberately smaller than n (grid=4, block=64 ⇒ 256
// work-items < n=1024), so the loop must stride 4× to cover all of out[i]=in[i]*2
// (in[i]=1) ⇒ sum 2048. One element per thread would give 512 — distinguishing.
TEST(XpuCudaDispatchDeviceTests, gridStrideForEachRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Grid {\n"
        "    @Kernel\n"
        "    public static void scale(KernelBuffer<float32> out, KernelBuffer<float32> in,\n"
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
        "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
        "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
        "        x.allocate();\n"
        "        y.allocate();\n"
        "        x.upload(hx);\n"
        "        y.upload(hy);\n"
        "        KernelStream s = KernelStream.current();\n"
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
    auto jit = CajetaJit::compile(src, "test.Grid", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 2048.0f);   // all 1024 ran via grid-stride
}

// A @Device helper taking KernelBuffer<T> params, on the real NVIDIA device. The kernel
// passes its two buffer bases into the inlined helper, which reads `in` and writes
// `out`. out[i]=in[i]*3, in[i]=i ⇒ sum over [0,256) of 3i = 3·(255·256/2) = 97920.
TEST(XpuCudaDispatchDeviceTests, deviceBufferParamHelperRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class DevBuf {\n"
        "    @Device\n"
        "    public static void scale(KernelBuffer<float32> out, KernelBuffer<float32> in,\n"
        "                             uint32 i) {\n"
        "        out[i] = in[i] * 3.0f;\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        scale(out, in, i);\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 256;\n"
        "        float32[] hx = heap float32[n];\n"
        "        float32[] hy = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hx[i] = (float32)i; hy[i] = 0.0f; }\n"
        "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
        "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
        "        x.allocate();\n"
        "        y.allocate();\n"
        "        x.upload(hx);\n"
        "        y.upload(hy);\n"
        "        KernelStream s = KernelStream.current();\n"
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
    auto jit = CajetaJit::compile(src, "test.DevBuf", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 97920.0f);   // sum 3i over [0,256)
}

// A POD struct passed BY VALUE as a kernel arg, on the real NVIDIA device.
// `Params { float32 scale; float32 bias; }` rides the cuLaunchKernel kernelParams
// ABI by value; the kernel reads p.scale/p.bias to compute out[i] = i*scale+bias.
// scale=2, bias=1, n=256 ⇒ Σ(2i+1) = 2·(255·256/2)+256 = 65536.
TEST(XpuCudaDispatchDeviceTests, podStructArgRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Params {\n"
        "    float32 scale;\n"
        "    float32 bias;\n"
        "    public Params(float32 scale, float32 bias)"
        " { this.scale = scale; this.bias = bias; }\n"
        "}\n"
        "public class PodArg {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> out, Params p) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        out[i] = (float32)i * p.scale + p.bias;\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        uint32 n = 256;\n"
        "        float32[] hy = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hy[i] = 0.0f; }\n"
        "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
        "        y.allocate();\n"
        "        y.upload(hy);\n"
        "        Params p = heap Params(2.0f, 1.0f);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        k.launch(s, grid: [4], block: [64])(y, p);\n"
        "        s.sync();\n"
        "        y.download(hy);\n"
        "        y.free();\n"
        "        float32 sum = 0.0f;\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + hy[i]; }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.PodArg", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 65536.0f);   // Σ(2i+1) over [0,256)
}

// KernelBuffer.slice on CUDA — device-verify the pointer-fold path. The device handle
// is a CUdeviceptr, so slice() folds the byte offset (handle + offset*elementBytes)
// at __cajeta_xpu_buffer_slice; upload/download/launch-arg stay offset-unaware. A
// 128-element parent is filled -1; the tail half [64,128) is sliced and a kernel
// writes globalIdX (0..63) through the view → lands at parent[64+i], head untouched.
TEST(XpuCudaDispatchDeviceTests, bufferSliceKernelRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Slice {\n"
        "    @Kernel\n"
        "    public static void fill(KernelBuffer<int32> b, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { b[i] = (int32) i; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 128;\n"
        "        uint32 half = 64;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = -1; }\n"
        "        KernelBuffer<int32> all = heap KernelBuffer<int32>(n);\n"
        "        all.upload(h);\n"
        "        KernelBuffer<int32> tail = all.slice(half, half);\n"
        "        KernelStream s = KernelStream.current();\n"
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
    auto jit = CajetaJit::compile(src, "test.Slice", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (1xx: head overwritten — bad offset; "
                         "2xx: tail wrong — view base off)";
}

// A kernel atomic with explicit MemoryOrder.Relaxed runs on the NVIDIA device.
// N threads each atomicAdd(0, 1, Relaxed) → out[0] == N (atomicity holds
// regardless of ordering). Same kernel as the CPU/AMD oracles.
TEST(XpuCudaDispatchDeviceTests, relaxedAtomicCounterRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.MemoryOrder;\n"
        "public class RAC {\n"
        "    @Kernel\n"
        "    public static void count(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            out.atomicAdd(0, 1, MemoryOrder.Relaxed);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(1);\n"
        "        int32[] z = heap int32[1];\n"
        "        z[0] = 0;\n"
        "        out.upload(z);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        count.launch(s, grid: [1], block: [256])(out, n);\n"
        "        s.sync();\n"
        "        int32[] ho = heap int32[1];\n"
        "        out.download(ho);\n"
        "        if (ho[0] != 256) { return ho[0]; }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.RAC", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "got " << r << " (expected 256 — relaxed atomic count)";
}

// Scoped memory fences (Barrier.deviceMemory / .workgroupMemory) on the NVIDIA
// device — lower to membar.gl / membar.cta. Same write→fence→read kernel as the
// CPU/AMD oracle → out[i] == 2i+1.
TEST(XpuCudaDispatchDeviceTests, memoryFenceRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "public class MF {\n"
        "    @Kernel\n"
        "    public static void fence(KernelBuffer<int32> data, KernelBuffer<int32> out,\n"
        "                             uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            data[i] = (int32)(i * 2);\n"
        "            Barrier.deviceMemory();\n"
        "            Barrier.workgroupMemory();\n"
        "            out[i] = data[i] + 1;\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        KernelBuffer<int32> data = heap KernelBuffer<int32>(n);\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);\n"
        "        KernelStream s = KernelStream.current();\n"
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
    auto jit = CajetaJit::compile(src, "test.MF", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 2i+1 — memory fence on CUDA device)";
}

// Shared-memory tree reduction through the runtime dispatcher (Cajeta-source
// orchestration, not the C++ CudaDriver path of XpuSharedDeviceTests). `shared
// int32[256]` lowers to addrspace(3) + bar.sync; one block of 256 reduces
// in[i]=i → 256*255/2 = 32640.
TEST(XpuCudaDispatchDeviceTests, sharedReductionRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Workgroup;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "public class Red {\n"
        "    @Kernel\n"
        "    public static void reduce(KernelBuffer<int32> out, KernelBuffer<int32> in, uint32 n) {\n"
        "        Shared<int32> tile = shared int32[256];\n"
        "        uint32 t = KernelThread.x();\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        if (g < n) { tile[t] = in[g]; } else { tile[t] = 0; }\n"
        "        Barrier.workgroup();\n"
        "        for (uint32 s = 128; s > 0; s >>= 1) {\n"
        "            if (t < s) { tile[t] += tile[t + s]; }\n"
        "            Barrier.workgroup();\n"
        "        }\n"
        "        if (t == 0) { out[Workgroup.x()] = tile[0]; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        int32[] hin = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) i; }\n"
        "        KernelBuffer<int32> in = heap KernelBuffer<int32>(n);\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(1);\n"
        "        in.upload(hin);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        reduce.launch(s, grid: [1], block: [256])(out, in, n);\n"
        "        s.sync();\n"
        "        int32[] ho = heap int32[1];\n"
        "        out.download(ho);\n"
        "        if (ho[0] != 32640) { return ho[0]; }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Red", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "got " << r << " (expected 32640 — shared tree reduction)";
}

// Bindless KernelBuffer<int32>[] (descriptor-indexed buffers) on CUDA. The launch
// marshals the host [count, h0 …] handle array into device memory and passes a
// device pointer; the kernel reads each device handle and gathers. Same gather +
// values as the CPU/AMD/Vulkan oracles → out[i] == 60 + 3i.
TEST(XpuCudaDispatchDeviceTests, bindlessBufferArrayRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Bindless {\n"
        "    @Kernel\n"
        "    public static void gather(KernelBuffer<int32>[] bufs, uint32 count,\n"
        "                              KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
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
        "        KernelBuffer<int32>[] bufs = heap KernelBuffer<int32>[k];\n"
        "        for (uint32 b = 0; b < k; b = b + 1) {\n"
        "            int32[] h = heap int32[n];\n"
        "            for (uint32 i = 0; i < n; i = i + 1) {\n"
        "                h[i] = (int32)((b + 1) * 10 + i);\n"
        "            }\n"
        "            bufs[b] = heap KernelBuffer<int32>(n);\n"
        "            bufs[b].upload(h);\n"
        "        }\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);\n"
        "        KernelStream s = KernelStream.current();\n"
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
    auto jit = CajetaJit::compile(src, "test.Bindless", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 60+3i — bindless descriptor-array "
                         "indexing wrong on CUDA)";
}

// Async copies / transfer queues on a REAL CUDA stream (cuStreamCreate). Three
// operations queue on one stream: async H2D upload, a stream-ordered kernel
// launch (cuLaunchKernel with the stream), and async D2H download. A SINGLE
// stream.sync() drains the whole pipeline; out[i] == i+1 is the ordering proof.
TEST(XpuCudaDispatchDeviceTests, asyncCopyPipelineRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Pipe {\n"
        "    @Kernel\n"
        "    public static void inc(KernelBuffer<int32> b, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        KernelBuffer<int32> b = heap KernelBuffer<int32>(n);\n"
        "        KernelStream s = KernelStream.create();\n"
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
    auto jit = CajetaJit::compile(src, "test.Pipe", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — async pipeline mis-ordered "
                         "on the CUDA stream)";
}

// Event / Fence cross-stream ordering + host fence on the NVIDIA device. Two REAL
// CUDA streams increment the same buffer; an Event records s1's tail and s2
// cuStreamWaitEvent()s it, so s2's +1 runs only after s1's +1 completes — no
// concurrent RMW race → out[i] == i+2. A Fence then records at s2's tail and the
// host cuEventSynchronize/Query()s it.
TEST(XpuCudaDispatchDeviceTests, eventFenceSyncRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.Event;\n"
        "import cajeta.xpu.Fence;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Sync {\n"
        "    @Kernel\n"
        "    public static void inc(KernelBuffer<int32> b, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 256;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        KernelBuffer<int32> b = heap KernelBuffer<int32>(n);\n"
        "        KernelStream s1 = KernelStream.create();\n"
        "        KernelStream s2 = KernelStream.create();\n"
        "        Event e = Event.create();\n"
        "        b.uploadAsync(h, s1);\n"
        "        inc.launch(s1, grid: [4], block: [64])(b, n);\n"
        "        e.recordOn(s1);\n"
        "        s2.waitFor(e);\n"
        "        inc.launch(s2, grid: [4], block: [64])(b, n);\n"
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
    auto jit = CajetaJit::compile(src, "test.Sync", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (1: fence not signaled; 100+i: out[i] != i+2 — "
                         "cross-stream event ordering broke on the CUDA device)";
}

// UNIFIED (managed) memory on CUDA: cuMemAllocManaged gives one pointer the host
// and device both address. The host writes via hostStore (a memcpy into managed
// memory, NO cuMemcpyHtoD), a kernel increments every element, the host reads via
// hostLoad (NO cuMemcpyDtoH). That the kernel saw the host's writes and vice
// versa — with no transfer call — is the zero-copy proof. kind ordinal 2.
static const char* cudaMemKindSource(const char* kindExpr) {
    static std::string s;
    s = std::string(
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.MemoryKind;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class MemKind {\n"
        "    @Kernel\n"
        "    public static void inc(KernelBuffer<int32> b, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { b[i] = b[i] + 1; }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] h = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (int32) i; }\n"
        "        KernelBuffer<int32> u = heap KernelBuffer<int32>(0, n);\n"
        "        u.allocate(") + kindExpr + ");\n"
        "        u.hostStore(h);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        inc.launch(s, grid: [1], block: [64])(u, n);\n"
        "        s.sync();\n"
        "        int32[] out = heap int32[n];\n"
        "        u.hostLoad(out);\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (out[i] != (int32)(i + 1)) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    return s.c_str();
}

TEST(XpuCudaDispatchDeviceTests, memoryKindUnifiedZeroCopyRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(cudaMemKindSource("MemoryKind.Unified"),
                                  "test.MemKind", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — managed host<->device "
                         "sharing broke)";
}

// PINNED (page-locked, device-mapped host) memory on CUDA: cuMemHostAlloc gives
// host memory the kernel reads directly through unified addressing. Same zero-copy
// hostStore→inc→hostLoad shape; also exercises kind-aware free → cuMemFreeHost.
TEST(XpuCudaDispatchDeviceTests, memoryKindPinnedZeroCopyRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(cudaMemKindSource("MemoryKind.Pinned"),
                                  "test.MemKind", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != i+1 — pinned host<->device "
                         "sharing broke)";
}

// Host spec-constant override (Stage 12 Phase C) on the NVIDIA device: the runtime
// sets the module's constant-memory __cajeta_xpu_spec_count/_values per launch via
// cuModuleGetGlobal + cuMemcpyHtoD, and the kernel reads the override (1234, not
// the default 7). This is the on-device proof of the CUDA constant-memory path the
// emit gate could only prove was wired.
TEST(XpuCudaDispatchDeviceTests, specOverrideRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class SO {\n"
        "    @Kernel\n"
        "    public static void fill(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { out[i] = Spec.geti(0, 7); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] hout = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, n);\n"
        "        out.allocate();\n"
        "        out.upload(hout);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        fill.launch(s, grid: [1], block: [64], spec: [1234])(out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != 1234) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.SO", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != overridden 1234)";
}

// Spec override slot-indexing + sparse tail on CUDA — slot 0 overridden, slot 1
// keeps its compile-time default (the runtime writes only the supplied values).
TEST(XpuCudaDispatchDeviceTests, specOverridePartialReadsDefaultForUnsetSlotsOnCuda) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class SOP {\n"
        "    @Kernel\n"
        "    public static void fill(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            out[i*2] = Spec.geti(0, 11);\n"
        "            out[i*2 + 1] = Spec.geti(1, 22);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 32;\n"
        "        int32[] hout = heap int32[n*2];\n"
        "        for (uint32 i = 0; i < n*2; i = i + 1) { hout[i] = -1; }\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, n*2);\n"
        "        out.allocate();\n"
        "        out.upload(hout);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        fill.launch(s, grid: [1], block: [64], spec: [555])(out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i*2] != 555) { return (int32)(100 + i); }\n"
        "            if (hout[i*2 + 1] != 22) { return (int32)(200 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.SOP", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+i: slot0 != overridden 555; 200+i: slot1 != default 22)";
}

// No `spec:` → the kernel reads its compile-time default (the zero-init safe
// path; runtime writes count 0, clearing any prior launch's values).
TEST(XpuCudaDispatchDeviceTests, noOverrideReadsDefaultOnCuda) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class SD {\n"
        "    @Kernel\n"
        "    public static void fill(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { out[i] = Spec.geti(0, 99); }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        uint32 n = 64;\n"
        "        int32[] hout = heap int32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
        "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, n);\n"
        "        out.allocate();\n"
        "        out.upload(hout);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        out.free();\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            if (hout[i] != 99) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.SD", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] != default 99)";
}

// Texture2D sampled through a Sampler with bilinear filtering, on the real NVIDIA
// device. The Texture2D is a CUDA array; at launch the runtime builds a CUtexObject
// from it + the Sampler's modes, and the kernel samples it via
// llvm.nvvm.tex.unified.2d → PTX tex.2d. A 2×2 R32F image {0,1,2,3} sampled at the
// four texel centers returns the exact texels; the dead-center (0.5,0.5) returns
// the 4-texel average 1.5. Epsilon compare guards GPU interpolation precision.
TEST(XpuCudaDispatchDeviceTests, textureSampleRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.gfx.Texture2D;\n"
        "import cajeta.gfx.Sampler;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class TexSample {\n"
        "    @Kernel\n"
        "    public static void sample(Texture2D tex, Sampler s,\n"
        "                              KernelBuffer<float32> us, KernelBuffer<float32> vs,\n"
        "                              KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
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
        "        KernelBuffer<float32> us = heap KernelBuffer<float32>(0, n);\n"
        "        KernelBuffer<float32> vs = heap KernelBuffer<float32>(0, n);\n"
        "        KernelBuffer<float32> out = heap KernelBuffer<float32>(0, n);\n"
        "        us.allocate(); vs.allocate(); out.allocate();\n"
        "        us.upload(hus); vs.upload(hvs); out.upload(hout);\n"
        "        KernelStream s = KernelStream.current();\n"
        "        sample.launch(s, grid: [1], block: [64])(tex, samp, us, vs, out, n);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        us.free(); vs.free(); out.free();\n"
        "        if (hout[0] == -1.0f) { return (int32)(555); }\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = hout[i] - hexp[i];\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.TexSample", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    if (r == 555) {
        GTEST_SKIP() << "CUDA texture alloc unavailable (driver lacks "
                        "cuArrayCreate/cuTexObjectCreate)";
    }
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: sampled texel != expected)";
}

// Image2D storage RMW on the real NVIDIA device — the writable twin of the texture
// path. `fill` writes each texel via img.store(); `rmw` reads-modify-writes it
// (2v+1) via img.load()+img.store(); the host downloads and checks 2i+1. Exercises
// the NVPTX storeImage/loadImage seam (sust.b.2d / suld.b.2d over a CUsurfObject)
// end to end: surface-capable cuArray + the CAJETA_KP_IMAGE launch arm
// (cuSurfObjectCreate) + the cuMemcpy2D download.
TEST(XpuCudaDispatchDeviceTests, imageLoadStoreRmwRoutesToCudaOnDevice) {
    CAJETA_SKIP_IF_NO_CUDA();
    const char* src =
        "package test;\n"
        "import cajeta.xpu.Image2D;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class ImgRmwCuda {\n"
        "    @Kernel\n"
        "    public static void fill(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < w * h) { img.store(i % w, i / w, (float32)(i / w * w + i % w)); }\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void rmw(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
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
        "        KernelStream s = KernelStream.current();\n"
        "        fill.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        rmw.launch(s, grid: [1], block: [64])(img, w, h);\n"
        "        s.sync();\n"
        "        float32[] out = heap float32[n];\n"
        "        for (uint32 i = 0; i < n; i = i + 1) { out[i] = -1.0f; }\n"
        "        img.download(out);\n"
        "        if (out[0] == -1.0f) { return (int32)(555); }\n"
        "        for (uint32 i = 0; i < n; i = i + 1) {\n"
        "            float32 d = out[i] - (float32)(2 * i + 1);\n"
        "            if (d < -0.01f || d > 0.01f) { return (int32)(100 + i); }\n"
        "        }\n"
        "        return 777;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.ImgRmwCuda", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    if (r == 555) {
        GTEST_SKIP() << "CUDA storage images unavailable (driver lacks "
                        "cuArrayCreate/cuSurfObjectCreate)";
    }
    EXPECT_EQ(r, 777) << "fail code " << r
                      << " (100+i: out[i] != 2*i+1 — storeImage/loadImage RMW)";
}

// Bundle BOTH nvptx and cpu; CAJETA_XPU_BACKEND=cpu forces the fall to the CPU
// even on a box WITH the NVIDIA GPU present — the explicit-bundle degrade-to-CPU
// contract, validated against real hardware. GPU-independent (forced to CPU), but
// builds the nvptx cubin too, exercising the multi-target bundle + manifest.
TEST(XpuCudaDispatchDeviceTests, bundledNvptxCpuForcedToCpu) {
    setenv("CAJETA_XPU_BACKEND", "cpu", /*overwrite=*/1);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx, cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSaxpyHostSource, "test.Saxpy", o);
    auto fn = jit ? jit->lookup<float (*)()>("run") : nullptr;
    float result = fn ? fn() : 0.0f;
    unsetenv("CAJETA_XPU_BACKEND");

    ASSERT_NE(jit, nullptr);
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(result, 4096.0f);   // ran on the CPU rung
}
