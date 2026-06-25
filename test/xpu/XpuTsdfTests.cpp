//
// XpuTsdfTests — cajeta-gfx plan §2.c, the truncated signed-distance-function
// voxel grid (cajeta.xpu.Tsdf) with Curless-Levoy fusion. Each voxel holds a
// running signed distance D and weight W; a measurement (truncated distance d,
// weight w) fuses by D' = (W*D + w*d)/(W+w), W' = W+w, so the fused zero-crossing
// is the weight-weighted average of the scans. Measurement weight is scaled by
// |normalZ| (grazing hits down-weighted). Each test JITs a small program
// returning 0 on success or a negative failure code, checked on ANALYTIC plane
// scans.
//
// cajeta.xpu is lazily parsed; importing it triggers the parse.
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

const char* IMP = "import cajeta.xpu.Tsdf;\n";

} // namespace

// 2c — two equal-weight head-on plane scans fuse to the analytic midpoint. A
// 1x1x11 column (spacing 0.1) is scanned at z=0.4 and z=0.8 (both normalZ=1,
// weight 1); the fused surface (D=0 crossing) is the midpoint z=0.6: D is ~0 at
// layer 6, negative below (layer 5), positive above (layer 7).
TEST(XpuTsdfTests, fuseTwoPlanesToMidpoint) {
    EXPECT_EQ(runI32(IMP,
        "        Tsdf g = heap Tsdf(1, 1, 11, 0.1f, 0.5f);\n"
        "        g.fusePlaneZ(0.4f, 1.0f, 1.0f);\n"
        "        g.fusePlaneZ(0.8f, 1.0f, 1.0f);\n"
        "        float32 dmid = g.distanceAt(0, 0, 6);\n"
        "        if (dmid < -0.01f || dmid > 0.01f) { return -1; }\n"
        "        float32 dlow = g.distanceAt(0, 0, 5);\n"
        "        if (dlow >= 0.0f) { return -2; }\n"
        "        float32 dhigh = g.distanceAt(0, 0, 7);\n"
        "        if (dhigh <= 0.0f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 2c — a grazing scan is down-weighted and pulls the estimate less. Fuse a
// head-on scan at z=0.4 (normalZ=1 -> w=1) then a grazing scan at z=0.8
// (normalZ=0.5 -> w=0.5). At layer 6 (z=0.6) the fused D is
// (1*0.2 + 0.5*(-0.2))/1.5 = 0.0667 (positive: crossing pulled toward the
// head-on scan, below 0.6, vs. 0 for equal weights), and the weight is 1.5.
TEST(XpuTsdfTests, grazingHitIsDownWeighted) {
    EXPECT_EQ(runI32(IMP,
        "        Tsdf g = heap Tsdf(1, 1, 11, 0.1f, 0.5f);\n"
        "        g.fusePlaneZ(0.4f, 1.0f, 1.0f);\n"
        "        g.fusePlaneZ(0.8f, 0.5f, 1.0f);\n"
        "        float32 d6 = g.distanceAt(0, 0, 6);\n"
        "        if (d6 < 0.056f || d6 > 0.077f) { return -1; }\n"
        "        float32 wt = g.weightAt(0, 0, 6);\n"
        "        if (wt < 1.49f || wt > 1.51f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 2c — the first fuse into an empty voxel stores the measurement exactly
// (W starts 0 -> D=d, W=w), and distances are truncated to +/-trunc. Scan z=0.5
// (normalZ=1, base weight 2.0) into a 1x1x21 column (trunc 0.5): layer 7 (z=0.7,
// d=0.2) -> D=0.2, W=2.0; layer 20 (z=2.0, d=1.5) clamps to +0.5; layer 0 (z=0,
// d=-0.5) clamps to -0.5.
TEST(XpuTsdfTests, firstFuseStoresMeasurementAndTruncates) {
    EXPECT_EQ(runI32(IMP,
        "        Tsdf g = heap Tsdf(1, 1, 21, 0.1f, 0.5f);\n"
        "        g.fusePlaneZ(0.5f, 1.0f, 2.0f);\n"
        "        float32 d7 = g.distanceAt(0, 0, 7);\n"
        "        if (d7 < 0.19f || d7 > 0.21f) { return -1; }\n"
        "        float32 w7 = g.weightAt(0, 0, 7);\n"
        "        if (w7 < 1.99f || w7 > 2.01f) { return -2; }\n"
        "        float32 dfar = g.distanceAt(0, 0, 20);\n"
        "        if (dfar < 0.49f || dfar > 0.51f) { return -3; }\n"
        "        float32 dbelow = g.distanceAt(0, 0, 0);\n"
        "        if (dbelow < -0.51f || dbelow > -0.49f) { return -4; }\n"
        "        return 0;\n"), 0);
}
