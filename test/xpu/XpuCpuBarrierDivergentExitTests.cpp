//
// A work-item that LEAVES before a barrier, on the CPU backend.
//
// THE SHAPE. Twenty-seven of cajeta-llm's forty-four skipped kernels were
// declined by one of these two messages:
//
//     a barrier under work-item-divergent control flow
//     a barrier loop under work-item-divergent control flow
//
// and in every case the divergence was a guard at the very top of the kernel
// — `if (row >= nH + nKv) { return; }` in `qkNormRowsF32`, `if (t0 < rows &&
// i0 < outDim) { … }` around the whole body of the WMMA GEMMs. The guard is
// workgroup-uniform in fact (`row = globalIdX() / 256` with a 256-wide block
// IS the workgroup id) but the fission cannot prove it: `globalIdX()` is
// seeded from the tid placeholder, so everything downstream is tainted, and
// the block size is a launch parameter, not a constant.
//
// WHY NOT PROVE UNIFORMITY. It would need the block bound at lowering time,
// which means either an annotation on every one of these kernels or a
// constant baked into the pass. The first is per-kernel maintenance and the
// second is a hard-coded per-device number. Neither earns its place when the
// general answer is cheaper.
//
// THE GENERAL ANSWER. A work-item that leaves is simply inactive for the rest
// of the kernel. The fission already turns a `ret` inside a region into "end
// this work-item's iteration of the region loop" — what was missing is that
// the SAME work-item then ran every LATER region too, reading state it never
// initialized. So: one i8 per work-item, set at entry, cleared where a `ret`
// becomes a latch edge, and read as a guard at the top of every region. The
// barriers stay scaffold, executed once by the group, which is right — a
// barrier is a point in the program, not a rendezvous of a particular set of
// work-items.
//
// WHAT IT DOES NOT DO. A work-item that skips a barrier and comes BACK is
// still declined, by name. The mask expresses leaving, not re-joining, and
// the third test here holds that line: without it the relaxation would be
// "accept everything and hope", which is the miscompile this check was added
// to prevent in the first place (RAGreedy SIGSEGV, 2026-09-06).
//
// MEASUREMENT. Every accepted shape runs against a scalar reference on small
// exact integers (each partial stays under 2^24, so f32 summation order
// cannot move the result), and each one launches MORE workgroups than the
// guard admits, with the out-of-range outputs pre-filled with a sentinel. A
// mask that does not work does not merely give a wrong sum — it overwrites
// the sentinel, and the return code says which slot.
//
#include "XpuCpuBarrierFissionShapes.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using namespace cajeta_fission_shapes;

namespace {

std::unique_ptr<CajetaJit> compileCpu(const std::string& src, std::string* err) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    *err = testing::internal::GetCapturedStderr();
    return jit;
}

// Four workgroups launched, two admitted. in[i] = i mod 13; out is pre-filled
// with -1 so a workgroup that should have left announces itself.
//
//   bit 0   the admitted workgroups' sums disagree with the scalar reference
//   bit 1   workgroup 2 wrote, so its work-items were not masked out
//   bit 2   workgroup 3 wrote
//
// The two bits are separate on purpose: one of them firing alone would mean
// the mask is indexed wrong rather than absent.
std::string runGuarded(const char* kernelName) {
    return std::string(
        "    public static int32 run() {\n"
        "        int32 n = 1024;\n"
        "        float32[] hin = heap float32[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { hin[i] = (float32) (i - (i / 13) * 13); i = i + 1; }\n"
        "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) n);\n"
        "        KernelBuffer<float32> out = heap KernelBuffer<float32>(4);\n"
        "        in.upload(hin);\n"
        "        float32[] hout = heap float32[4];\n"
        "        int32 j = 0;\n"
        "        while (j < 4) { hout[j] = -1.0f; j = j + 1; }\n"
        "        out.upload(hout);\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        uint32 un = (uint32) n;\n"
        "        uint32 groups = 2;\n")
        + "        " + kernelName
        + ".launch(s, grid: [4], block: [256])(out, in, un, groups);\n"
        + "        s.sync();\n"
        "        out.download(hout);\n"
        "        float32 want = 0.0f;\n"
        "        int32 k = 0;\n"
        "        while (k < 512) { want = want + (float32) (k - (k / 13) * 13); k = k + 1; }\n"
        "        int32 code = 0;\n"
        "        if (hout[0] + hout[1] != want) { code = code + 1; }\n"
        "        if (hout[2] != -1.0f) { code = code + 2; }\n"
        "        if (hout[3] != -1.0f) { code = code + 4; }\n"
        "        return code;\n"
        "    }\n";
}

const char* RUN_DIVERGENT_LAUNCH =
    "    public static int32 run() {\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(256);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 f0 = Device.launchFailures();\n"
    "        uint32 n = 256;\n"
    "        divergentJoin.launch(s, grid: [1], block: [256])(out, in, n);\n"
    "        s.sync();\n"
    "        return (int32) (Device.launchFailures() - f0);\n"
    "    }\n";

} // namespace

// The WMMA GEMM guard: the whole body, barrier loop included, under one `if`.
TEST(XpuCpuBarrierDivergentExit, aGuardedBarrierLoopLowersAndMatches) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + GUARDED_LOOP
                          + runGuarded("treeGuarded") + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a guard the workgroup takes together is no longer a decline:\n"
        << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "bit 0 = wrong sum, bit 1 = workgroup 2 wrote, bit 2 = workgroup 3 "
           "wrote";
}

// `qkNormRowsF32`'s guard: an early `return` rather than an enclosing `if`.
TEST(XpuCpuBarrierDivergentExit, anEarlyReturnBeforeABarrierLowersAndMatches) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + EARLY_RETURN
                          + runGuarded("treeEarly") + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "bit 0 = wrong sum, bit 1 = workgroup 2 wrote, bit 2 = workgroup 3 "
           "wrote";
}

// The line the relaxation must not cross. A work-item that skips the barrier
// and REJOINS has no defined place to be masked back in, so the kernel is
// still declined, by name, and the launch still counts as a failure.
TEST(XpuCpuBarrierDivergentExit, aBarrierInOneArmOfAJoinIsStillDeclined) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + DIVERGENT_JOIN
                          + RUN_DIVERGENT_LAUNCH + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_NE(err.find("[xpu-kernel-skipped] divergentJoin"), std::string::npos)
        << "the note must name the kernel:\n" << err;
    EXPECT_NE(err.find("barrier fission"), std::string::npos) << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    testing::internal::CaptureStderr();
    int32_t delta = fn();
    std::string runErr = testing::internal::GetCapturedStderr();
    EXPECT_EQ(delta, 1)
        << "one declined launch moves Device.launchFailures() by one:\n"
        << runErr;
}
