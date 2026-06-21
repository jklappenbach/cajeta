//
// GfxFrustumTests — cajeta-gfx plan §1.e (graphics math foundation), the Frustum
// value type in cajeta.math: 6 clip planes extracted from a view-projection
// matrix (Gribb-Hartmann, adapted to the RH / clip-depth-[0,1] / column-vector
// convention of cajeta.math.Camera), with containsPoint / intersectsSphere /
// intersectsAabb cull tests. Each test JITs a program returning 0 on success or
// a negative failure code.
//
// The viewProj used here is an orthographic projection with an identity view, so
// the frustum is the analytic box x in [-2,2], y in [-2,2], z in [-10,-1]
// (view space looks down -z; near=1 -> z=-1, far=10 -> z=-10).
//
// cajeta.math is lazily parsed; importing cajeta.math.Frustum triggers it.
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
    "import cajeta.math.Frustum;\n"
    "import cajeta.math.Camera;\n";

const char* VP =
    "        Matrix<float32,4,4> vp = Camera.ortho(-2.0f, 2.0f, -2.0f, 2.0f, 1.0f, 10.0f);\n"
    "        Frustum fr = Frustum.fromViewProjection(vp);\n";

} // namespace

// 1e — containsPoint: a point inside the box is in; a point past the right plane
// (x>2) or in front of the near plane (|z|<1) or past the far plane (|z|>10) is out.
TEST(GfxFrustumTests, containsPoint) {
    EXPECT_EQ(runI32(IMP, std::string(VP) +
        "        Vector<float32,3> inside = stack Vector<float32,3>(0.0f, 0.0f, -5.0f);\n"
        "        if (!fr.containsPoint(inside)) { return -1; }\n"
        "        Vector<float32,3> rightOut = stack Vector<float32,3>(3.0f, 0.0f, -5.0f);\n"
        "        if (fr.containsPoint(rightOut)) { return -2; }\n"
        "        Vector<float32,3> nearOut = stack Vector<float32,3>(0.0f, 0.0f, -0.5f);\n"
        "        if (fr.containsPoint(nearOut)) { return -3; }\n"
        "        Vector<float32,3> farOut = stack Vector<float32,3>(0.0f, 0.0f, -20.0f);\n"
        "        if (fr.containsPoint(farOut)) { return -4; }\n"
        "        return 0;\n"), 0);
}

// 1e — intersectsSphere: a sphere well inside intersects; one far outside does
// not; one whose center is just outside the right plane but whose radius reaches
// back in does intersect.
TEST(GfxFrustumTests, intersectsSphere) {
    EXPECT_EQ(runI32(IMP + std::string("import cajeta.math.Sphere;\n"), std::string(VP) +
        "        Vector<float32,3> cIn = stack Vector<float32,3>(0.0f, 0.0f, -5.0f);\n"
        "        Sphere sIn = stack Sphere(cIn, 1.0f);\n"
        "        if (!fr.intersectsSphere(sIn)) { return -1; }\n"
        "        Vector<float32,3> cOut = stack Vector<float32,3>(5.0f, 0.0f, -5.0f);\n"
        "        Sphere sOut = stack Sphere(cOut, 1.0f);\n"
        "        if (fr.intersectsSphere(sOut)) { return -2; }\n"
        "        Vector<float32,3> cStr = stack Vector<float32,3>(3.0f, 0.0f, -5.0f);\n"
        "        Sphere sStr = stack Sphere(cStr, 2.0f);\n"   // center x=3 (1 past x=2), r=2 reaches in
        "        if (!fr.intersectsSphere(sStr)) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1e — intersectsAabb: a box inside intersects; one far outside does not; one
// straddling the right plane intersects.
TEST(GfxFrustumTests, intersectsAabb) {
    EXPECT_EQ(runI32(IMP + std::string("import cajeta.math.Aabb;\n"), std::string(VP) +
        "        Vector<float32,3> loIn = stack Vector<float32,3>(-1.0f, -1.0f, -6.0f);\n"
        "        Vector<float32,3> hiIn = stack Vector<float32,3>(1.0f, 1.0f, -4.0f);\n"
        "        Aabb bIn = stack Aabb(loIn, hiIn);\n"
        "        if (!fr.intersectsAabb(bIn)) { return -1; }\n"
        "        Vector<float32,3> loOut = stack Vector<float32,3>(5.0f, 5.0f, -6.0f);\n"
        "        Vector<float32,3> hiOut = stack Vector<float32,3>(7.0f, 7.0f, -4.0f);\n"
        "        Aabb bOut = stack Aabb(loOut, hiOut);\n"
        "        if (fr.intersectsAabb(bOut)) { return -2; }\n"
        "        Vector<float32,3> loStr = stack Vector<float32,3>(1.0f, -1.0f, -6.0f);\n"
        "        Vector<float32,3> hiStr = stack Vector<float32,3>(3.0f, 1.0f, -4.0f);\n"   // crosses x=2
        "        Aabb bStr = stack Aabb(loStr, hiStr);\n"
        "        if (!fr.intersectsAabb(bStr)) { return -3; }\n"
        "        return 0;\n"), 0);
}
