// The CPU backend's barrier fission declines some barrier shapes and falls
// back to a host stub: the kernel gets NO CPU code, its launch prints one
// runtime line and returns, and every output reads back whatever it held.
// That fallback used to be SILENT at build time — the neighbouring XPU-N01
// path printed `[xpu-kernel-skipped]`, the fission catch did not — and three
// baseline kernels of the xpu-tile family (a stride-loop reduce, a tiled
// GEMM, a two-array final reduce) read as sub-microsecond "kernels" for a
// whole leg until the runtime's failure counter was checked
// (xpu-tile-scheduling Unit 0, 2026-09-06).
//
// Those three kernels are accepted since cpu-barrier-fission-loops Unit 1
// (a uniform latch after the loop's last barrier is scaffold); the declined
// example here is the shape that unit declines by design — per-work-item
// code in that latch (plan 1.1.5). Tests in both directions: the note FIRES
// for the declined shape and names the fission as the reason, and it does
// NOT fire for the shapes the fission accepts.
#include "XpuCpuBarrierFissionShapes.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using namespace cajeta_fission_shapes;

namespace {

const char* RUN =
    "    public static int32 run() { return 1; }\n";

// Launch the declined kernel and return how far Device.launchFailures() moved.
const char* RUN_LAUNCH =
    "    public static int32 run() {\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(4);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 f0 = Device.launchFailures();\n"
    "        uint32 n = 256;\n"
    "        treeLatch.launch(s, grid: [1], block: [256])(out, in, n);\n"
    "        s.sync();\n"
    "        return (int32) (Device.launchFailures() - f0);\n"
    "    }\n";

} // namespace

TEST(XpuCpuBarrierFissionNote, declinedShapeIsNamedAtBuildTime) {
    std::string err = compileCpuCapturingStderr(std::string(PRE) + TAINTED_LATCH + RUN + END);
    EXPECT_NE(err.find("[xpu-kernel-skipped] treeLatch"), std::string::npos)
        << "per-work-item code in a barrier loop's latch is declined by the CPU fission; "
           "the build must SAY so:\n" << err;
    EXPECT_NE(err.find("barrier fission"), std::string::npos)
        << "the note must name the fission as the reason:\n" << err;
}

TEST(XpuCpuBarrierFissionNote, acceptedShapeIsSilent) {
    std::string flat = compileCpuCapturingStderr(std::string(PRE) + FLAT_BARRIER + RUN + END);
    EXPECT_EQ(flat.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a straight-line barrier fissions on the CPU backend; no note expected:\n" << flat;
    std::string tree = compileCpuCapturingStderr(std::string(PRE) + TREE_REDUCE + RUN + END);
    EXPECT_EQ(tree.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a barrier loop whose latch is uniform fissions on the CPU backend; "
           "no note expected:\n" << tree;
}

// The runtime side of the same silence: a launch that finds no registered
// CPU kernel prints its line and returns, and Device.launchFailures() must
// COUNT it — it did not, so a harness asserting a zero delta saw zero.
TEST(XpuCpuBarrierFissionNote, declinedKernelLaunchCountsAsFailure) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(std::string(PRE) + TAINTED_LATCH + RUN_LAUNCH + END, "test.D", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int32_t delta = fn();
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(delta, 1) << "one declined launch must move Device.launchFailures() by one:\n" << err;
    EXPECT_NE(err.find("no registered CPU kernel 'treeLatch'"), std::string::npos) << err;
}
