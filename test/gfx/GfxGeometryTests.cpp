//
// GfxGeometryTests — cajeta-gfx plan §1 (graphics math foundation), the geometry
// value types: Ray / Aabb / Sphere / Plane. Pure-Cajeta value types in
// cajeta.math built over the builtin Vector<float32,3> (dot/length/arith). Each
// test JITs a small program that exercises the type on an ANALYTIC case and
// returns 0 on success or a negative code identifying the first failed check.
//
// cajeta.math is lazily parsed; importing cajeta.math.Ray etc. triggers it.
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

} // namespace

// 1c — Ray.at(t) walks origin + t*direction. origin (1,2,3), dir (0,0,1):
// at(2) == (1,2,5).
TEST(GfxGeometryTests, rayAt) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Ray;\n",
        "        Vector<float32,3> o = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> d = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);\n"
        "        Ray r = stack Ray(o, d);\n"
        "        Vector<float32,3> p = r.at(2.0f);\n"
        "        if (p.x != 1.0f) { return -1; }\n"
        "        if (p.y != 2.0f) { return -2; }\n"
        "        if (p.z != 5.0f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1c — Aabb containment + center/extent. Box [0,0,0]-[2,2,2].
TEST(GfxGeometryTests, aabbContainsCenterExtent) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Aabb;\n",
        "        Vector<float32,3> lo = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> hi = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);\n"
        "        Aabb b = stack Aabb(lo, hi);\n"
        "        Vector<float32,3> inside = stack Vector<float32,3>(1.0f, 1.0f, 1.0f);\n"
        "        Vector<float32,3> outside = stack Vector<float32,3>(3.0f, 1.0f, 1.0f);\n"
        "        if (!b.contains(inside)) { return -1; }\n"
        "        if (b.contains(outside)) { return -2; }\n"
        "        Vector<float32,3> c = b.center();\n"
        "        if (c.x != 1.0f) { return -3; }\n"
        "        Vector<float32,3> e = b.extent();\n"
        "        if (e.y != 1.0f) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 1c — Aabb vs Aabb overlap. [0..2]^3 overlaps [1..3]^3, disjoint from [5..6]^3.
TEST(GfxGeometryTests, aabbIntersectsAabb) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Aabb;\n",
        "        Vector<float32,3> lo = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> hi = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);\n"
        "        Aabb a = stack Aabb(lo, hi);\n"
        "        Vector<float32,3> lo2 = stack Vector<float32,3>(1.0f, 1.0f, 1.0f);\n"
        "        Vector<float32,3> hi2 = stack Vector<float32,3>(3.0f, 3.0f, 3.0f);\n"
        "        Aabb b = stack Aabb(lo2, hi2);\n"
        "        Vector<float32,3> lo3 = stack Vector<float32,3>(5.0f, 5.0f, 5.0f);\n"
        "        Vector<float32,3> hi3 = stack Vector<float32,3>(6.0f, 6.0f, 6.0f);\n"
        "        Aabb c = stack Aabb(lo3, hi3);\n"
        "        if (!a.intersectsAabb(b)) { return -1; }\n"
        "        if (a.intersectsAabb(c)) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1c — Aabb slab ray test. Box [0..2]^3. Ray (-1,1,1)+x hits; (-1,5,1)+x misses.
TEST(GfxGeometryTests, aabbIntersectsRay) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Aabb;\n"
        "import cajeta.math.Ray;\n",
        "        Vector<float32,3> lo = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> hi = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);\n"
        "        Aabb b = stack Aabb(lo, hi);\n"
        "        Vector<float32,3> dir = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> oHit = stack Vector<float32,3>(-1.0f, 1.0f, 1.0f);\n"
        "        Ray hitRay = stack Ray(oHit, dir);\n"
        "        Vector<float32,3> oMiss = stack Vector<float32,3>(-1.0f, 5.0f, 1.0f);\n"
        "        Ray missRay = stack Ray(oMiss, dir);\n"
        "        if (!b.intersectsRay(hitRay)) { return -1; }\n"
        "        if (b.intersectsRay(missRay)) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1c — Sphere containment (squared, no sqrt). center origin, r=2.
TEST(GfxGeometryTests, sphereContains) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Sphere;\n",
        "        Vector<float32,3> c = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Sphere s = stack Sphere(c, 2.0f);\n"
        "        Vector<float32,3> inside = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> outside = stack Vector<float32,3>(3.0f, 0.0f, 0.0f);\n"
        "        if (!s.contains(inside)) { return -1; }\n"
        "        if (s.contains(outside)) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1c — Sphere vs ray (analytic, sqrt-free). Unit sphere at origin: +x ray from
// (-5,0,0) hits; parallel +x ray from (-5,5,0) misses.
TEST(GfxGeometryTests, sphereIntersectsRay) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Sphere;\n"
        "import cajeta.math.Ray;\n",
        "        Vector<float32,3> c = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Sphere s = stack Sphere(c, 1.0f);\n"
        "        Vector<float32,3> dir = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> oHit = stack Vector<float32,3>(-5.0f, 0.0f, 0.0f);\n"
        "        Ray hitRay = stack Ray(oHit, dir);\n"
        "        Vector<float32,3> oMiss = stack Vector<float32,3>(-5.0f, 5.0f, 0.0f);\n"
        "        Ray missRay = stack Ray(oMiss, dir);\n"
        "        if (!s.intersectsRay(hitRay)) { return -1; }\n"
        "        if (s.intersectsRay(missRay)) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1c — Sphere vs sphere. centers 3 apart: radii 2+2 intersect; radii 1+1 do not.
TEST(GfxGeometryTests, sphereIntersectsSphere) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Sphere;\n",
        "        Vector<float32,3> c0 = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> c1 = stack Vector<float32,3>(3.0f, 0.0f, 0.0f);\n"
        "        Sphere big0 = stack Sphere(c0, 2.0f);\n"
        "        Sphere big1 = stack Sphere(c1, 2.0f);\n"
        "        Sphere small0 = stack Sphere(c0, 1.0f);\n"
        "        Sphere small1 = stack Sphere(c1, 1.0f);\n"
        "        if (!big0.intersectsSphere(big1)) { return -1; }\n"
        "        if (small0.intersectsSphere(small1)) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1c — Plane signed distance. y=0 plane (normal (0,1,0), d=0): point (0,3,0) -> +3,
// point (0,-2,0) -> -2. Scaled by 1000 + 0.5 and truncated for an exact int check.
TEST(GfxGeometryTests, planeSignedDistance) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Plane;\n",
        "        Vector<float32,3> n = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Plane pl = stack Plane(n, 0.0f);\n"
        "        Vector<float32,3> above = stack Vector<float32,3>(0.0f, 3.0f, 0.0f);\n"
        "        Vector<float32,3> below = stack Vector<float32,3>(0.0f, -2.0f, 0.0f);\n"
        "        if ((int32)(pl.signedDistance(above) + 0.5f) != 3) { return -1; }\n"
        "        if ((int32)(pl.signedDistance(below) - 0.5f) != -2) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1c — Plane.fromPointNormal builds the plane through a point. The point itself
// has signed distance 0; a point 1 unit along the normal has distance 1.
TEST(GfxGeometryTests, planeFromPointNormal) {
    EXPECT_EQ(runI32(
        "import cajeta.math.Plane;\n",
        "        Vector<float32,3> p = stack Vector<float32,3>(0.0f, 5.0f, 0.0f);\n"
        "        Vector<float32,3> n = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Plane pl = Plane.fromPointNormal(p, n);\n"
        "        if ((int32)(pl.signedDistance(p) + 0.5f) != 0) { return -1; }\n"
        "        Vector<float32,3> up = stack Vector<float32,3>(0.0f, 6.0f, 0.0f);\n"
        "        if ((int32)(pl.signedDistance(up) + 0.5f) != 1) { return -2; }\n"
        "        return 0;\n"), 0);
}
