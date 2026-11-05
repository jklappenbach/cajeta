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

namespace {

// xpu-kernel-targets Unit 3: a GPU-only kernel beside a plain one, on the cpu backend.
const char* kOffTargetSource =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class Off {\n"
    "    @Kernel\n"
    "    public static void here(KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = 2.0f; }\n"
    "    }\n"
    "    @Kernel(targets = {\"gpu\"})\n"
    "    public static void gpuOnly(KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = 3.0f; }\n"
    "    }\n"
    "    /** Bit 1: `here` is available; bit 2: `gpuOnly` is. */\n"
    "    public static int32 available() {\n"
    "        int32 bits = 0;\n"
    "        if (Device.kernelAvailable(\"here\")) { bits = bits | 1; }\n"
    "        if (Device.kernelAvailable(\"gpuOnly\")) { bits = bits | 2; }\n"
    "        return bits;\n"
    "    }\n"
    "    /** Bit 1: `gpuOnly` is not a target; bit 2: `here` has no reason; bit 4: an\n"
    "     *  unknown name has no device code. */\n"
    "    public static int32 reasons() {\n"
    "        int32 bits = 0;\n"
    "        String off #= Device.kernelUnavailableReason(\"gpuOnly\");\n"
    "        String on #= Device.kernelUnavailableReason(\"here\");\n"
    "        String none #= Device.kernelUnavailableReason(\"noSuchKernelAnywhere\");\n"
    "        if (off.equals(\"not a target\")) { bits = bits | 1; }\n"
    "        if (on.equals(\"\")) { bits = bits | 2; }\n"
    "        if (none.equals(\"no usable device code\")) { bits = bits | 4; }\n"
    "        return bits;\n"
    "    }\n"
    "    /** 1 when the launch threw, plus 2/4/8 for the kernel, the backend and the\n"
    "     *  reason in its message; 100 + y[0] when it ran. */\n"
    "    public static int32 launchOff() {\n"
    "        uint32 n = 64;\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        y.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 r = 0;\n"
    "        try {\n"
    "            gpuOnly.launch(s, grid: [1], block: [64])(y, n);\n"
    "            s.sync();\n"
    "            float32[] h = heap float32[64];\n"
    "            y.download(h);\n"
    "            r = 100 + (int32) h[0];\n"
    "        } catch (XpuLaunchException e) {\n"
    "            String m = e.getMessage();\n"
    "            r = 1;\n"
    "            if (m.contains(\"'gpuOnly'\")) { r = r + 2; }\n"
    "            if (m.contains(\"'cpu'\")) { r = r + 4; }\n"
    "            if (m.contains(\"not a target\")) { r = r + 8; }\n"
    "        }\n"
    "        y.free();\n"
    "        return r;\n"
    "    }\n"
    "    /** Bit 1: `gpuOnly` is listed off target; bit 2: `here` is not; bit 4: an\n"
    "     *  out-of-range index answers an empty name. */\n"
    "    public static int32 listed() {\n"
    "        int32 bits = 0;\n"
    "        int32 n = Device.offTargetKernelCount();\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            String name #= Device.offTargetKernel(i);\n"
    "            if (name.equals(\"gpuOnly\")) { bits = bits | 1; }\n"
    "            if (name.equals(\"here\")) { bits = bits | 8; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        if ((bits & 8) == 0) { bits = bits | 2; }\n"
    "        String past #= Device.offTargetKernel(100000);\n"
    "        if (past.count() == 0L) { bits = bits | 4; }\n"
    "        return bits & 7;\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> offTargetOnCpu() {
    return CajetaJit::compile(kOffTargetSource, "test.Off", withBackend(cajeta::xpu::Backend::Cpu));
}

} // namespace

// xpu-kernel-targets 3.1.1 (spec 3.1): an off-target kernel is unavailable, and the reason
// says it is not a target, apart from a name with no device code.
TEST(XpuKernelTargetsRuntime, anOffTargetKernelIsUnavailableBecauseItIsNotATarget) {
    auto jit = offTargetOnCpu();
    ASSERT_NE(jit, nullptr);
    auto available = jit->lookup<int32_t (*)()>("available");
    auto reasons = jit->lookup<int32_t (*)()>("reasons");
    ASSERT_TRUE(available && reasons);
    EXPECT_EQ(available(), 1);
    EXPECT_EQ(reasons(), 1 | 2 | 4);
}

// 3.1.2 (spec 3.2): launching it throws, naming the kernel, the backend and the reason.
TEST(XpuKernelTargetsRuntime, launchingAnOffTargetKernelThrowsNamingIt) {
    auto jit = offTargetOnCpu();
    ASSERT_NE(jit, nullptr);
    auto launchOff = jit->lookup<int32_t (*)()>("launchOff");
    ASSERT_NE(launchOff, nullptr);
    EXPECT_EQ(launchOff(), 1 + 2 + 4 + 8);
}

// 3.1.3 (spec 3.3): the build's off-target kernels are listed for the active backend.
TEST(XpuKernelTargetsRuntime, theOffTargetKernelsAreListed) {
    auto jit = offTargetOnCpu();
    ASSERT_NE(jit, nullptr);
    auto listed = jit->lookup<int32_t (*)()>("listed");
    ASSERT_NE(listed, nullptr);
    EXPECT_EQ(listed(), 1 | 2 | 4);
}
