//
// Stage 4 of the Vector<T,N> work: host codegen. Construction, component /
// index read + assignment, element-wise arithmetic + scalar broadcast, and the
// dot/length/normalize geometry helpers — all exercised through the host JIT.
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

float runF32(const std::string& body) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static float32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}

} // namespace

// Construct and read a component by name (.y) and by color alias (.g == .y).
TEST(VectorTests, constructAndComponentRead) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> v = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        return (int32) v.y;\n"), 2);
    EXPECT_EQ(runI32(
        "        Vector<float32,4> v = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        return (int32) v.w;\n"), 4);
}

// .r/.g/.b/.a alias the first four lanes.
TEST(VectorTests, colorAliases) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> c = new Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        return (int32)(c.r + c.b);\n"), 40);  // 10 + 30
}

// Element-wise add of two same-shape vectors.
TEST(VectorTests, elementWiseAdd) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> a = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> b = new Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Vector<float32,4> s = a + b;\n"
        "        return (int32) s.z;\n"), 33);  // 3 + 30
}

// Scalar broadcast: vec * scalar and scalar * vec.
TEST(VectorTests, scalarBroadcast) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> a = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> h = a * 2.0f;\n"
        "        return (int32) h.w;\n"), 8);   // 4 * 2
    EXPECT_EQ(runI32(
        "        Vector<float32,4> a = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> h = 3.0f * a;\n"
        "        return (int32) h.x;\n"), 3);   // 3 * 1
}

// Integer vectors: element-wise subtract.
TEST(VectorTests, integerVectorSub) {
    EXPECT_EQ(runI32(
        "        Vector<int32,3> a = new Vector<int32,3>(10, 20, 30);\n"
        "        Vector<int32,3> b = new Vector<int32,3>(1, 2, 3);\n"
        "        Vector<int32,3> d = a - b;\n"
        "        return d.y;\n"), 18);  // 20 - 2
}

// Indexed read v[i], constant and dynamic index.
TEST(VectorTests, indexRead) {
    EXPECT_EQ(runI32(
        "        Vector<int32,4> v = new Vector<int32,4>(5, 6, 7, 8);\n"
        "        return v[2];\n"), 7);
    EXPECT_EQ(runI32(
        "        Vector<int32,4> v = new Vector<int32,4>(5, 6, 7, 8);\n"
        "        int32 i = 3;\n"
        "        return v[i];\n"), 8);
}

// Component assignment v.x = e.
TEST(VectorTests, componentAssign) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> v = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        v.x = 9.0f;\n"
        "        return (int32)(v.x + v.y);\n"), 11);  // 9 + 2
}

// Indexed assignment v[i] = e.
TEST(VectorTests, indexAssign) {
    EXPECT_EQ(runI32(
        "        Vector<int32,4> v = new Vector<int32,4>(5, 6, 7, 8);\n"
        "        v[1] = 60;\n"
        "        int32 k = 2;\n"
        "        v[k] = 70;\n"
        "        return v[1] + v[2];\n"), 130);  // 60 + 70
}

// dot(a, b) over a float vector.
TEST(VectorTests, dotProduct) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = new Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> b = new Vector<float32,3>(4.0f, 5.0f, 6.0f);\n"
        "        return (int32) a.dot(b);\n"), 32);  // 4 + 10 + 18
}

// length((3,4)) == 5.
TEST(VectorTests, lengthHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> v = new Vector<float32,2>(3.0f, 4.0f);\n"
        "        return (int32) v.length();\n"), 5);
}

// normalize((0,4)) == (0,1); the y lane is 1.
TEST(VectorTests, normalizeHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> v = new Vector<float32,2>(0.0f, 4.0f);\n"
        "        Vector<float32,2> n = v.normalize();\n"
        "        return (int32)(n.y + 0.5f);\n"), 1);  // 1.0 + 0.5 -> 1
}

// B1 intrinsics A1 — element-wise min/max, scalar-bound clamp, and lerp.
// min((1,5,3),(4,2,6)) = (1,2,3) -> sum 6.
TEST(VectorTests, minHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = new Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = new Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
        "        Vector<float32,3> m = a.min(b);\n"
        "        return (int32)(m.x + m.y + m.z);\n"), 6);  // 1+2+3
}

// max((1,5,3),(4,2,6)) = (4,5,6) -> sum 15.
TEST(VectorTests, maxHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = new Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = new Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
        "        Vector<float32,3> m = a.max(b);\n"
        "        return (int32)(m.x + m.y + m.z);\n"), 15);  // 4+5+6
}

// clamp((-2,5,20), 0, 10) = (0,5,10) -> sum 15.
TEST(VectorTests, clampHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> v = new Vector<float32,3>(-2.0f, 5.0f, 20.0f);\n"
        "        Vector<float32,3> c = v.clamp(0.0f, 10.0f);\n"
        "        return (int32)(c.x + c.y + c.z);\n"), 15);  // 0+5+10
}

// lerp((0,0),(10,20), 0.5) = (5,10) -> sum 15.
TEST(VectorTests, lerpHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> a = new Vector<float32,2>(0.0f, 0.0f);\n"
        "        Vector<float32,2> b = new Vector<float32,2>(10.0f, 20.0f);\n"
        "        Vector<float32,2> r = a.lerp(b, 0.5f);\n"
        "        return (int32)(r.x + r.y);\n"), 15);  // 5+10
}

// B1 intrinsics A2 — geometry. cross((1,0,0),(0,1,0)) = (0,0,1) -> z lane 1.
TEST(VectorTests, crossHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = new Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> b = new Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Vector<float32,3> c = a.cross(b);\n"
        "        return (int32)(c.x + c.y + 2.0f * c.z);\n"), 2);  // (0,0,1): 2*1
}

// reflect((1,-1), (0,1)) = (1,-1) - 2*(-1)*(0,1) = (1,1) -> sum 2.
TEST(VectorTests, reflectHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> i = new Vector<float32,2>(1.0f, -1.0f);\n"
        "        Vector<float32,2> n = new Vector<float32,2>(0.0f, 1.0f);\n"
        "        Vector<float32,2> r = i.reflect(n);\n"
        "        return (int32)(r.x + r.y);\n"), 2);  // (1,1)
}

// distance((0,0),(3,4)) = 5.
TEST(VectorTests, distanceHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> a = new Vector<float32,2>(0.0f, 0.0f);\n"
        "        Vector<float32,2> b = new Vector<float32,2>(3.0f, 4.0f);\n"
        "        return (int32) a.distance(b);\n"), 5);
}

// refract straight-down through eta=1 is the unchanged ray: i=(0,-1), n=(0,1),
// dot(n,i)=-1, k=1, result = i - (… )*n = (0,-1) -> sum -1.
TEST(VectorTests, refractHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> i = new Vector<float32,2>(0.0f, -1.0f);\n"
        "        Vector<float32,2> n = new Vector<float32,2>(0.0f, 1.0f);\n"
        "        Vector<float32,2> r = i.refract(n, 1.0f);\n"
        "        return (int32)(r.x + r.y);\n"), -1);  // (0,-1)
}

// cross requires 3-component vectors.
TEST(VectorTests, crossNon3DRejected) {
    try {
        runI32(
            "        Vector<float32,2> a = new Vector<float32,2>(1.0f, 0.0f);\n"
            "        Vector<float32,2> b = new Vector<float32,2>(0.0f, 1.0f);\n"
            "        Vector<float32,2> c = a.cross(b);\n"
            "        return 0;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_METHOD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_METHOD");
    }
}

// Integer min/max is a follow-on — rejected for now with a clean diagnostic.
TEST(VectorTests, integerMinRejected) {
    try {
        runI32(
            "        Vector<int32,2> a = new Vector<int32,2>(1, 5);\n"
            "        Vector<int32,2> b = new Vector<int32,2>(4, 2);\n"
            "        Vector<int32,2> m = a.min(b);\n"
            "        return m.x;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_METHOD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_METHOD");
    }
}

// C — multi-component swizzle reads. v=(1,2,3,4):
//   v.xy   = (1,2)      v.xyz = (1,2,3)      v.zyx = (3,2,1)
//   v.xxyy = (1,1,2,2)  (repeats allowed)   .wz = (4,3)
TEST(VectorTests, swizzleReads) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> v = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,2> xy = v.xy;\n"        // (1,2)
        "        Vector<float32,3> zyx = v.zyx;\n"      // (3,2,1)
        "        Vector<float32,4> xxyy = v.xxyy;\n"    // (1,1,2,2)
        "        return (int32)(xy.x * 1.0f + xy.y * 10.0f\n"     // 1 + 20 = 21
        "                     + zyx.x * 100.0f\n"                 // + 300 = 321
        "                     + xxyy.x + xxyy.y + xxyy.z + xxyy.w);\n"), 327);  // +6
}

// Color aliases swizzle too: c=(10,20,30,40); c.rgb = (10,20,30).
TEST(VectorTests, swizzleColorAlias) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> c = new Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Vector<float32,3> rgb = c.rgb;\n"
        "        return (int32)(rgb.r + rgb.g + rgb.b);\n"), 60);  // 10+20+30
}

// A swizzle referencing a lane beyond the source is rejected (.xyz on N=2).
TEST(VectorTests, swizzleOutOfRangeRejected) {
    try {
        runI32(
            "        Vector<float32,2> v = new Vector<float32,2>(1.0f, 2.0f);\n"
            "        Vector<float32,3> bad = v.xyz;\n"
            "        return 0;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_COMPONENT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_COMPONENT");
    }
}

// B intrinsics — comparison masks + all/any/select. a=(1,5,3), b=(4,2,3):
//   a < b   = (T,F,F)        -> .any()=true, .all()=false
//   a == b  = (F,F,T)        -> .any()=true, .all()=false
TEST(VectorTests, comparisonMaskAnyAll) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = new Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = new Vector<float32,3>(4.0f, 2.0f, 3.0f);\n"
        "        int32 ltAny = (a < b).any() ? 1 : 0;\n"     // 1
        "        int32 ltAll = (a < b).all() ? 1 : 0;\n"     // 0
        "        int32 eqAny = (a == b).any() ? 1 : 0;\n"    // 1
        "        return ltAny * 100 + ltAll * 10 + eqAny;\n"), 101);
}

// select: pick a where (a < b), else b -> the element-wise min.
//   a=(1,5,3) b=(4,2,6): (a<b)=(T,F,T) -> (1,2,3) -> sum 6.
TEST(VectorTests, maskSelectIsMin) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = new Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = new Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
        "        Vector<float32,3> m = (a < b).select(a, b);\n"
        "        return (int32)(m.x + m.y + m.z);\n"), 6);  // 1+2+3
}

// A scalar RHS broadcasts: (x > 0) is a per-lane mask. x=(-1,2,-3):
//   (x > 0) = (F,T,F) -> select(x, 0) = (0,2,0) (a ReLU) -> sum 2.
TEST(VectorTests, maskScalarBroadcastRelu) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> x = new Vector<float32,3>(-1.0f, 2.0f, -3.0f);\n"
        "        Vector<float32,3> z = new Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> y = (x > 0.0f).select(x, z);\n"
        "        return (int32)(y.x + y.y + y.z);\n"), 2);  // ReLU -> (0,2,0)
}

// A component beyond the lane count is rejected (.w on N=3).
TEST(VectorTests, componentOutOfRangeRejected) {
    try {
        runI32(
            "        Vector<float32,3> v = new Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
            "        return (int32) v.w;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_COMPONENT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_COMPONENT");
    }
}

// Constructor arity must equal N.
TEST(VectorTests, constructorArityMismatchRejected) {
    try {
        runI32(
            "        Vector<float32,4> v = new Vector<float32,4>(1.0f, 2.0f);\n"
            "        return 0;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_CONSTRUCT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_CONSTRUCT");
    }
}
