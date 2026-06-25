//
// XpuSdfTests — cajeta-gfx plan §3 (slice 3a): signed-distance-field primitives
// + sphere tracing (cajeta.xpu.Sdf).
//
// TDD 3.c's headline clause: "SDF sphere-trace hits an analytic surface within
// ε". An SDF maps a point to the signed distance to the nearest surface
// (negative inside); sphere tracing marches a ray by exactly that distance each
// step — the largest empty-sphere radius — so it provably never overshoots and
// converges onto the surface. All pure float32 math (Math.sqrt/abs/min/max), so
// it verifies on the host against analytic distances and the exact ray/sphere
// hit (a ray from (0,0,-5) toward a unit sphere at the origin hits at t = 4).
//
// GPU-free (pure host JIT; no device).
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

const char* IMP = "import cajeta.xpu.Sdf;\n";

} // namespace

// 3c — sphere SDF is |p| - r: outside positive, surface 0, inside negative.
TEST(XpuSdfTests, sphereSdfAnalytic) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Sdf.sphere(3.0f, 0.0f, 0.0f, 1.0f);\n"  // 3 - 1 = 2
        "        if (a < 1.999f) { return -1; }\n"
        "        if (a > 2.001f) { return -2; }\n"
        "        float32 b = Sdf.sphere(0.0f, 0.0f, 0.0f, 1.0f);\n"  // -1 (centre)
        "        if (b < -1.001f) { return -3; }\n"
        "        if (b > -0.999f) { return -4; }\n"
        "        float32 c = Sdf.sphere(1.0f, 0.0f, 0.0f, 1.0f);\n"  // 0 (surface)
        "        if (c < -0.001f) { return -5; }\n"
        "        if (c > 0.001f) { return -6; }\n"
        "        float32 d = Sdf.sphere(0.0f, 4.0f, 0.0f, 2.0f);\n"  // 4 - 2 = 2
        "        if (d < 1.999f) { return -7; }\n"
        "        if (d > 2.001f) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3c — box SDF (half-extents b): exterior distance + interior negative.
TEST(XpuSdfTests, boxSdfAnalytic) {
    EXPECT_EQ(runI32(IMP,
        "        float32 a = Sdf.box(2.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);\n"  // 1
        "        if (a < 0.999f) { return -1; }\n"
        "        if (a > 1.001f) { return -2; }\n"
        "        float32 b = Sdf.box(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);\n"  // -1
        "        if (b < -1.001f) { return -3; }\n"
        "        if (b > -0.999f) { return -4; }\n"
        "        float32 c = Sdf.box(0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 1.0f);\n"  // 1
        "        if (c < 0.999f) { return -5; }\n"
        "        if (c > 1.001f) { return -6; }\n"
        // corner: q=(1,1,-1) -> outside sqrt(2) ~ 1.41421
        "        float32 d = Sdf.box(2.0f, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f);\n"
        "        if (d < 1.413f) { return -7; }\n"
        "        if (d > 1.415f) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3c — CSG combinators: union = min, intersect = max, subtract(a,b) = max(a,-b).
TEST(XpuSdfTests, booleanOps) {
    EXPECT_EQ(runI32(IMP,
        "        float32 u = Sdf.opUnion(0.3f, 0.7f);\n"       // 0.3
        "        if (u < 0.299f) { return -1; }\n"
        "        if (u > 0.301f) { return -2; }\n"
        "        float32 n = Sdf.opIntersect(0.3f, 0.7f);\n"   // 0.7
        "        if (n < 0.699f) { return -3; }\n"
        "        if (n > 0.701f) { return -4; }\n"
        "        float32 s = Sdf.opSubtract(0.3f, -0.2f);\n"   // max(0.3, 0.2) = 0.3
        "        if (s < 0.299f) { return -5; }\n"
        "        if (s > 0.301f) { return -6; }\n"
        "        float32 s2 = Sdf.opSubtract(0.5f, 0.2f);\n"   // max(0.5, -0.2) = 0.5
        "        if (s2 < 0.499f) { return -7; }\n"
        "        if (s2 > 0.501f) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3c — sphere tracing hits the analytic surface within ε: a ray from (0,0,-5)
// along +z toward a unit sphere at the origin hits the front face (z = -1) at
// t = 4. The march converges there to within the eps band.
TEST(XpuSdfTests, sphereTraceHitsSurface) {
    EXPECT_EQ(runI32(IMP,
        "        float32 t = Sdf.sphereTraceSphere(\n"
        "            0.0f, 0.0f, -5.0f,   0.0f, 0.0f, 1.0f,\n"   // origin, dir(+z)
        "            0.0f, 0.0f, 0.0f, 1.0f,\n"                  // unit sphere at origin
        "            100.0f, 128, 0.001f);\n"                    // tMax, steps, eps
        "        if (t < 3.99f) { return -1; }\n"
        "        if (t > 4.01f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 3c — a ray that passes the sphere by more than its radius reports a miss (-1).
TEST(XpuSdfTests, sphereTraceMisses) {
    EXPECT_EQ(runI32(IMP,
        "        float32 t = Sdf.sphereTraceSphere(\n"
        "            0.0f, 3.0f, -5.0f,   0.0f, 0.0f, 1.0f,\n"   // y = 3, clears the unit sphere
        "            0.0f, 0.0f, 0.0f, 1.0f,\n"
        "            100.0f, 128, 0.001f);\n"
        "        if (t > -0.5f) { return -1; }\n"               // must be the -1 sentinel
        "        return 0;\n"), 0);
}

// 3c-rest — grid-sampled SDF: trilinear fetch from a flat float32[] volume. A
// 2x2x2 volume whose voxel (x,y,z) holds value x + 2y + 4z: integer corners read
// back exactly, the centre (0.5,0.5,0.5) is the mean of the 8 corners (3.5), and
// out-of-range samples clamp to the nearest face.
TEST(XpuSdfTests, sampleGridTrilinear) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] vol = heap float32[8];\n"
        "        int32 z = 0;\n"
        "        while (z < 2) {\n"
        "            int32 y = 0;\n"
        "            while (y < 2) {\n"
        "                int32 x = 0;\n"
        "                while (x < 2) {\n"
        "                    int32 idx = x + y * 2 + z * 4;\n"
        "                    float32 val = x + 2 * y + 4 * z;\n"
        "                    vol[idx] = val;\n"
        "                    x = x + 1;\n"
        "                }\n"
        "                y = y + 1;\n"
        "            }\n"
        "            z = z + 1;\n"
        "        }\n"
        // exact corners
        "        if (Sdf.sampleGrid(vol, 2, 2, 2, 0.0f, 0.0f, 0.0f) != 0.0f) { return -1; }\n"
        "        if (Sdf.sampleGrid(vol, 2, 2, 2, 1.0f, 0.0f, 0.0f) != 1.0f) { return -2; }\n"
        "        if (Sdf.sampleGrid(vol, 2, 2, 2, 0.0f, 1.0f, 0.0f) != 2.0f) { return -3; }\n"
        "        if (Sdf.sampleGrid(vol, 2, 2, 2, 1.0f, 1.0f, 1.0f) != 7.0f) { return -4; }\n"
        // centre = mean of all 8 = 3.5
        "        float32 mid = Sdf.sampleGrid(vol, 2, 2, 2, 0.5f, 0.5f, 0.5f);\n"
        "        float32 em = mid - 3.5f;\n"
        "        if (em < 0.0f) { em = 0.0f - em; }\n"
        "        if (em > 0.001f) { return -5; }\n"
        // a face midpoint along x at (0.5,0,0) = mean(0,1) = 0.5
        "        float32 fx = Sdf.sampleGrid(vol, 2, 2, 2, 0.5f, 0.0f, 0.0f);\n"
        "        float32 ef = fx - 0.5f;\n"
        "        if (ef < 0.0f) { ef = 0.0f - ef; }\n"
        "        if (ef > 0.001f) { return -6; }\n"
        // clamp-to-edge: beyond max -> (1,1,1)=7; below 0 -> (0,0,0)=0
        "        if (Sdf.sampleGrid(vol, 2, 2, 2, 5.0f, 5.0f, 5.0f) != 7.0f) { return -7; }\n"
        "        if (Sdf.sampleGrid(vol, 2, 2, 2, 0.0f - 3.0f, 0.0f, 0.0f) != 0.0f) { return -8; }\n"
        "        return 0;\n"), 0);
}
