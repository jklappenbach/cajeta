//
// GfxRotationTests — cajeta-gfx plan §1.a (graphics math foundation), the
// Quaternion construction ergonomics in cajeta.math.Rotation: identity /
// fromAxisAngle / fromEuler. The builtin Quaternion<float32> already does the
// algebra (mul, slerp, conjugate, rotate-vector); what was missing is building a
// rotation from an axis+angle or Euler angles. Closed-form analytic tests via
// the rotate-vector form q * v and quaternion equality (dot == 1 for unit quats).
// Each test JITs a program returning 0 on success or a negative failure code.
//
// cajeta.math is lazily parsed; importing cajeta.math.Rotation triggers it.
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

const char* IMP = "import cajeta.math.Rotation;\n";

} // namespace

// 1a — identity() leaves a vector unchanged under rotation.
TEST(GfxRotationTests, identityRotatesUnchanged) {
    EXPECT_EQ(runI32(IMP,
        "        Quaternion<float32> q = Rotation.identity();\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> r = q * v;\n"
        "        if (r.x < 0.99f || r.x > 1.01f) { return -1; }\n"
        "        if (r.y < 1.99f || r.y > 2.01f) { return -2; }\n"
        "        if (r.z < 2.99f || r.z > 3.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — fromAxisAngle about +z by 90deg takes (1,0,0) -> (0,1,0).
TEST(GfxRotationTests, fromAxisAngleZ90) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> axis = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);\n"
        "        Quaternion<float32> q = Rotation.fromAxisAngle(axis, 1.57079633f);\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r = q * v;\n"
        "        if (r.x < -0.01f || r.x > 0.01f) { return -1; }\n"
        "        if (r.y < 0.99f || r.y > 1.01f) { return -2; }\n"
        "        if (r.z < -0.01f || r.z > 0.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — fromAxisAngle about +x by 90deg takes (0,1,0) -> (0,0,1).
TEST(GfxRotationTests, fromAxisAngleX90) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> axis = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Quaternion<float32> q = Rotation.fromAxisAngle(axis, 1.57079633f);\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Vector<float32,3> r = q * v;\n"
        "        if (r.x < -0.01f || r.x > 0.01f) { return -1; }\n"
        "        if (r.y < -0.01f || r.y > 0.01f) { return -2; }\n"
        "        if (r.z < 0.99f || r.z > 1.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 1a — fromAxisAngle normalizes a non-unit axis: axis (0,0,5) about z by 90deg
// is the same rotation as the unit-z axis: (1,0,0) -> (0,1,0).
TEST(GfxRotationTests, fromAxisAngleNormalizesAxis) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> axis = stack Vector<float32,3>(0.0f, 0.0f, 5.0f);\n"
        "        Quaternion<float32> q = Rotation.fromAxisAngle(axis, 1.57079633f);\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r = q * v;\n"
        "        if (r.x < -0.01f || r.x > 0.01f) { return -1; }\n"
        "        if (r.y < 0.99f || r.y > 1.01f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 1a — fromAxisAngle matches the closed-form unit quaternion: axis +z, 90deg ->
// (cos45, 0, 0, sin45). Two unit quaternions are equal iff their dot is 1.
TEST(GfxRotationTests, fromAxisAngleClosedForm) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> axis = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);\n"
        "        Quaternion<float32> q = Rotation.fromAxisAngle(axis, 1.57079633f);\n"
        "        Quaternion<float32> expected = stack Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        float32 d = q.dot(expected);\n"
        "        if (d < 0.999f || d > 1.001f) { return -1; }\n"
        "        return 0;\n"), 0);
}

// 1a — fromEuler with yaw (about +y) by 90deg takes (0,0,1) -> (1,0,0).
TEST(GfxRotationTests, fromEulerYaw90) {
    EXPECT_EQ(runI32(IMP,
        "        Quaternion<float32> q = Rotation.fromEuler(0.0f, 1.57079633f, 0.0f);\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);\n"
        "        Vector<float32,3> r = q * v;\n"
        "        if (r.x < 0.99f || r.x > 1.01f) { return -1; }\n"
        "        if (r.y < -0.01f || r.y > 0.01f) { return -2; }\n"
        "        if (r.z < -0.01f || r.z > 0.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}
