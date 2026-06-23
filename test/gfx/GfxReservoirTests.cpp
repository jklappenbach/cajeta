//
// GfxReservoirTests — cajeta-gfx plan §3 (3.b-rest): the reservoir value type +
// RIS/MIS estimators (cajeta.gpu.Reservoir + cajeta.gpu.Ris).
//
// Resampled Importance Sampling keeps one sample from a weighted stream with
// probability proportional to its weight (a size-1 reservoir), so a single pass
// draws a sample distributed by the source weights. The reservoir state is an
// immutable @ValueType (Reservoir) carried by value through Ris's static helpers
// — each update CONSTRUCTS a new reservoir, the same value-type-carried pattern
// as SwRayCursor through SoftwareRayQuery. The selection is driven by an explicit
// `rand` argument so the stream is deterministic and host-testable; the MIS
// heuristics are pure analytic functions. GPU-free (pure host JIT).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP =
    "import cajeta.gpu.Reservoir;\n"
    "import cajeta.gpu.Ris;\n";

} // namespace

// 3.b-rest — RIS streaming selection with unit weights. With wi all 1, the
// replacement probability at step k is 1/k. Driven by explicit rand values:
// candidate 10 is taken (rand 0 < 1), 20 replaces it (rand 0.4 < 1/2), 30 does
// NOT replace (rand 0.9 > 1/3) — so the survivor is 20, M = 3, wSum = 3.
TEST(GfxReservoirTests, risUnitWeightSelection) {
    EXPECT_EQ(runI32(IMP,
        "        Reservoir r = Ris.empty();\n"
        "        if (r.y != 0 - 1) { return -1; }\n"           // empty sentinel
        "        if (r.m != 0) { return -2; }\n"
        "        r = Ris.update(r, 10, 1.0f, 0.0f);\n"
        "        if (r.y != 10) { return -3; }\n"
        "        if (r.m != 1) { return -4; }\n"
        "        r = Ris.update(r, 20, 1.0f, 0.4f);\n"         // 0.4 < 1/2 -> take 20
        "        if (r.y != 20) { return -5; }\n"
        "        r = Ris.update(r, 30, 1.0f, 0.9f);\n"         // 0.9 > 1/3 -> keep 20
        "        if (r.y != 20) { return -6; }\n"
        "        if (r.m != 3) { return -7; }\n"
        "        float32 ws = r.wSum;\n"
        "        float32 e = ws - 3.0f;\n"
        "        if (e < 0.0f) { e = 0.0f - e; }\n"
        "        if (e > 0.0001f) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3.b-rest — weights bias the selection, and resolve() computes the unbiased
// weight. A weight-3 candidate is taken; a weight-1 candidate then replaces it
// only with prob 1/4 (rand 0.5 > 0.25 -> kept). resolve with targetPdf 1.0 gives
// W = wSum / (M * pdf) = 4 / (2 * 1) = 2.
TEST(GfxReservoirTests, risWeightedAndResolve) {
    EXPECT_EQ(runI32(IMP,
        "        Reservoir r = Ris.empty();\n"
        "        r = Ris.update(r, 10, 3.0f, 0.5f);\n"         // 0.5 < 3/3 -> take 10
        "        if (r.y != 10) { return -1; }\n"
        "        r = Ris.update(r, 20, 1.0f, 0.5f);\n"         // 0.5 > 1/4 -> keep 10
        "        if (r.y != 10) { return -2; }\n"
        "        r = Ris.resolve(r, 1.0f);\n"
        "        float32 w = r.w;\n"
        "        float32 e = w - 2.0f;\n"
        "        if (e < 0.0f) { e = 0.0f - e; }\n"
        "        if (e > 0.0001f) { return -3; }\n"
        // resolve on an empty reservoir is a safe zero
        "        Reservoir z = Ris.empty();\n"
        "        z = Ris.resolve(z, 1.0f);\n"
        "        if (z.w != 0.0f) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 3.b-rest — MIS weights (Veach). balance(4,0.5, 2,0.25): f=2, g=0.5 -> 2/2.5 =
// 0.8. power(4,0.5, 2,0.25): f^2=4, g^2=0.25 -> 4/4.25 = 0.9411..  Degenerate
// inputs return 0.
TEST(GfxReservoirTests, misHeuristics) {
    EXPECT_EQ(runI32(IMP,
        "        float32 b = Ris.balanceHeuristic(4, 0.5f, 2, 0.25f);\n"
        "        float32 eb = b - 0.8f;\n"
        "        if (eb < 0.0f) { eb = 0.0f - eb; }\n"
        "        if (eb > 0.0005f) { return -1; }\n"
        "        float32 p = Ris.powerHeuristic(4, 0.5f, 2, 0.25f);\n"
        "        float32 ep = p - 0.9411765f;\n"
        "        if (ep < 0.0f) { ep = 0.0f - ep; }\n"
        "        if (ep > 0.0005f) { return -2; }\n"
        // balance is symmetric-complementary: balance(f,g) + balance(g,f) = 1
        "        float32 bg = Ris.balanceHeuristic(2, 0.25f, 4, 0.5f);\n"
        "        float32 sum = b + bg;\n"
        "        float32 es = sum - 1.0f;\n"
        "        if (es < 0.0f) { es = 0.0f - es; }\n"
        "        if (es > 0.0005f) { return -3; }\n"
        // degenerate -> 0
        "        if (Ris.balanceHeuristic(0, 0.0f, 0, 0.0f) != 0.0f) { return -4; }\n"
        "        if (Ris.powerHeuristic(0, 0.0f, 0, 0.0f) != 0.0f) { return -5; }\n"
        "        return 0;\n"), 0);
}
