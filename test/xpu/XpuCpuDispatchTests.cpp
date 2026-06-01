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
    "        float32[] hx = new float32[n];\n"
    "        float32[] hy = new float32[n];\n"
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
