//
// GfxTransformTests — cajeta-gfx plan §1.a (graphics math foundation), the TRS
// Transform value type in cajeta.math: translation (Vector3) + rotation
// (Quaternion) + scale (Vector3), built over the builtin Vector<float32,3> and
// Quaternion<float32>. transformPoint / transformVector / compose / inverse, on
// ANALYTIC cases. Each test JITs a small program that returns 0 on success or a
// negative code identifying the first failed check.
//
// cajeta.math is lazily parsed; importing cajeta.math.Transform triggers it.
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

const char* IMP = "import cajeta.math.Transform;\n";

} // namespace

// 1a — identity() leaves a point unchanged.
TEST(GfxTransformTests, identityLeavesPointUnchanged) {
    EXPECT_EQ(runI32(IMP,
        "        Transform t = Transform.identity();\n"
        "        Vector<float32,3> p = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> r = t.transformPoint(p);\n"
        "        if (r.x != 1.0f) { return -1; }\n"
        "        if (r.y != 2.0f) { return -2; }\n"
        "        if (r.z != 3.0f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — translation-only transform adds the translation to a point.
TEST(GfxTransformTests, transformPointTranslateOnly) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> tr = stack Vector<float32,3>(10.0f, 20.0f, 30.0f);\n"
        "        Quaternion<float32> rot = stack Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> sc = stack Vector<float32,3>(1.0f, 1.0f, 1.0f);\n"
        "        Transform t = stack Transform(tr, rot, sc);\n"
        "        Vector<float32,3> p = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> r = t.transformPoint(p);\n"
        "        if (r.x != 11.0f) { return -1; }\n"
        "        if (r.y != 22.0f) { return -2; }\n"
        "        if (r.z != 33.0f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — full TRS order is scale, then rotate, then translate.
// scale (2,2,2), rotate 90deg about z, translate (0,0,5). p=(1,0,0):
// scale -> (2,0,0); rotate90z -> (0,2,0); translate -> (0,2,5).
TEST(GfxTransformTests, transformPointScaleRotateTranslate) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> tr = stack Vector<float32,3>(0.0f, 0.0f, 5.0f);\n"
        "        Quaternion<float32> rot = stack Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        Vector<float32,3> sc = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);\n"
        "        Transform t = stack Transform(tr, rot, sc);\n"
        "        Vector<float32,3> p = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r = t.transformPoint(p);\n"
        "        if (r.x < -0.01f || r.x > 0.01f) { return -1; }\n"
        "        if (r.y < 1.99f || r.y > 2.01f) { return -2; }\n"
        "        if (r.z < 4.99f || r.z > 5.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — transformVector ignores translation (a direction, not a point).
TEST(GfxTransformTests, transformVectorIgnoresTranslation) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> tr = stack Vector<float32,3>(100.0f, 100.0f, 100.0f);\n"
        "        Quaternion<float32> rot = stack Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> sc = stack Vector<float32,3>(3.0f, 3.0f, 3.0f);\n"
        "        Transform t = stack Transform(tr, rot, sc);\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> r = t.transformVector(v);\n"
        "        if (r.x < 2.99f || r.x > 3.01f) { return -1; }\n"
        "        if (r.y < 5.99f || r.y > 6.01f) { return -2; }\n"
        "        if (r.z < 8.99f || r.z > 9.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — inverse() undoes transformPoint: inv(t(p)) == p. Uniform scale so the
// TRS form is closed under inversion. rotate 90z, scale (2,2,2), translate (1,2,3).
TEST(GfxTransformTests, inverseUndoesTransformPoint) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> tr = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Quaternion<float32> rot = stack Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        Vector<float32,3> sc = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);\n"
        "        Transform t = stack Transform(tr, rot, sc);\n"
        "        Vector<float32,3> p = stack Vector<float32,3>(4.0f, 5.0f, 6.0f);\n"
        "        Vector<float32,3> q = t.transformPoint(p);\n"
        "        Transform inv = t.inverse();\n"
        "        Vector<float32,3> back = inv.transformPoint(q);\n"
        "        if (back.x < 3.98f || back.x > 4.02f) { return -1; }\n"
        "        if (back.y < 4.98f || back.y > 5.02f) { return -2; }\n"
        "        if (back.z < 5.98f || back.z > 6.02f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — t.compose(t.inverse()) is the identity transform (round-trip). Apply the
// composed transform to a point and expect the point back.
TEST(GfxTransformTests, composeWithInverseIsIdentity) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> tr = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Quaternion<float32> rot = stack Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        Vector<float32,3> sc = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);\n"
        "        Transform t = stack Transform(tr, rot, sc);\n"
        "        Transform inv = t.inverse();\n"
        "        Transform id = t.compose(inv);\n"
        "        Vector<float32,3> p = stack Vector<float32,3>(7.0f, 8.0f, 9.0f);\n"
        "        Vector<float32,3> r = id.transformPoint(p);\n"
        "        if (r.x < 6.98f || r.x > 7.02f) { return -1; }\n"
        "        if (r.y < 7.98f || r.y > 8.02f) { return -2; }\n"
        "        if (r.z < 8.98f || r.z > 9.02f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — compose applies the child transform first, then this. A = translate
// (10,0,0); B = translate (0,5,0). A.compose(B) on the origin -> B then A ->
// (10,5,0).
TEST(GfxTransformTests, composeAppliesChildFirst) {
    EXPECT_EQ(runI32(IMP,
        "        Quaternion<float32> rot = stack Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> one = stack Vector<float32,3>(1.0f, 1.0f, 1.0f);\n"
        "        Vector<float32,3> ta = stack Vector<float32,3>(10.0f, 0.0f, 0.0f);\n"
        "        Transform a = stack Transform(ta, rot, one);\n"
        "        Vector<float32,3> tb = stack Vector<float32,3>(0.0f, 5.0f, 0.0f);\n"
        "        Transform b = stack Transform(tb, rot, one);\n"
        "        Transform ab = a.compose(b);\n"
        "        Vector<float32,3> origin = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r = ab.transformPoint(origin);\n"
        "        if (r.x < 9.99f || r.x > 10.01f) { return -1; }\n"
        "        if (r.y < 4.99f || r.y > 5.01f) { return -2; }\n"
        "        if (r.z < -0.01f || r.z > 0.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}
