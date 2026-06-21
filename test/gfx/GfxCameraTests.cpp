//
// GfxCameraTests — cajeta-gfx plan §1.b (graphics math foundation), the
// projection/view matrix builders in cajeta.math.Camera: perspective / ortho /
// lookAt, each returning a Matrix<float32,4,4>. Convention: right-handed,
// clip-space depth range [0,1] (Vulkan/D3D), column-vector convention (m * v).
// Golden tests against hand-computed reference matrices on analytic cases. Each
// test JITs a small program returning 0 on success or a negative failure code.
//
// cajeta.math is lazily parsed; importing cajeta.math.Camera triggers it.
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

const char* IMP = "import cajeta.math.Camera;\n";

} // namespace

// 1b — perspective(fovy=90deg, aspect=1, near=1, far=100), RH depth [0,1].
// f = 1/tan(45deg) = 1. m00 = f/aspect = 1; m11 = f = 1; m22 = far/(near-far) =
// 100/-99 = -1.0101; m23 = far*near/(near-far) = -1.0101; m32 = -1; m33 = 0.
TEST(GfxCameraTests, perspectiveEntries) {
    EXPECT_EQ(runI32(IMP,
        "        float32 fovy = 1.57079633f;\n"   // 90 degrees in radians
        "        Matrix<float32,4,4> m = Camera.perspective(fovy, 1.0f, 1.0f, 100.0f);\n"
        "        if (m[0][0] < 0.99f || m[0][0] > 1.01f) { return -1; }\n"
        "        if (m[1][1] < 0.99f || m[1][1] > 1.01f) { return -2; }\n"
        "        if (m[2][2] < -1.02f || m[2][2] > -1.00f) { return -3; }\n"
        "        if (m[2][3] < -1.02f || m[2][3] > -1.00f) { return -4; }\n"
        "        if (m[3][2] != -1.0f) { return -5; }\n"
        "        if (m[3][3] < -0.01f || m[3][3] > 0.01f) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 1b — perspective maps the near plane to clip z=0 and far plane to clip z=w
// (depth 1) under the column-vector convention m * v. Point on near plane in RH
// view space is (0,0,-near,1); on far plane (0,0,-far,1).
TEST(GfxCameraTests, perspectiveNearFarDepth) {
    EXPECT_EQ(runI32(IMP,
        "        Matrix<float32,4,4> m = Camera.perspective(1.57079633f, 1.0f, 1.0f, 100.0f);\n"
        "        Vector<float32,4> nearP = stack Vector<float32,4>(0.0f, 0.0f, -1.0f, 1.0f);\n"
        "        Vector<float32,4> cn = m * nearP;\n"
        "        if (cn.z < -0.01f || cn.z > 0.01f) { return -1; }\n"     // near -> z 0
        "        if (cn.w < 0.99f || cn.w > 1.01f) { return -2; }\n"       // w = 1
        "        Vector<float32,4> farP = stack Vector<float32,4>(0.0f, 0.0f, -100.0f, 1.0f);\n"
        "        Vector<float32,4> cf = m * farP;\n"
        "        if (cf.z < 99.0f || cf.z > 101.0f) { return -3; }\n"      // far -> z = w
        "        if (cf.w < 99.0f || cf.w > 101.0f) { return -4; }\n"      // w = 100 -> z/w = 1
        "        return 0;\n"), 0);
}

// 1b — ortho(-1,1,-1,1,0,1), RH depth [0,1]. m00=2/(r-l)=1; m11=1; m22=-1/(f-n)=-1;
// m03=-(r+l)/(r-l)=0; m13=0; m23=-near/(f-n)=0; m33=1.
TEST(GfxCameraTests, orthoEntries) {
    EXPECT_EQ(runI32(IMP,
        "        Matrix<float32,4,4> m = Camera.ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);\n"
        "        if (m[0][0] < 0.99f || m[0][0] > 1.01f) { return -1; }\n"
        "        if (m[1][1] < 0.99f || m[1][1] > 1.01f) { return -2; }\n"
        "        if (m[2][2] < -1.01f || m[2][2] > -0.99f) { return -3; }\n"
        "        if (m[0][3] < -0.01f || m[0][3] > 0.01f) { return -4; }\n"
        "        if (m[2][3] < -0.01f || m[2][3] > 0.01f) { return -5; }\n"
        "        if (m[3][3] < 0.99f || m[3][3] > 1.01f) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 1b — ortho maps a corner of the box to the NDC corner. With the symmetric box
// above, the point (1,1,1,1) maps to NDC (1,1,1).
TEST(GfxCameraTests, orthoMapsCorner) {
    EXPECT_EQ(runI32(IMP,
        "        Matrix<float32,4,4> m = Camera.ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);\n"
        "        Vector<float32,4> corner = stack Vector<float32,4>(1.0f, 1.0f, -1.0f, 1.0f);\n"
        "        Vector<float32,4> ndc = m * corner;\n"
        "        if (ndc.x < 0.99f || ndc.x > 1.01f) { return -1; }\n"
        "        if (ndc.y < 0.99f || ndc.y > 1.01f) { return -2; }\n"
        "        if (ndc.z < 0.99f || ndc.z > 1.01f) { return -3; }\n"     // z=-1 -> depth 1
        "        return 0;\n"), 0);
}

// 1b — lookAt(eye=(0,0,5), center=origin, up=+y) is a pure -z view. Right=+x,
// up=+y, forward=-z. Entries: m00=1, m11=1, m22=1, m23=-5, m33=1.
TEST(GfxCameraTests, lookAtEntries) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> eye = stack Vector<float32,3>(0.0f, 0.0f, 5.0f);\n"
        "        Vector<float32,3> center = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> up = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Matrix<float32,4,4> m = Camera.lookAt(eye, center, up);\n"
        "        if (m[0][0] < 0.99f || m[0][0] > 1.01f) { return -1; }\n"
        "        if (m[1][1] < 0.99f || m[1][1] > 1.01f) { return -2; }\n"
        "        if (m[2][2] < 0.99f || m[2][2] > 1.01f) { return -3; }\n"
        "        if (m[2][3] < -5.01f || m[2][3] > -4.99f) { return -4; }\n"
        "        if (m[3][3] < 0.99f || m[3][3] > 1.01f) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 1b — lookAt maps the eye to the view-space origin and the look-at target to a
// point straight ahead on -z at distance |eye - center| (= 5).
TEST(GfxCameraTests, lookAtMapsEyeAndTarget) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> eye = stack Vector<float32,3>(0.0f, 0.0f, 5.0f);\n"
        "        Vector<float32,3> center = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> up = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Matrix<float32,4,4> m = Camera.lookAt(eye, center, up);\n"
        "        Vector<float32,4> eyeH = stack Vector<float32,4>(0.0f, 0.0f, 5.0f, 1.0f);\n"
        "        Vector<float32,4> ve = m * eyeH;\n"
        "        if (ve.x < -0.01f || ve.x > 0.01f) { return -1; }\n"
        "        if (ve.y < -0.01f || ve.y > 0.01f) { return -2; }\n"
        "        if (ve.z < -0.01f || ve.z > 0.01f) { return -3; }\n"      // eye -> origin
        "        Vector<float32,4> tgtH = stack Vector<float32,4>(0.0f, 0.0f, 0.0f, 1.0f);\n"
        "        Vector<float32,4> vt = m * tgtH;\n"
        "        if (vt.z < -5.01f || vt.z > -4.99f) { return -4; }\n"     // target -> -z 5 ahead
        "        return 0;\n"), 0);
}
