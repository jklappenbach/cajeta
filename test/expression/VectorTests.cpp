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
        "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        return (int32) v.y;\n"), 2);
    EXPECT_EQ(runI32(
        "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        return (int32) v.w;\n"), 4);
}

// SIMD eqMask: per-lane equality packed into a bitmask (bit i set iff lane i ==
// needle) — the JSON-scanner workhorse. Lanes 2 and 5 are '"' -> mask 0b100100.
TEST(VectorTests, eqMaskBitmask) {
    EXPECT_EQ(runI32(
        "        Vector<int8,16> v = heap Vector<int8,16>(\n"
        "            (int8)97,(int8)97,(int8)34,(int8)97,(int8)97,(int8)34,(int8)97,(int8)97,\n"
        "            (int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97);\n"
        "        int32 m = v.eqMask((int8)34);\n"
        "        return m;\n"), 36);
}

// eqMask + Cajeta.ctz64 -> the index of the first matching lane (== 2 here).
TEST(VectorTests, eqMaskFirstViaCtz) {
    EXPECT_EQ(runI32(
        "        Vector<int8,16> v = heap Vector<int8,16>(\n"
        "            (int8)97,(int8)97,(int8)34,(int8)97,(int8)97,(int8)34,(int8)97,(int8)97,\n"
        "            (int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97);\n"
        "        int32 m = v.eqMask((int8)34);\n"
        "        return Cajeta.ctz64((int64) m);\n"), 2);
}

// vload16 loads a 16-byte block from an int8[]; eqMask classifies it — the SIMD
// scanner's inner loop. Bytes 2 and 5 are '"' -> mask 0b100100.
TEST(VectorTests, vload16ThenEqMask) {
    EXPECT_EQ(runI32(
        "        int8[] b = heap int8[16];\n"
        "        int64 i = 0;\n"
        "        while (i < 16) { b[i] = (int8) 97; i = i + 1; }\n"
        "        b[2] = (int8) 34; b[5] = (int8) 34;\n"
        "        Vector<int8,16> v = Cajeta.vload16(b, 0);\n"
        "        return v.eqMask((int8) 34);\n"), 36);
}

// .r/.g/.b/.a alias the first four lanes.
TEST(VectorTests, colorAliases) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> c = heap Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        return (int32)(c.r + c.b);\n"), 40);  // 10 + 30
}

// Element-wise add of two same-shape vectors.
TEST(VectorTests, elementWiseAdd) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> a = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> b = heap Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Vector<float32,4> s = a + b;\n"
        "        return (int32) s.z;\n"), 33);  // 3 + 30
}

// Scalar broadcast: vec * scalar and scalar * vec.
TEST(VectorTests, scalarBroadcast) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> a = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> h = a * 2.0f;\n"
        "        return (int32) h.w;\n"), 8);   // 4 * 2
    EXPECT_EQ(runI32(
        "        Vector<float32,4> a = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> h = 3.0f * a;\n"
        "        return (int32) h.x;\n"), 3);   // 3 * 1
}

// Integer vectors: element-wise subtract.
TEST(VectorTests, integerVectorSub) {
    EXPECT_EQ(runI32(
        "        Vector<int32,3> a = heap Vector<int32,3>(10, 20, 30);\n"
        "        Vector<int32,3> b = heap Vector<int32,3>(1, 2, 3);\n"
        "        Vector<int32,3> d = a - b;\n"
        "        return d.y;\n"), 18);  // 20 - 2
}

// Indexed read v[i], constant and dynamic index.
TEST(VectorTests, indexRead) {
    EXPECT_EQ(runI32(
        "        Vector<int32,4> v = heap Vector<int32,4>(5, 6, 7, 8);\n"
        "        return v[2];\n"), 7);
    EXPECT_EQ(runI32(
        "        Vector<int32,4> v = heap Vector<int32,4>(5, 6, 7, 8);\n"
        "        int32 i = 3;\n"
        "        return v[i];\n"), 8);
}

// Component assignment v.x = e.
TEST(VectorTests, componentAssign) {
    EXPECT_EQ(runI32(
        "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        v.x = 9.0f;\n"
        "        return (int32)(v.x + v.y);\n"), 11);  // 9 + 2
}

// Indexed assignment v[i] = e.
TEST(VectorTests, indexAssign) {
    EXPECT_EQ(runI32(
        "        Vector<int32,4> v = heap Vector<int32,4>(5, 6, 7, 8);\n"
        "        v[1] = 60;\n"
        "        int32 k = 2;\n"
        "        v[k] = 70;\n"
        "        return v[1] + v[2];\n"), 130);  // 60 + 70
}

// dot(a, b) over a float vector.
TEST(VectorTests, dotProduct) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
        "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 5.0f, 6.0f);\n"
        "        return (int32) a.dot(b);\n"), 32);  // 4 + 10 + 18
}

// Integer dot (DP4a): Vector<int8,4> -> int32. 1*5+2*6+3*7+4*8 = 70.
TEST(VectorTests, integerDotProduct) {
    EXPECT_EQ(runI32(
        "        Vector<int8,4> a = heap Vector<int8,4>(1, 2, 3, 4);\n"
        "        Vector<int8,4> b = heap Vector<int8,4>(5, 6, 7, 8);\n"
        "        return a.dot(b);\n"), 70);
}

// Fused integer dot-add: a.dot(b, acc) = acc + dot(a,b) = 100 + 70 = 170.
TEST(VectorTests, integerDotAccumulate) {
    EXPECT_EQ(runI32(
        "        Vector<int8,4> a = heap Vector<int8,4>(1, 2, 3, 4);\n"
        "        Vector<int8,4> b = heap Vector<int8,4>(5, 6, 7, 8);\n"
        "        return a.dot(b, 100);\n"), 170);
}

// Unsigned dot reads the bytes as uint8 (no sign extension): the signed reading
// of 200/100 would be negative. 200*2+100*3+50*4+25*5 = 1025.
TEST(VectorTests, unsignedIntegerDotProduct) {
    EXPECT_EQ(runI32(
        "        Vector<uint8,4> u = heap Vector<uint8,4>(200, 100, 50, 25);\n"
        "        Vector<uint8,4> w = heap Vector<uint8,4>(2, 3, 4, 5);\n"
        "        return u.dot(w);\n"), 1025);
}

// length((3,4)) == 5.
TEST(VectorTests, lengthHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> v = heap Vector<float32,2>(3.0f, 4.0f);\n"
        "        return (int32) v.length();\n"), 5);
}

// normalize((0,4)) == (0,1); the y lane is 1.
TEST(VectorTests, normalizeHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> v = heap Vector<float32,2>(0.0f, 4.0f);\n"
        "        Vector<float32,2> n = v.normalize();\n"
        "        return (int32)(n.y + 0.5f);\n"), 1);  // 1.0 + 0.5 -> 1
}

// B1 intrinsics A1 — element-wise min/max, scalar-bound clamp, and lerp.
// min((1,5,3),(4,2,6)) = (1,2,3) -> sum 6.
TEST(VectorTests, minHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
        "        Vector<float32,3> m = a.min(b);\n"
        "        return (int32)(m.x + m.y + m.z);\n"), 6);  // 1+2+3
}

// max((1,5,3),(4,2,6)) = (4,5,6) -> sum 15.
TEST(VectorTests, maxHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
        "        Vector<float32,3> m = a.max(b);\n"
        "        return (int32)(m.x + m.y + m.z);\n"), 15);  // 4+5+6
}

// clamp((-2,5,20), 0, 10) = (0,5,10) -> sum 15.
TEST(VectorTests, clampHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> v = heap Vector<float32,3>(-2.0f, 5.0f, 20.0f);\n"
        "        Vector<float32,3> c = v.clamp(0.0f, 10.0f);\n"
        "        return (int32)(c.x + c.y + c.z);\n"), 15);  // 0+5+10
}

// lerp((0,0),(10,20), 0.5) = (5,10) -> sum 15.
TEST(VectorTests, lerpHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> a = heap Vector<float32,2>(0.0f, 0.0f);\n"
        "        Vector<float32,2> b = heap Vector<float32,2>(10.0f, 20.0f);\n"
        "        Vector<float32,2> r = a.lerp(b, 0.5f);\n"
        "        return (int32)(r.x + r.y);\n"), 15);  // 5+10
}

// B1 intrinsics A2 — geometry. cross((1,0,0),(0,1,0)) = (0,0,1) -> z lane 1.
TEST(VectorTests, crossHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> b = heap Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
        "        Vector<float32,3> c = a.cross(b);\n"
        "        return (int32)(c.x + c.y + 2.0f * c.z);\n"), 2);  // (0,0,1): 2*1
}

// reflect((1,-1), (0,1)) = (1,-1) - 2*(-1)*(0,1) = (1,1) -> sum 2.
TEST(VectorTests, reflectHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> i = heap Vector<float32,2>(1.0f, -1.0f);\n"
        "        Vector<float32,2> n = heap Vector<float32,2>(0.0f, 1.0f);\n"
        "        Vector<float32,2> r = i.reflect(n);\n"
        "        return (int32)(r.x + r.y);\n"), 2);  // (1,1)
}

// distance((0,0),(3,4)) = 5.
TEST(VectorTests, distanceHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> a = heap Vector<float32,2>(0.0f, 0.0f);\n"
        "        Vector<float32,2> b = heap Vector<float32,2>(3.0f, 4.0f);\n"
        "        return (int32) a.distance(b);\n"), 5);
}

// refract straight-down through eta=1 is the unchanged ray: i=(0,-1), n=(0,1),
// dot(n,i)=-1, k=1, result = i - (… )*n = (0,-1) -> sum -1.
TEST(VectorTests, refractHelper) {
    EXPECT_EQ(runI32(
        "        Vector<float32,2> i = heap Vector<float32,2>(0.0f, -1.0f);\n"
        "        Vector<float32,2> n = heap Vector<float32,2>(0.0f, 1.0f);\n"
        "        Vector<float32,2> r = i.refract(n, 1.0f);\n"
        "        return (int32)(r.x + r.y);\n"), -1);  // (0,-1)
}

// cross requires 3-component vectors.
TEST(VectorTests, crossNon3DRejected) {
    try {
        runI32(
            "        Vector<float32,2> a = heap Vector<float32,2>(1.0f, 0.0f);\n"
            "        Vector<float32,2> b = heap Vector<float32,2>(0.0f, 1.0f);\n"
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
            "        Vector<int32,2> a = heap Vector<int32,2>(1, 5);\n"
            "        Vector<int32,2> b = heap Vector<int32,2>(4, 2);\n"
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
        "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
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
        "        Vector<float32,4> c = heap Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Vector<float32,3> rgb = c.rgb;\n"
        "        return (int32)(rgb.r + rgb.g + rgb.b);\n"), 60);  // 10+20+30
}

// A swizzle referencing a lane beyond the source is rejected (.xyz on N=2).
TEST(VectorTests, swizzleOutOfRangeRejected) {
    try {
        runI32(
            "        Vector<float32,2> v = heap Vector<float32,2>(1.0f, 2.0f);\n"
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
        "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 3.0f);\n"
        "        int32 ltAny = (a < b).any() ? 1 : 0;\n"     // 1
        "        int32 ltAll = (a < b).all() ? 1 : 0;\n"     // 0
        "        int32 eqAny = (a == b).any() ? 1 : 0;\n"    // 1
        "        return ltAny * 100 + ltAll * 10 + eqAny;\n"), 101);
}

// select: pick a where (a < b), else b -> the element-wise min.
//   a=(1,5,3) b=(4,2,6): (a<b)=(T,F,T) -> (1,2,3) -> sum 6.
TEST(VectorTests, maskSelectIsMin) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
        "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
        "        Vector<float32,3> m = (a < b).select(a, b);\n"
        "        return (int32)(m.x + m.y + m.z);\n"), 6);  // 1+2+3
}

// A scalar RHS broadcasts: (x > 0) is a per-lane mask. x=(-1,2,-3):
//   (x > 0) = (F,T,F) -> select(x, 0) = (0,2,0) (a ReLU) -> sum 2.
TEST(VectorTests, maskScalarBroadcastRelu) {
    EXPECT_EQ(runI32(
        "        Vector<float32,3> x = heap Vector<float32,3>(-1.0f, 2.0f, -3.0f);\n"
        "        Vector<float32,3> z = heap Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
        "        Vector<float32,3> y = (x > 0.0f).select(x, z);\n"
        "        return (int32)(y.x + y.y + y.z);\n"), 2);  // ReLU -> (0,2,0)
}

// B2 — half (float16) and bfloat16 vector element types (ML/graphics dtypes).
// c = (1,2)+(10,20) = (11,22); e = (3,4)+(3,4) = (6,8). c.y + e.x = 22 + 6 = 28.
TEST(VectorTests, halfAndBfloat16Elements) {
    EXPECT_EQ(runI32(
        "        Vector<float16,2> a = heap Vector<float16,2>(1.0f, 2.0f);\n"
        "        Vector<float16,2> b = heap Vector<float16,2>(10.0f, 20.0f);\n"
        "        Vector<float16,2> c = a + b;\n"
        "        Vector<bfloat16,2> d = heap Vector<bfloat16,2>(3.0f, 4.0f);\n"
        "        Vector<bfloat16,2> e = d + d;\n"
        "        return (int32)(float32) c.y + (int32)(float32) e.x;\n"), 28);
}

// A component beyond the lane count is rejected (.w on N=3).
TEST(VectorTests, componentOutOfRangeRejected) {
    try {
        runI32(
            "        Vector<float32,3> v = heap Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
            "        return (int32) v.w;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_COMPONENT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_COMPONENT");
    }
}

// ===== Phase 0.5: chained-op / value-local reassignment codegen =====
// Regression for the xxhash3 SIMD port: chaining several vector->vector ops
// (and reassigning a vector local) must produce correct lanes and must not
// crash codegen. Individual ops were already fine; composing them was not.

// Chained ops, distinct single-assignment locals: a=[1..]; b=a+a; c=b*b -> c[0]=4.
TEST(VectorTests, chainedOpsInt32) {
    EXPECT_EQ(runI32(
        "        Vector<int32,4> a = heap Vector<int32,4>(1, 2, 3, 4);\n"
        "        Vector<int32,4> b = a + a;\n"
        "        Vector<int32,4> c = b * b;\n"
        "        return c[0];\n"), 4);   // (1+1)*(1+1)
}
TEST(VectorTests, chainedOpsInt64x8) {
    EXPECT_EQ(runI32(
        "        Vector<int64,8> a = heap Vector<int64,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
        "        Vector<int64,8> b = a + a;\n"
        "        Vector<int64,8> c = b * b;\n"
        "        return (int32) c[3];\n"), 64);  // (4+4)*(4+4)
}
// Reassigning the same vector local must persist across statements.
TEST(VectorTests, reassignVectorLocalPersists) {
    EXPECT_EQ(runI32(
        "        Vector<int32,4> a = heap Vector<int32,4>(1, 2, 3, 4);\n"
        "        a = a + a;\n"   // 2
        "        a = a + a;\n"   // 4
        "        return a[0];\n"), 4);
}
TEST(VectorTests, reassignVectorLocalPersistsInt64x8) {
    EXPECT_EQ(runI32(
        "        Vector<int64,8> a = heap Vector<int64,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
        "        a = a + a;\n"
        "        a = a + a;\n"
        "        return (int32) a[1];\n"), 8);  // 2*4
}
// Self-referential op (v = v + v / w = v * v) must not crash the compiler.
TEST(VectorTests, selfReferentialVectorOpCompiles) {
    EXPECT_EQ(runI32(
        "        Vector<int64,8> v = heap Vector<int64,8>(2, 2, 2, 2, 2, 2, 2, 2);\n"
        "        Vector<int64,8> w = v * v;\n"
        "        Vector<int64,8> x = v + v;\n"
        "        return (int32)(w[0] + x[0]);\n"), 8);  // 4 + 4
}
// Many live vector locals + a longer chain (the xxhash accumulate shape).
TEST(VectorTests, manyLiveVectorLocals) {
    EXPECT_EQ(runI32(
        "        Vector<int64,8> a = heap Vector<int64,8>(7, 7, 7, 7, 7, 7, 7, 7);\n"
        "        Vector<int64,8> k = heap Vector<int64,8>(3, 3, 3, 3, 3, 3, 3, 3);\n"
        "        Vector<int64,8> key = a ^ k;\n"          // 4
        "        Vector<int64,8> lo = key & 0xFFFFFFFFL;\n" // 4
        "        Vector<int64,8> hi = key >>> 1;\n"        // 2
        "        Vector<int64,8> prod = lo * hi;\n"        // 8
        "        Vector<int64,8> sum = a + key + prod;\n"  // 7+4+8 = 19
        "        return (int32) sum[0];\n"), 19);
}

// Passing a Vector value to a method and reassigning the return must persist
// across loop iterations (the xxhash3 `acc = accStripe(acc, ...)` shape).
TEST(VectorTests, methodReturnReassignPersists) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static Vector<int64,8> dbl(Vector<int64,8> v) { return v + v; }\n"
        "    public static int32 run() {\n"
        "        Vector<int64,8> a = heap Vector<int64,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
        "        int32 i = 0;\n"
        "        while (i < 3) { a = D.dbl(a); i = i + 1; }\n" // ×8
        "        return (int32) a[0];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 8);  // 1 * 2^3
}

// The exact xxhash accStripe shape with the SIMD load/swap intrinsics in a
// chain: d=vload8i64; key=d^vload8i64; lo=key&m; hi=key>>>32; prod=lo*hi;
// acc + vswapPairs(d) + prod. d[i]=i, secret=0, acc=0 -> lane[j]=d[j^1].
TEST(VectorTests, accStripeIntrinsicChain) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] zb = heap int8[64];\n"
        "        int8[] data = heap int8[64];\n"
        "        int8[] sec = heap int8[64];\n"
        "        int64 i = 0;\n"
        "        while (i < 8) { Cajeta.storeU64(zb, i*8, 0L); Cajeta.storeU64(data, i*8, i); Cajeta.storeU64(sec, i*8, 0L); i = i + 1; }\n"
        "        Vector<int64,8> acc = Cajeta.vload8i64(zb, 0);\n"
        "        Vector<int64,8> d = Cajeta.vload8i64(data, 0);\n"
        "        Vector<int64,8> key = d ^ Cajeta.vload8i64(sec, 0);\n"
        "        Vector<int64,8> lo = key & 0xFFFFFFFFL;\n"
        "        Vector<int64,8> hi = key >>> 32;\n"
        "        Vector<int64,8> swapped = Cajeta.vswapPairs(d);\n"
        "        Vector<int64,8> prod = lo * hi;\n"
        "        Vector<int64,8> r = acc + swapped + prod;\n"
        "        return (int32)(Cajeta.vlane(r,0)*1000 + Cajeta.vlane(r,1)*100 + Cajeta.vlane(r,2)*10 + Cajeta.vlane(r,7));\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1036);  // r = [1,0,3,...,6] -> 1*1000+0*100+3*10+6
}

// Constructor arity must equal N.
TEST(VectorTests, constructorArityMismatchRejected) {
    try {
        runI32(
            "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f);\n"
            "        return 0;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_CONSTRUCT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_CONSTRUCT");
    }
}
