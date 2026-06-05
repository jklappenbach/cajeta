//
// Quaternion<T> host codegen — construction, `*` (Hamilton product + vector
// rotation), `+ -`, and the normalize/conjugate/length/dot/nlerp methods, all
// exercised through the host JIT. Storage is `<4 x T>` = (w, x, y, z).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
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

// A unit quaternion has length 1.
TEST(QuaternionTests, identityLength) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> q = new Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        return (int32) q.length();\n"), 1);
}

// Hamilton product i*j = k. i=(0,1,0,0) j=(0,0,1,0) k=(0,0,0,1); ij.dot(k)=1.
TEST(QuaternionTests, hamiltonProductIJequalsK) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> i = new Quaternion<float32>(0.0f, 1.0f, 0.0f, 0.0f);\n"
        "        Quaternion<float32> j = new Quaternion<float32>(0.0f, 0.0f, 1.0f, 0.0f);\n"
        "        Quaternion<float32> k = new Quaternion<float32>(0.0f, 0.0f, 0.0f, 1.0f);\n"
        "        Quaternion<float32> ij = i * j;\n"
        "        return (int32)(ij.dot(k) + 0.5f);\n"), 1);  // dot(k,k) = 1
}

// conjugate((1,2,3,4)) = (1,-2,-3,-4); its dot with the original = 1-4-9-16 = -28.
TEST(QuaternionTests, conjugate) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> q = new Quaternion<float32>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Quaternion<float32> c = q.conjugate();\n"
        "        return (int32) c.dot(q);\n"), -28);
}

// A 90-degree rotation about z (q = (cos45, 0, 0, sin45)) takes (1,0,0) -> (0,1,0).
TEST(QuaternionTests, rotateVector90AboutZ) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> q = new Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        Vector<float32,3> v = new Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r = q * v;\n"   // (0, 1, 0)
        "        return (int32)(r.y + 0.5f);\n"), 1);
}

// nlerp at t=0 returns the (normalized) first quaternion.
TEST(QuaternionTests, nlerpEndpoint) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> a = new Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Quaternion<float32> b = new Quaternion<float32>(0.0f, 0.0f, 0.0f, 1.0f);\n"
        "        Quaternion<float32> m = a.nlerp(b, 0.0f);\n"
        "        return (int32)(m.dot(a) + 0.5f);\n"), 1);  // dot(a,a) = 1
}

// Element-wise add: (1,2,3,4) + (1,1,1,1) = (2,3,4,5); length^2 = 4+9+16+25 = 54.
TEST(QuaternionTests, elementWiseAdd) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> a = new Quaternion<float32>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Quaternion<float32> b = new Quaternion<float32>(1.0f, 1.0f, 1.0f, 1.0f);\n"
        "        Quaternion<float32> s = a + b;\n"
        "        return (int32) s.dot(s);\n"), 54);  // 4+9+16+25
}

// slerp endpoints: slerp(a,b,0) rotates (1,0,0)->(1,0,0); slerp(a,b,1)->(0,1,0).
//   a = identity, b = 90deg-about-z.  return r0.x*10 + r1.y = 10 + 1 = 11.
TEST(QuaternionTests, slerpEndpoints) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> a = new Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Quaternion<float32> b = new Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        Vector<float32,3> x = new Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r0 = a.slerp(b, 0.0f) * x;\n"   // (1,0,0)
        "        Vector<float32,3> r1 = a.slerp(b, 1.0f) * x;\n"   // (0,1,0)
        "        return (int32)(r0.x + 0.5f) * 10 + (int32)(r1.y + 0.5f);\n"), 11);
}

// slerp midpoint: halfway between identity and a 90deg-about-z rotation is a
// 45deg rotation, which takes (1,0,0) -> (cos45, sin45, 0) ~ (0.707, 0.707, 0).
//   (r.x + r.y) ~ 1.414 -> round(14.14) = 14.
TEST(QuaternionTests, slerpMidpoint) {
    EXPECT_EQ(runI32(
        "        Quaternion<float32> a = new Quaternion<float32>(1.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Quaternion<float32> b = new Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
        "        Vector<float32,3> x = new Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> r = a.slerp(b, 0.5f) * x;\n"
        "        return (int32)((r.x + r.y) * 10.0f + 0.5f);\n"), 14);
}

// A non-float element type is rejected.
TEST(QuaternionTests, integerElementRejected) {
    try {
        runI32(
            "        Quaternion<int32> q = new Quaternion<int32>(1, 0, 0, 0);\n"
            "        return 0;\n");
        FAIL() << "expected CAJETA_ERROR_QUATERNION_ELEMENT_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_QUATERNION_ELEMENT_TYPE");
    }
}
