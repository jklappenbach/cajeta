//
// The kernel census (xpu-kernel-adaptor Unit 6): which kernels are REGISTERED
// for the active backend, and which of them have actually RUN.
//
// A suite's pass count cannot answer the second question. A route that picks a
// different variant, a launch that is refused, or a test that skips its device
// arm all leave the count untouched, and every kernel they did not reach reads
// as covered. So the runtime keeps one number per kernel name, bumped at the
// single seam every launch passes through when the failure counter did not
// move, and enumerates the registry the launch consults. Device exposes both,
// and a suite closes by checking every registered kernel against them.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options withBackend(cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    return o;
}

// Two kernels; the source launches one of them, twice, and never the other.
const char* kCensusSource =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Census {\n"
    "    @Kernel\n"
    "    public static void censusRan(KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = y[i] + 1.0f; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void censusNeverRan(KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = 0.0f; }\n"
    "    }\n"
    "    public static int64 run() {\n"
    "        uint32 n = 64;\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        y.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        censusRan.launch(s, grid: [1], block: [64])(y, n);\n"
    "        censusRan.launch(s, grid: [1], block: [64])(y, n);\n"
    "        s.sync();\n"
    "        y.free();\n"
    "        return Device.kernelLaunchCount(\"censusRan\");\n"
    "    }\n"
    "    public static int64 neverRan() { return Device.kernelLaunchCount(\"censusNeverRan\"); }\n"
    "    public static int64 unknown() { return Device.kernelLaunchCount(\"noSuchKernelAnywhere\"); }\n"
    "    public static int32 registered() { return Device.registeredKernelCount(); }\n"
    "    /** Bit 1: censusRan is enumerated; bit 2: censusNeverRan is; bit 4: no empty names. */\n"
    "    public static int32 enumerates() {\n"
    "        int32 bits = 4;\n"
    "        int32 n = Device.registeredKernelCount();\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            String name #= Device.registeredKernel(i);\n"
    "            if (name.count() == 0L) { bits = bits & 3; }\n"
    "            if (name.equals(\"censusRan\")) { bits = bits | 1; }\n"
    "            if (name.equals(\"censusNeverRan\")) { bits = bits | 2; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return bits;\n"
    "    }\n"
    "    public static int64 outOfRange() { String s #= Device.registeredKernel(100000); return s.count(); }\n"
    "}\n";

void checkCensus(cajeta::xpu::Backend backend, const char* label) {
    auto jit = CajetaJit::compile(kCensusSource, "test.Census", withBackend(backend));
    ASSERT_NE(jit, nullptr);
    auto run        = jit->lookup<int64_t (*)()>("run");
    auto neverRan   = jit->lookup<int64_t (*)()>("neverRan");
    auto unknown    = jit->lookup<int64_t (*)()>("unknown");
    auto registered = jit->lookup<int32_t (*)()>("registered");
    auto enumerates = jit->lookup<int32_t (*)()>("enumerates");
    auto outOfRange = jit->lookup<int64_t (*)()>("outOfRange");
    ASSERT_TRUE(run && neverRan && unknown && registered && enumerates && outOfRange);

    const int64_t ran = run();
    std::printf(" RESULT u6_%s_launch_count=%lld registered=%d\n", label,
                (long long) ran, (int) registered());
    EXPECT_EQ(ran, 2) << "two launches of censusRan were not counted as two";
    EXPECT_EQ(neverRan(), 0) << "a kernel that never launched has a count";
    EXPECT_EQ(unknown(), 0) << "a name that is not a kernel has a count";
    EXPECT_GE(registered(), 2) << "the registry enumerates fewer kernels than this source declares";
    const int32_t bits = enumerates();
    EXPECT_TRUE(bits & 1) << "censusRan is registered (it launched) but not enumerated";
    EXPECT_TRUE(bits & 2) << "censusNeverRan is registered but not enumerated — the census "
                             "would never see the kernels nobody launches, which are its point";
    EXPECT_TRUE(bits & 4) << "an enumerated name was empty";
    EXPECT_EQ(outOfRange(), 0) << "an out-of-range index did not answer an empty name";
}

} // namespace

TEST(XpuKernelCensus, cpuCountsLaunchesAndEnumeratesTheRegistry) {
    checkCensus(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuKernelCensus, nvptxCountsLaunchesAndEnumeratesTheRegistry) {
    CAJETA_SKIP_IF_NO_CUDA();
    checkCensus(cajeta::xpu::Backend::Nvptx, "nvptx");
}

// A refused launch is not a launch: the count must not move for it. The
// device-only refusal path is the same on every backend (no registered code),
// so the CPU backend is enough to pin it, and it is what every CI leg has.
TEST(XpuKernelCensus, aRefusedLaunchIsNotCounted) {
    auto jit = CajetaJit::compile(kCensusSource, "test.Census",
                                  withBackend(cajeta::xpu::Backend::Cpu));
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    ASSERT_EQ(run(), 2);
    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    auto launch   = reinterpret_cast<void (*)(const char*, int32_t, int32_t, int32_t,
                                              int32_t, int32_t, int32_t, uint32_t,
                                              void*, int64_t)>(sym("__cajeta_xpu_launch"));
    auto count    = reinterpret_cast<int64_t (*)(void*, int64_t)>(sym("__cajeta_xpu_kernel_launch_count"));
    auto failures = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_xpu_launch_failures"));
    ASSERT_TRUE(launch && count && failures);
    // An int8[] payload starts at +8: fake the header.
    char arr[8 + 32] = {0};
    const char* name = "censusNeverRan";
    std::snprintf(arr + 8, 32, "%s", name);
    const int64_t before = failures();
    void* argv[2] = {nullptr, nullptr};
    launch("kernelThatWasNeverRegistered", 1, 1, 1, 1, 1, 1, 0, argv, 0);
    EXPECT_GT(failures(), before) << "launching an unregistered kernel was not recorded as a failure";
    std::snprintf(arr + 8, 32, "%s", "kernelThatWasNeverRegistered");
    EXPECT_EQ(count(arr, (int64_t) std::string("kernelThatWasNeverRegistered").size()), 0)
        << "a refused launch was counted as a run";
    std::snprintf(arr + 8, 32, "%s", name);
    EXPECT_EQ(count(arr, (int64_t) std::string(name).size()), 0);
}
