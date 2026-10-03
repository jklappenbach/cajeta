//
// The measured choice and its cache (xpu-tile-shape-selection plan Unit 6,
// spec §4 and §7.12).
//
// The feasibility filter's survivors are timed at first use, interleaved, and
// the winner is cached per (device, family, exact M x N x K), keyed by the
// build of the candidates, so a rebuild that changes one re-measures. The
// entry records the winner's margin, and a lead under 3% is a tie. An
// override forces a shape for a sweep.
//
// The probe here is a fake with fixed times per candidate, so what is under
// test is the choosing, not the device: the family supplies the real probe.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kHead = R"CJ(
package test;
import cajeta.lang.String;
import cajeta.xpu.DeviceFacts;
import cajeta.xpu.KernelFacts;
import cajeta.xpu.ShapeCandidate;
import cajeta.xpu.ShapeChoice;
import cajeta.xpu.ShapeFilter;
import cajeta.xpu.ShapeProbe;
import cajeta.xpu.ShapeResult;
public final class Fake implements ShapeProbe {
    int64[] times;
    public int32[] order;
    public int32 calls;
    public Fake(int64 a, int64 b, int64 c) {
        this.times #= heap int64[3];
        this.times[0] = a;
        this.times[1] = b;
        this.times[2] = c;
        this.order #= heap int32[64];
        this.calls = 0;
    }
    public int64 timeNanos(int32 i) {
        if (this.calls < 64) { this.order[this.calls] = i; }
        this.calls = this.calls + 1;
        return this.times[i];
    }
}
public class M {
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
    // Three feasible candidates; `bRegs` stands in for a rebuild of "b".
    static #ShapeFilter three(int32 bRegs) {
        ShapeFilter f #= heap ShapeFilter("fam", M.ada(), 512L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("a", 128, 128, 256, 256, 8),
                   #heap KernelFacts(200, 0, 34816));
        f.consider(#heap ShapeCandidate("b", 128, 64, 256, 256, 8),
                   #heap KernelFacts(bRegs, 0, 34816));
        f.consider(#heap ShapeCandidate("c", 64, 128, 256, 256, 8),
                   #heap KernelFacts(160, 0, 34816));
        return f;
    }
)CJ";

int run(const std::string& body, const std::string& dir) {
    std::string program = std::string(kHead) +
        "    public static int32 run() {\n"
        "        String dir = \"" + dir + "\";\n" + body +
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -100;
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -101;
}

std::string freshDir() {
    static std::mt19937_64 rng(std::random_device{}());
    auto d = std::filesystem::temp_directory_path()
           / ("cajeta-shapechoice-" + std::to_string(rng()));
    std::filesystem::create_directories(d);
    return d.generic_string();
}

} // namespace

// 4.6.1.1: first use times every survivor, interleaved, and caches the
// winner; a second use reads the cache and times nothing.
TEST(XpuShapeChoice, firstUseTimesInterleavedAndCachesTheWinner) {
    EXPECT_EQ(run(R"CJ(
        ShapeFilter f #= M.three(128);
        Fake p #= heap Fake(300L, 280L, 307L);
        ShapeResult r #= ShapeChoice.choose(dir, f, p);
        if (!r.timed || r.fromCache) { return 1; }
        if (!r.kernel.equals("b")) { return 2; }
        if (p.calls < 9) { return 3; }
        // Interleaved: each round visits every candidate before any repeats,
        // and rounds start on different candidates.
        if (p.order[0] == p.order[1] || p.order[1] == p.order[2]
                || p.order[0] == p.order[2]) { return 4; }
        if (p.order[3] == p.order[0]) { return 5; }
        Fake q #= heap Fake(1L, 1L, 1L);
        ShapeResult s #= ShapeChoice.choose(dir, f, q);
        if (q.calls != 0) { return 6; }
        if (!s.fromCache || !s.kernel.equals("b")) { return 7; }
        return 0;
)CJ", freshDir()), 0);
}

// 4.6.1.6: the entry records the winner's margin over the runner-up, per
// mille, and a lead under 3% is a tie. The cached answer keeps both.
TEST(XpuShapeChoice, aLeadUnderThreePercentIsATie) {
    EXPECT_EQ(run(R"CJ(
        ShapeFilter f #= M.three(128);
        Fake p #= heap Fake(100L, 98L, 200L);
        ShapeResult r #= ShapeChoice.choose(dir, f, p);
        if (r.marginPerMille != 20) { return 1; }
        if (!r.tie) { return 2; }
        Fake q #= heap Fake(1L, 1L, 1L);
        ShapeResult s #= ShapeChoice.choose(dir, f, q);
        if (!s.fromCache || !s.tie || s.marginPerMille != 20) { return 3; }
        ShapeFilter g #= heap ShapeFilter("fam2", M.ada(), 512L, 4096L, 4096L);
        g.consider(#heap ShapeCandidate("a", 128, 128, 256, 256, 8),
                   #heap KernelFacts(200, 0, 34816));
        g.consider(#heap ShapeCandidate("b", 128, 64, 256, 256, 8),
                   #heap KernelFacts(128, 0, 34816));
        Fake w #= heap Fake(100L, 80L, 0L);
        ShapeResult t #= ShapeChoice.choose(dir, g, w);
        if (t.marginPerMille != 200 || t.tie) { return 4; }
        return 0;
)CJ", freshDir()), 0);
}

// 4.6.1.2: a rebuild that changes a candidate changes the build the entry was
// measured against, so the next use times again.
TEST(XpuShapeChoice, aRebuiltCandidateInvalidatesTheEntry) {
    testing::internal::CaptureStderr();
    int rc = run(R"CJ(
        ShapeFilter f #= M.three(128);
        Fake p #= heap Fake(300L, 280L, 307L);
        ShapeResult r #= ShapeChoice.choose(dir, f, p);
        ShapeFilter g #= M.three(136);
        Fake q #= heap Fake(300L, 400L, 307L);
        ShapeResult s #= ShapeChoice.choose(dir, g, q);
        if (s.fromCache || q.calls == 0) { return 1; }
        if (!s.kernel.equals("a")) { return 2; }
        return 0;
)CJ", freshDir());
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 0);
}

// 4.6.1.3: an override forces a shape and times nothing; clearing it restores
// the cached winner.
TEST(XpuShapeChoice, anOverrideForcesAShapeUntilCleared) {
    EXPECT_EQ(run(R"CJ(
        ShapeFilter f #= M.three(128);
        Fake p #= heap Fake(300L, 280L, 307L);
        ShapeResult r #= ShapeChoice.choose(dir, f, p);
        ShapeChoice.setShapeOverride("fam", "c");
        Fake q #= heap Fake(1L, 1L, 1L);
        ShapeResult s #= ShapeChoice.choose(dir, f, q);
        if (!s.overridden || !s.kernel.equals("c") || q.calls != 0) { return 1; }
        ShapeChoice.clearShapeOverride("fam");
        ShapeResult t #= ShapeChoice.choose(dir, f, q);
        if (t.overridden || !t.fromCache || !t.kernel.equals("b")) { return 2; }
        return 0;
)CJ", freshDir()), 0);
}

// 4.6.1.4: prewarm fills the cache ahead of first use.
TEST(XpuShapeChoice, prewarmFillsTheCacheAheadOfFirstUse) {
    EXPECT_EQ(run(R"CJ(
        ShapeFilter f #= M.three(128);
        Fake p #= heap Fake(300L, 280L, 307L);
        ShapeChoice.prewarm(dir, f, p);
        if (p.calls == 0) { return 1; }
        Fake q #= heap Fake(1L, 1L, 1L);
        ShapeResult s #= ShapeChoice.choose(dir, f, q);
        if (!s.fromCache || q.calls != 0 || !s.kernel.equals("b")) { return 2; }
        return 0;
)CJ", freshDir()), 0);
}

// No survivor is a named refusal, and one survivor needs no timing.
TEST(XpuShapeChoice, noSurvivorRefusesAndOneSurvivorIsNotTimed) {
    EXPECT_EQ(run(R"CJ(
        ShapeFilter f #= heap ShapeFilter("fam", M.ada(), 512L, 4096L, 4096L);
        f.consider(#heap ShapeCandidate("spills", 128, 128, 512, 256, 8),
                   #heap KernelFacts(128, 52, 34816));
        Fake p #= heap Fake(1L, 1L, 1L);
        ShapeResult r #= ShapeChoice.choose(dir, f, p);
        if (r.survivor != -1 || r.refusal.indexOf("R1") < 0) { return 1; }
        ShapeFilter g #= heap ShapeFilter("one", M.ada(), 512L, 4096L, 4096L);
        g.consider(#heap ShapeCandidate("only", 128, 128, 256, 256, 8),
                   #heap KernelFacts(200, 0, 34816));
        ShapeResult s #= ShapeChoice.choose(dir, g, p);
        if (s.survivor != 0 || s.timed || p.calls != 0) { return 2; }
        return 0;
)CJ", freshDir()), 0);
}
