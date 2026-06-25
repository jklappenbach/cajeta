//
// XpuQemTests — cajeta-gfx plan §2.a (geometry processing foundation), the
// Garland–Heckbert quadric error metric (QEM) math core in cajeta.xpu.Qem. A
// quadric IS a Matrix<float32,4,4> (the symmetric fundamental error quadric
// Q = sum of p·pᵀ over the planes incident to a vertex); error(Q,v) = [v 1]ᵀ Q
// [v 1] is the sum of squared distances to those planes. Qem is a pure
// static-utility class (like cajeta.math.Camera). Each test JITs a small program
// returning 0 on success or a negative failure code, checked on ANALYTIC cases.
//
// cajeta.gpu is lazily parsed; importing cajeta.xpu.Qem triggers it.
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

const char* IMP = "import cajeta.xpu.Qem;\n";

} // namespace

// 2a — a single-plane quadric measures squared distance to that plane: a point
// ON the plane has error 0; a point 2 units off has error 4. Plane y=0.
TEST(XpuQemTests, planeErrorIsSquaredDistance) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> n = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Matrix<float32,4,4> q = Qem.planeQuadric(n, 0.0f);\n"
        "        Vector<float32,3> onPlane = stack Vector<float32,3>(5.0f, 0.0f, 3.0f);\n"
        "        float32 e0 = Qem.error(q, onPlane);\n"
        "        if (e0 < -0.01f || e0 > 0.01f) { return -1; }\n"
        "        Vector<float32,3> offPlane = stack Vector<float32,3>(0.0f, 2.0f, 0.0f);\n"
        "        float32 e1 = Qem.error(q, offPlane);\n"
        "        if (e1 < 3.99f || e1 > 4.01f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 2a — quadrics accumulate by addition: the sum of the three axis-plane quadrics
// through the origin measures squared distance to the origin (|v|²). At (1,2,2)
// that is 1+4+4 = 9; at the origin it is 0.
TEST(XpuQemTests, quadricsAccumulateByAddition) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> nx = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> ny = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Vector<float32,3> nz = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);\n"
        "        Matrix<float32,4,4> qx = Qem.planeQuadric(nx, 0.0f);\n"
        "        Matrix<float32,4,4> qy = Qem.planeQuadric(ny, 0.0f);\n"
        "        Matrix<float32,4,4> qz = Qem.planeQuadric(nz, 0.0f);\n"
        "        Matrix<float32,4,4> qxy = qx + qy;\n"
        "        Matrix<float32,4,4> q = qxy + qz;\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(1.0f, 2.0f, 2.0f);\n"
        "        float32 e = Qem.error(q, v);\n"
        "        if (e < 8.99f || e > 9.01f) { return -1; }\n"
        "        Vector<float32,3> o = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        float32 e0 = Qem.error(q, o);\n"
        "        if (e0 < -0.01f || e0 > 0.01f) { return -2; }\n"
        "        return 0;\n"), 0);
}

// 2a — optimalPosition of a well-conditioned (invertible) quadric is the exact
// minimizer. Three orthogonal planes x=2, y=0, z=0 meet at (2,0,0) with zero
// error there, so the optimal vertex placement is (2,0,0).
TEST(XpuQemTests, optimalPositionSolvesCorner) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> nx = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> ny = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Vector<float32,3> nz = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);\n"
        "        Matrix<float32,4,4> qx = Qem.planeQuadric(nx, -2.0f);\n"   // x - 2 = 0
        "        Matrix<float32,4,4> qy = Qem.planeQuadric(ny, 0.0f);\n"
        "        Matrix<float32,4,4> qz = Qem.planeQuadric(nz, 0.0f);\n"
        "        Matrix<float32,4,4> qxy = qx + qy;\n"
        "        Matrix<float32,4,4> q = qxy + qz;\n"
        "        Vector<float32,3> fallback = stack Vector<float32,3>(9.0f, 9.0f, 9.0f);\n"
        "        Vector<float32,3> p = Qem.optimalPosition(q, fallback);\n"
        "        if (p.x < 1.99f || p.x > 2.01f) { return -1; }\n"
        "        if (p.y < -0.01f || p.y > 0.01f) { return -2; }\n"
        "        if (p.z < -0.01f || p.z > 0.01f) { return -3; }\n"
        "        float32 e = Qem.error(q, p);\n"
        "        if (e < -0.01f || e > 0.01f) { return -4; }\n"   // exact corner, zero error
        "        return 0;\n"), 0);
}

// 2a — a rank-deficient (singular) quadric falls back to the supplied vertex
// instead of solving. Two parallel planes y=0 and y=2 constrain only y, so the
// 3×3 normal system is singular; optimalPosition returns the fallback.
TEST(XpuQemTests, optimalPositionFallsBackWhenSingular) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> ny = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Matrix<float32,4,4> q0 = Qem.planeQuadric(ny, 0.0f);\n"    // y = 0
        "        Matrix<float32,4,4> q2 = Qem.planeQuadric(ny, -2.0f);\n"   // y - 2 = 0
        "        Matrix<float32,4,4> q = q0 + q2;\n"
        "        Vector<float32,3> fallback = stack Vector<float32,3>(3.0f, 1.0f, 7.0f);\n"
        "        Vector<float32,3> p = Qem.optimalPosition(q, fallback);\n"
        "        if (p.x < 2.99f || p.x > 3.01f) { return -1; }\n"
        "        if (p.y < 0.99f || p.y > 1.01f) { return -2; }\n"
        "        if (p.z < 6.99f || p.z > 7.01f) { return -3; }\n"
        "        return 0;\n"), 0);
}

// 2a — Garland–Heckbert edge-collapse cost = error of the combined quadric at
// the chosen collapse position. Collapsing across the parallel planes y=0 and
// y=2 to the mid-y point (y=1) costs 1² + 1² = 2.
TEST(XpuQemTests, edgeCollapseCostMatchesGarlandHeckbert) {
    EXPECT_EQ(runI32(IMP,
        "        Vector<float32,3> ny = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Matrix<float32,4,4> q0 = Qem.planeQuadric(ny, 0.0f);\n"
        "        Matrix<float32,4,4> q2 = Qem.planeQuadric(ny, -2.0f);\n"
        "        Vector<float32,3> mid = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        float32 cost = Qem.edgeCollapseCost(q0, q2, mid);\n"
        "        if (cost < 1.99f || cost > 2.01f) { return -1; }\n"
        "        return 0;\n"), 0);
}
