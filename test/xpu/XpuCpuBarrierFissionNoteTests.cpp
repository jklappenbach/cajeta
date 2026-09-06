// The CPU backend's barrier fission declines some barrier shapes (a barrier
// inside a loop, today) and falls back to a host stub: the kernel gets NO
// CPU code, its launch prints one runtime line and returns, and every output
// reads back whatever it held. That fallback used to be SILENT at build
// time — the neighbouring XPU-N01 path printed `[xpu-kernel-skipped]`, the
// fission catch did not — and three baseline kernels of the xpu-tile family
// (a stride-loop reduce, a tiled GEMM, a two-array final reduce) read as
// sub-microsecond "kernels" for a whole leg until the runtime's failure
// counter was checked (xpu-tile-scheduling Unit 0, 2026-09-06).
//
// Two tests, one per direction: the note FIRES for the declined shape and
// names the fission as the reason, and it does NOT fire for the straight-line
// barrier the fission accepts.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string compileCpuCapturingStderr(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    std::string err = testing::internal::GetCapturedStderr();
    (void) jit;
    return err;
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.Workgroup;\n"
    "public final class D {\n";

// A barrier inside a uniform stride loop: the classic LDS tree reduce.
const char* LOOP_BARRIER =
    "    @Kernel\n"
    "    public static void tree(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        Barrier.workgroup();\n"
    "        uint32 stride = 128;\n"
    "        while (stride > 0) {\n"
    "            if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "            Barrier.workgroup();\n"
    "            stride = stride / 2;\n"
    "        }\n"
    "        if (t == 0) { out[Workgroup.x()] = lds[0]; }\n"
    "    }\n";

// One straight-line barrier: the shape the fission accepts.
const char* FLAT_BARRIER =
    "    @Kernel\n"
    "    public static void flat(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        Barrier.workgroup();\n"
    "        if (t == 0) { out[Workgroup.x()] = lds[0] + lds[1]; }\n"
    "    }\n";

const char* RUN =
    "    public static int32 run() { return 1; }\n"
    "}\n";

// Launch the declined kernel and return how far Device.launchFailures() moved.
const char* RUN_LAUNCH =
    "    public static int32 run() {\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(4);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 f0 = Device.launchFailures();\n"
    "        uint32 n = 256;\n"
    "        tree.launch(s, grid: [1], block: [256])(out, in, n);\n"
    "        s.sync();\n"
    "        return (int32) (Device.launchFailures() - f0);\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuCpuBarrierFissionNote, declinedShapeIsNamedAtBuildTime) {
    std::string err = compileCpuCapturingStderr(std::string(PRE) + LOOP_BARRIER + RUN);
    EXPECT_NE(err.find("[xpu-kernel-skipped] tree"), std::string::npos)
        << "a barrier in a loop is declined by the CPU fission; the build must SAY so:\n" << err;
    EXPECT_NE(err.find("barrier fission"), std::string::npos)
        << "the note must name the fission as the reason:\n" << err;
}

TEST(XpuCpuBarrierFissionNote, acceptedShapeIsSilent) {
    std::string err = compileCpuCapturingStderr(std::string(PRE) + FLAT_BARRIER + RUN);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a straight-line barrier fissions on the CPU backend; no note expected:\n" << err;
}

// The runtime side of the same silence: a launch that finds no registered
// CPU kernel prints its line and returns, and Device.launchFailures() must
// COUNT it — it did not, so a harness asserting a zero delta saw zero.
TEST(XpuCpuBarrierFissionNote, declinedKernelLaunchCountsAsFailure) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(std::string(PRE) + LOOP_BARRIER + RUN_LAUNCH, "test.D", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int32_t delta = fn();
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(delta, 1) << "one declined launch must move Device.launchFailures() by one:\n" << err;
    EXPECT_NE(err.find("no registered CPU kernel 'tree'"), std::string::npos) << err;
}
