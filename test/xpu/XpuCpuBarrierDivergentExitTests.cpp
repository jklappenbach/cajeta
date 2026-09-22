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
    // Declined: the launch RAISES; catch it so run() returns the delta (the
    // failure counter is bumped before the throw, so the delta stays 1).
    "        try {\n"
    "        divergentJoin.launch(s, grid: [1], block: [256])(out, in, n);\n"
    "        s.sync();\n"
    "        } catch (XpuLaunchException e) { }\n"
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

// --- the uniform guard -----------------------------------------------------
//
// The mask handles a work-item that LEAVES. It cannot handle `qkPrepKernel`'s
// `if (norm != 0) { …barriers… }` with the RoPE loop after the join, because
// nobody leaves — every work-item takes the same side and then carries on.
//
// What makes that shape safe is not the mask but the CONDITION: `norm` is a
// kernel parameter, so it is not in the taint set, so the whole workgroup
// agrees. A branch the workgroup agrees on can be lifted out of the work-item
// loops and run once, exactly as a barrier loop's header and latch already
// are. That is the only thing being claimed here — uniformity by provenance,
// not by a block size the pass cannot see.

namespace {

// flag = 1 runs the reduce, flag = 0 skips it; the tail runs either way, so
// out[0] is the block sum + 1 or just 1. A scaffolded branch that got hoisted
// wrongly shows up as the wrong arm's answer, not as noise.
std::string runUniform(const char* kernelName, int flag, int wantScaled) {
    std::string s =
        "    public static int32 run() {\n"
        "        int32 n = 256;\n"
        "        float32[] hin = heap float32[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { hin[i] = (float32) (i - (i / 13) * 13); i = i + 1; }\n"
        "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) n);\n"
        "        KernelBuffer<float32> out = heap KernelBuffer<float32>(1);\n"
        "        in.upload(hin);\n"
        "        float32[] hout = heap float32[1];\n"
        "        hout[0] = -1.0f;\n"
        "        out.upload(hout);\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        uint32 un = (uint32) n;\n";
    s += "        uint32 fl = " + std::to_string(flag) + ";\n";
    s += std::string("        ") + kernelName
       + ".launch(s, grid: [1], block: [256])(out, in, un, fl);\n"
         "        s.sync();\n"
         "        out.download(hout);\n"
         "        float32 want = 0.0f;\n"
         "        int32 k = 0;\n"
         "        while (k < 256) { want = want + (float32) (k - (k / 13) * 13); k = k + 1; }\n";
    s += std::string("        if (") + (wantScaled ? "1" : "0") + " == 0) { want = 0.0f; }\n";
    s += "        return hout[0] == want + 1.0f ? 0 : 1;\n"
         "    }\n";
    return s;
}

const char* RUN_SPLIT =
    "    public static int32 run() {\n"
    "        int32 n = 256;\n"
    "        float32[] hin = heap float32[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { hin[i] = (float32) (i - (i / 13) * 13); i = i + 1; }\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) n);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(1);\n"
    "        in.upload(hin);\n"
    "        float32[] hout = heap float32[1];\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        uint32 un = (uint32) n;\n"
    "        float32 want = 0.0f;\n"
    "        int32 k = 0;\n"
    "        while (k < 256) { want = want + (float32) (k - (k / 13) * 13); k = k + 1; }\n"
    "        int32 code = 0;\n"
    "        hout[0] = -1.0f; out.upload(hout);\n"
    "        treeSplit.launch(s, grid: [1], block: [256])(out, in, un, 1);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != want) { code = code + 1; }\n"
    "        hout[0] = -1.0f; out.upload(hout);\n"
    "        treeSplit.launch(s, grid: [1], block: [256])(out, in, un, 0);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        if (hout[0] != -2.0f) { code = code + 2; }\n"
    "        return code;\n"
    "    }\n";

} // namespace

TEST(XpuCpuBarrierDivergentExit, aUniformGuardOverABarrierIsScaffoldNotADecline) {
    for (int flag = 0; flag <= 1; ++flag) {
        std::string err;
        auto jit = compileCpu(std::string(PRE) + UNIFORM_GUARD_JOIN
                              + runUniform("treeUniform", flag, flag) + END,
                              &err);
        ASSERT_NE(jit, nullptr) << err;
        EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
            << "flag=" << flag
            << ": a kernel-parameter guard is workgroup-uniform by "
               "provenance:\n" << err;
        auto fn = jit->lookup<int32_t (*)()>("run");
        ASSERT_NE(fn, nullptr);
        EXPECT_EQ(fn(), 0) << "flag=" << flag << ": wrong arm's answer";
    }
}

// No join block exists: both arms end the kernel. The split has to stop
// cleanly rather than hunt for a post-dominator that is the function exit.
TEST(XpuCpuBarrierDivergentExit, aUniformGuardWhoseArmsBothEndTheKernelLowers) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + UNIFORM_GUARD_BOTH_RETURN
                          + RUN_SPLIT + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "bit 0 = the reduce arm's sum is wrong, bit 1 = the other arm did "
           "not run";
}

// The full iq3xxsQ8WaveGateUpGluKernel shape: an outer uniform guard, a
// short-circuit `&&` inside its taken arm (two branches, so a split whose
// join lives inside another split's arm), a barrier chain under each, and
// both sides ending the kernel. Two workgroups, one per side.
TEST(XpuCpuBarrierDivergentExit, nestedUniformGuardsOverTwoBarrierChainsLower) {
    const char* runSrc =
        "    public static int32 run() {\n"
        "        int32 n = 512;\n"
        "        float32[] hin = heap float32[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { hin[i] = (float32) (i - (i / 13) * 13); i = i + 1; }\n"
        "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) n);\n"
        "        KernelBuffer<float32> out = heap KernelBuffer<float32>(2);\n"
        "        in.upload(hin);\n"
        "        float32[] hout = heap float32[2];\n"
        "        hout[0] = -1.0f; hout[1] = -1.0f;\n"
        "        out.upload(hout);\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        uint32 un = (uint32) n;\n"
        "        treeNested.launch(s, grid: [2], block: [256])(out, in, un, 1, 1);\n"
        "        s.sync();\n"
        "        out.download(hout);\n"
        "        float32 w0 = 0.0f;\n"
        "        int32 k = 0;\n"
        "        while (k < 256) { w0 = w0 + (float32) (k - (k / 13) * 13); k = k + 1; }\n"
        "        float32 w1 = 0.0f;\n"
        "        while (k < 512) { w1 = w1 + (float32) (k - (k / 13) * 13); k = k + 1; }\n"
        "        int32 code = 0;\n"
        "        if (hout[0] != w0) { code = code + 1; }\n"
        "        if (hout[1] != w1) { code = code + 2; }\n"
        "        return code;\n"
        "    }\n";
    std::string err;
    auto jit = compileCpu(std::string(PRE) + UNIFORM_GUARD_NESTED + runSrc + END,
                          &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "bit 0 = workgroup 0's chain (the flagB side), bit 1 = workgroup 1's"
           " chain (the && side)";
}

// --- the staged-epilogue probe (4A.5.4.C) ----------------------------------
//
// Asks whether a compiler-emitted LDS staging + barrier, at the position a
// `WaveVector` lowering would put one, survives fission in the control flow
// cajeta-llm's S-verb kernels actually use. See STAGED_EPILOGUE's comment
// for why hand-written cajeta answers this: the fission sees the same call
// whoever emitted it.

namespace {

// in[i] = (i mod 7) + 1. Every wave stages the same sixteen values per
// iteration and sums them, so all 256 work-items reach the same accumulator
// and out[0] is 256x the sum of the first 128 inputs. A mis-sliced staging
// gives a different number rather than a slower one.
const char* RUN_STAGED =
    "    public static int32 run() {\n"
    "        int32 n = 2048;\n"
    "        float32[] hin = heap float32[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { hin[i] = (float32) (i - (i / 7) * 7) + 1.0f;"
    " i = i + 1; }\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) n);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(1);\n"
    "        in.upload(hin);\n"
    "        float32[] hout = heap float32[1];\n"
    "        hout[0] = -1.0f;\n"
    "        out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        stagedEpi.launch(s, grid: [1], block: [256])(out, in, 1024, 1024);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        float32 acc = 0.0f;\n"
    "        int32 k = 0;\n"
    "        while (k < 128) { acc = acc + hin[k]; k = k + 1; }\n"
    "        return hout[0] == acc * 256.0f ? 0 : 1;\n"
    "    }\n";

const char* RUN_STAGED_DIV =
    "    public static int32 run() {\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>(64);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(1);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 f0 = Device.launchFailures();\n"
    // If stagedDiv is declined on a tier, its launch RAISES; catch it so run()
    // returns the delta. Harmless when it is accepted (no throw, delta 0).
    "        try {\n"
    "        stagedDiv.launch(s, grid: [1], block: [256])(out, in, 1024, 1024);\n"
    "        s.sync();\n"
    "        } catch (XpuLaunchException e) { }\n"
    "        return (int32) (Device.launchFailures() - f0);\n"
    "    }\n";

} // namespace

// THE ANSWER. If this lowers and computes, a WaveVector lowering may stage
// through LDS on a backend with no usable wave, and option C dominates
// option A. If it declines, it does not and the 14 CPU skips need A or a
// distributed CPU tile.
TEST(XpuCpuBarrierDivergentExit, aStagedEpilogueBarrierSurvivesTheRealShape) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + STAGED_EPILOGUE + RUN_STAGED + END,
                          &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a barrier where a staging lowering would emit one, in the control "
           "flow q80WmmaDeqMw8Kernel actually uses:\n" << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0) << "lowered, but the staged sum is wrong";
}

// The line the answer depends on: staging under a divergent guard that
// REJOINS is still declined, so the result above is about this shape and not
// about the fission having stopped checking.
TEST(XpuCpuBarrierDivergentExit, aStagedBarrierUnderADivergentJoinIsDeclined) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + STAGED_EPILOGUE_DIVERGENT
                          + RUN_STAGED_DIV + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_NE(err.find("[xpu-kernel-skipped] stagedDiv"), std::string::npos)
        << "a work-item that skips a barrier and rejoins has no point at "
           "which the group is together:\n" << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1) << "the declined launch must count as a failure";
}
