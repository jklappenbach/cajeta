//
// The feasibility filter (xpu-tile-shape-selection plan Unit 5, spec §3).
//
// Before a candidate shape is timed, it must pass rules computed from the
// device's facts and the compiler's verdict for that candidate:
//
//   R1 no spill; R2 the register file holds the workgroup; R3 shared memory
//   fits, with the opt-in limit; R4 at least one workgroup is resident, and
//   how many; R5 the grid fills the multiprocessors, or carries the split-K
//   that does; R6 the padding the shape costs at the problem.
//
// R1 to R4 prune; R5 and R6 are carried on the survivor. Every pruned
// candidate is named with its rule, and an empty survivor set is a named
// refusal, never a silent fallback.
//
// The facts below are an RTX 4090 (sm_89) as the driver reports it, and the
// candidates are the TcTile reshapes measured on 2026-10-02: today's 8-warp
// 128x128 tile at 255 registers, and the 16-warp 32x32-warp-tile reshape that
// held 128 registers with a 52-byte spill.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kHead = R"CJ(
package test;
import cajeta.lang.String;
import cajeta.xpu.DeviceFacts;
import cajeta.xpu.KernelFacts;
import cajeta.xpu.ShapeCandidate;
import cajeta.xpu.ShapeFilter;
import cajeta.xpu.ShapeVerdict;
public class M {
    // sm_89 as the CUDA driver reports it.
    static #DeviceFacts ada() {
        DeviceFacts d #= heap DeviceFacts();
        d.registersPerMultiprocessor = 65536L;
        d.sharedBytesPerBlock = 49152L;
        d.sharedBytesPerBlockOptIn = 101376L;
        d.sharedBytesPerMultiprocessor = 102400L;
        d.threadsPerMultiprocessor = 1536;
        d.maxBlocksPerMultiprocessor = 24;
        d.multiprocessorCount = 128;
        d.maxThreadsPerBlock = 1024;
        return d;
    }
)CJ";

const char* kTail = "}\n";

int run(const std::string& body, const char* entry = "run") {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(std::string(kHead) + body + kTail, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int32_t (*)()>(entry);
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

} // namespace

// 4.5.1.1: R1. The 16-warp reshape spills, so it is pruned without a run;
// today's tile survives at one resident workgroup.
TEST(XpuShapeFilter, aSpillingCandidateIsPrunedByR1) {
    EXPECT_EQ(run(R"CJ(
    public static int32 run() {
        ShapeFilter f #= heap ShapeFilter("q4kTile", M.ada(), 512L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("tile8w", 128, 128, 256, 256, 8),
                   #heap KernelFacts(255, 0, 34816));
        f.consider(#heap ShapeCandidate("tile16w", 128, 128, 512, 256, 8),
                   #heap KernelFacts(128, 52, 34816));
        if (f.survivorCount() != 1) { return 1; }
        if (f.prunedCount() != 1) { return 2; }
        ShapeVerdict p = f.pruned(0);
        if (p.rule != 1) { return 3; }
        if (!p.candidate.kernel.equals("tile16w")) { return 4; }
        ShapeVerdict s = f.survivor(0);
        if (s.residentGroups != 1) { return 10 + s.residentGroups; }
        return 0;
    }
)CJ"), 0);
}

// 4.5.1.2: R2 and R3. Threads times registers over the register file prunes,
// and shared memory over the opt-in limit prunes; at the opt-in limit fits.
TEST(XpuShapeFilter, theRegisterFileAndSharedMemoryPrune) {
    EXPECT_EQ(run(R"CJ(
    public static int32 run() {
        ShapeFilter f #= heap ShapeFilter("q4kTile", M.ada(), 512L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("regs", 128, 128, 512, 256, 8),
                   #heap KernelFacts(255, 0, 34816));
        f.consider(#heap ShapeCandidate("lds", 128, 128, 256, 256, 8),
                   #heap KernelFacts(128, 0, 110000));
        f.consider(#heap ShapeCandidate("ldsOptIn", 128, 128, 256, 256, 8),
                   #heap KernelFacts(128, 0, 101376));
        if (f.prunedCount() != 2) { return 1; }
        if (f.pruned(0).rule != 2) { return 2; }
        if (f.pruned(1).rule != 3) { return 3; }
        if (f.survivorCount() != 1) { return 4; }
        if (!f.survivor(0).candidate.kernel.equals("ldsOptIn")) { return 5; }
        return 0;
    }
)CJ"), 0);
}

// 4.5.1.3: R5. A grid short of the multiprocessors carries the split-K count
// TcTile.splitsFor gives today: 32 tiles over 128 SMs with 16 K blocks splits
// 4 ways; 128 tiles fill the part and do not split.
TEST(XpuShapeFilter, anUnderfilledGridCarriesTheSplitKCount) {
    EXPECT_EQ(run(R"CJ(
    public static int32 run() {
        ShapeFilter f #= heap ShapeFilter("q4kTile", M.ada(), 128L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("tile8w", 128, 128, 256, 256, 8),
                   #heap KernelFacts(255, 0, 34816));
        ShapeFilter g #= heap ShapeFilter("q4kTile", M.ada(), 512L, 4096L, 4096L);
        g.consider(#heap ShapeCandidate("tile8w", 128, 128, 256, 256, 8),
                   #heap KernelFacts(255, 0, 34816));
        return f.survivor(0).splits * 10 + g.survivor(0).splits;
    }
)CJ"), 41);
}

// R6: the padding a shape costs is stated. 100 x 4096 on a 128-row tile pads
// to 128 rows, so 28 of every 128 rows are waste: 218 per mille.
TEST(XpuShapeFilter, thePaddingAShapeCostsIsStated) {
    EXPECT_EQ(run(R"CJ(
    public static int32 run() {
        ShapeFilter f #= heap ShapeFilter("q4kTile", M.ada(), 100L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("tile8w", 128, 128, 256, 256, 8),
                   #heap KernelFacts(255, 0, 34816));
        return f.survivor(0).padWastePerMille;
    }
)CJ"), 218);
}

// 4.5.1.4: every pruned candidate is named with its rule, and an empty
// survivor set is a named refusal. With survivors there is no refusal.
TEST(XpuShapeFilter, anEmptySurvivorSetIsANamedRefusal) {
    EXPECT_EQ(run(R"CJ(
    public static int32 run() {
        ShapeFilter f #= heap ShapeFilter("q4kTile", M.ada(), 512L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("tile16w", 128, 128, 512, 256, 8),
                   #heap KernelFacts(128, 52, 34816));
        f.consider(#heap ShapeCandidate("lds", 128, 128, 256, 256, 8),
                   #heap KernelFacts(128, 0, 110000));
        String why #= f.refusal();
        if (why.indexOf("q4kTile") < 0) { return 1; }
        if (why.indexOf("tile16w") < 0) { return 2; }
        if (why.indexOf("R1") < 0) { return 3; }
        if (why.indexOf("lds") < 0) { return 4; }
        if (why.indexOf("R3") < 0) { return 5; }
        if (why.indexOf("512x4096x4096") < 0) { return 6; }
        ShapeFilter g #= heap ShapeFilter("q4kTile", M.ada(), 512L, 4096L, 4096L);
        g.consider(#heap ShapeCandidate("tile8w", 128, 128, 256, 256, 8),
                   #heap KernelFacts(255, 0, 34816));
        String none #= g.refusal();
        if (none.count() != 0L) { return 7; }
        return 0;
    }
)CJ"), 0);
}

// Absent is not zero: a backend that reports no register count (cpu) is not
// pruned by R2, and a kernel's measured facts read absent there.
TEST(XpuShapeFilter, anAbsentFactPrunesNothing) {
    EXPECT_EQ(run(R"CJ(
    public static int32 run() {
        ShapeFilter f #= heap ShapeFilter("q4kTile", M.ada(), 512L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("cpuTile", 128, 128, 1024, 256, 8),
                   #heap KernelFacts(-1, -1, -1));
        if (f.survivorCount() != 1) { return 1; }
        KernelFacts k #= KernelFacts.measured("noSuchKernel", 0);
        if (k.registersPerThread != -1) { return 2; }
        return 0;
    }
)CJ"), 0);
}

// The live facts: on the RTX 4090, DeviceFacts.current() reads the driver.
TEST(XpuShapeFilter, theCurrentDeviceFactsComeFromTheDriverOnNvptx) {
    CAJETA_SKIP_IF_NO_CUDA();
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(std::string(kHead) + R"CJ(
    public static int32 run() {
        DeviceFacts d #= DeviceFacts.current();
        if (d.multiprocessorCount <= 0) { return 1; }
        if (d.registersPerMultiprocessor != 65536L) { return 2; }
        if (d.sharedBytesPerBlockOptIn < d.sharedBytesPerBlock) { return 3; }
        if (d.threadsPerMultiprocessor <= 0) { return 4; }
        return 0;
    }
)CJ" + kTail, "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}
