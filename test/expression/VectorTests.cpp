//
// Stage 4 of the Vector<T,N> work: host codegen. Construction, component /
// index read + assignment, element-wise arithmetic + scalar broadcast, and the
// dot/length/normalize geometry helpers — all exercised through the host JIT.
//
// Perf note: the value-asserting tests (VectorTests fixture) share ONE compiled
// module. Every test body is a `public static run_<Name>()` method on class
// `test.D`; SetUpTestSuite compiles that module once and each TEST_F just looks
// up + calls its entry. This replaces ~40 full compiles (each dominated by
// Vector<T,N> monomorphization) with a single compile. The rejection tests
// (VectorRejectTests) deliberately fail to compile, so they keep their own
// per-test compile via runI32 and must NOT live in the shared module.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <memory>
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

// One module holding every value-asserting test body as its own static entry.
// Multi-check tests (constructAndComponentRead, scalarBroadcast, indexRead)
// return 0 on success and a distinct nonzero code per failing sub-check.
const std::string MODULE_SRC =
    "package test;\n"
    "public final class D {\n"

    // constructAndComponentRead (two checks) -> 0 on success.
    "    public static int32 run_constructAndComponentRead() {\n"
    "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "        if ((int32) v.y != 2) { return 1; }\n"
    "        if ((int32) v.w != 4) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    "    public static int32 run_eqMaskBitmask() {\n"
    "        Vector<int8,16> v = heap Vector<int8,16>(\n"
    "            (int8)97,(int8)97,(int8)34,(int8)97,(int8)97,(int8)34,(int8)97,(int8)97,\n"
    "            (int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97);\n"
    "        int32 m = v.eqMask((int8)34);\n"
    "        return m;\n"
    "    }\n"

    "    public static int32 run_eqMaskFirstViaCtz() {\n"
    "        Vector<int8,16> v = heap Vector<int8,16>(\n"
    "            (int8)97,(int8)97,(int8)34,(int8)97,(int8)97,(int8)34,(int8)97,(int8)97,\n"
    "            (int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97,(int8)97);\n"
    "        int32 m = v.eqMask((int8)34);\n"
    "        return Cajeta.ctz64((int64) m);\n"
    "    }\n"

    "    public static int32 run_vload16ThenEqMask() {\n"
    "        int8[] b = heap int8[16];\n"
    "        int64 i = 0;\n"
    "        while (i < 16) { b[i] = (int8) 97; i = i + 1; }\n"
    "        b[2] = (int8) 34; b[5] = (int8) 34;\n"
    "        Vector<int8,16> v = Cajeta.vload16(b, 0);\n"
    "        return v.eqMask((int8) 34);\n"
    "    }\n"

    "    public static int32 run_colorAliases() {\n"
    "        Vector<float32,4> c = heap Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
    "        return (int32)(c.r + c.b);\n"
    "    }\n"

    "    public static int32 run_elementWiseAdd() {\n"
    "        Vector<float32,4> a = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "        Vector<float32,4> b = heap Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
    "        Vector<float32,4> s = a + b;\n"
    "        return (int32) s.z;\n"
    "    }\n"

    // scalarBroadcast (two checks) -> 0 on success.
    "    public static int32 run_scalarBroadcast() {\n"
    "        Vector<float32,4> a = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "        Vector<float32,4> h = a * 2.0f;\n"
    "        if ((int32) h.w != 8) { return 1; }\n"
    "        h = 3.0f * a;\n"
    "        if ((int32) h.x != 3) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    "    public static int32 run_integerVectorSub() {\n"
    "        Vector<int32,3> a = heap Vector<int32,3>(10, 20, 30);\n"
    "        Vector<int32,3> b = heap Vector<int32,3>(1, 2, 3);\n"
    "        Vector<int32,3> d = a - b;\n"
    "        return d.y;\n"
    "    }\n"

    // indexRead (two checks) -> 0 on success.
    "    public static int32 run_indexRead() {\n"
    "        Vector<int32,4> v = heap Vector<int32,4>(5, 6, 7, 8);\n"
    "        if (v[2] != 7) { return 1; }\n"
    "        int32 i = 3;\n"
    "        if (v[i] != 8) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    "    public static int32 run_componentAssign() {\n"
    "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "        v.x = 9.0f;\n"
    "        return (int32)(v.x + v.y);\n"
    "    }\n"

    "    public static int32 run_indexAssign() {\n"
    "        Vector<int32,4> v = heap Vector<int32,4>(5, 6, 7, 8);\n"
    "        v[1] = 60;\n"
    "        int32 k = 2;\n"
    "        v[k] = 70;\n"
    "        return v[1] + v[2];\n"
    "    }\n"

    "    public static int32 run_dotProduct() {\n"
    "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
    "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 5.0f, 6.0f);\n"
    "        return (int32) a.dot(b);\n"
    "    }\n"

    "    public static int32 run_integerDotProduct() {\n"
    "        Vector<int8,4> a = heap Vector<int8,4>(1, 2, 3, 4);\n"
    "        Vector<int8,4> b = heap Vector<int8,4>(5, 6, 7, 8);\n"
    "        return a.dot(b);\n"
    "    }\n"

    "    public static int32 run_integerDotAccumulate() {\n"
    "        Vector<int8,4> a = heap Vector<int8,4>(1, 2, 3, 4);\n"
    "        Vector<int8,4> b = heap Vector<int8,4>(5, 6, 7, 8);\n"
    "        return a.dot(b, 100);\n"
    "    }\n"

    "    public static int32 run_unsignedIntegerDotProduct() {\n"
    "        Vector<uint8,4> u = heap Vector<uint8,4>(200, 100, 50, 25);\n"
    "        Vector<uint8,4> w = heap Vector<uint8,4>(2, 3, 4, 5);\n"
    "        return u.dot(w);\n"
    "    }\n"

    "    public static int32 run_lengthHelper() {\n"
    "        Vector<float32,2> v = heap Vector<float32,2>(3.0f, 4.0f);\n"
    "        return (int32) v.length();\n"
    "    }\n"

    "    public static int32 run_normalizeHelper() {\n"
    "        Vector<float32,2> v = heap Vector<float32,2>(0.0f, 4.0f);\n"
    "        Vector<float32,2> n = v.normalize();\n"
    "        return (int32)(n.y + 0.5f);\n"
    "    }\n"

    "    public static int32 run_minHelper() {\n"
    "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
    "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
    "        Vector<float32,3> m = a.min(b);\n"
    "        return (int32)(m.x + m.y + m.z);\n"
    "    }\n"

    "    public static int32 run_maxHelper() {\n"
    "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
    "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
    "        Vector<float32,3> m = a.max(b);\n"
    "        return (int32)(m.x + m.y + m.z);\n"
    "    }\n"

    "    public static int32 run_clampHelper() {\n"
    "        Vector<float32,3> v = heap Vector<float32,3>(-2.0f, 5.0f, 20.0f);\n"
    "        Vector<float32,3> c = v.clamp(0.0f, 10.0f);\n"
    "        return (int32)(c.x + c.y + c.z);\n"
    "    }\n"

    "    public static int32 run_lerpHelper() {\n"
    "        Vector<float32,2> a = heap Vector<float32,2>(0.0f, 0.0f);\n"
    "        Vector<float32,2> b = heap Vector<float32,2>(10.0f, 20.0f);\n"
    "        Vector<float32,2> r = a.lerp(b, 0.5f);\n"
    "        return (int32)(r.x + r.y);\n"
    "    }\n"

    "    public static int32 run_crossHelper() {\n"
    "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
    "        Vector<float32,3> b = heap Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
    "        Vector<float32,3> c = a.cross(b);\n"
    "        return (int32)(c.x + c.y + 2.0f * c.z);\n"
    "    }\n"

    "    public static int32 run_reflectHelper() {\n"
    "        Vector<float32,2> i = heap Vector<float32,2>(1.0f, -1.0f);\n"
    "        Vector<float32,2> n = heap Vector<float32,2>(0.0f, 1.0f);\n"
    "        Vector<float32,2> r = i.reflect(n);\n"
    "        return (int32)(r.x + r.y);\n"
    "    }\n"

    "    public static int32 run_distanceHelper() {\n"
    "        Vector<float32,2> a = heap Vector<float32,2>(0.0f, 0.0f);\n"
    "        Vector<float32,2> b = heap Vector<float32,2>(3.0f, 4.0f);\n"
    "        return (int32) a.distance(b);\n"
    "    }\n"

    "    public static int32 run_refractHelper() {\n"
    "        Vector<float32,2> i = heap Vector<float32,2>(0.0f, -1.0f);\n"
    "        Vector<float32,2> n = heap Vector<float32,2>(0.0f, 1.0f);\n"
    "        Vector<float32,2> r = i.refract(n, 1.0f);\n"
    "        return (int32)(r.x + r.y);\n"
    "    }\n"

    "    public static int32 run_swizzleReads() {\n"
    "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "        Vector<float32,2> xy = v.xy;\n"
    "        Vector<float32,3> zyx = v.zyx;\n"
    "        Vector<float32,4> xxyy = v.xxyy;\n"
    "        return (int32)(xy.x * 1.0f + xy.y * 10.0f\n"
    "                     + zyx.x * 100.0f\n"
    "                     + xxyy.x + xxyy.y + xxyy.z + xxyy.w);\n"
    "    }\n"

    "    public static int32 run_swizzleColorAlias() {\n"
    "        Vector<float32,4> c = heap Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
    "        Vector<float32,3> rgb = c.rgb;\n"
    "        return (int32)(rgb.r + rgb.g + rgb.b);\n"
    "    }\n"

    "    public static int32 run_comparisonMaskAnyAll() {\n"
    "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
    "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 3.0f);\n"
    "        int32 ltAny = (a < b).any() ? 1 : 0;\n"
    "        int32 ltAll = (a < b).all() ? 1 : 0;\n"
    "        int32 eqAny = (a == b).any() ? 1 : 0;\n"
    "        return ltAny * 100 + ltAll * 10 + eqAny;\n"
    "    }\n"

    "    public static int32 run_maskSelectIsMin() {\n"
    "        Vector<float32,3> a = heap Vector<float32,3>(1.0f, 5.0f, 3.0f);\n"
    "        Vector<float32,3> b = heap Vector<float32,3>(4.0f, 2.0f, 6.0f);\n"
    "        Vector<float32,3> m = (a < b).select(a, b);\n"
    "        return (int32)(m.x + m.y + m.z);\n"
    "    }\n"

    "    public static int32 run_maskScalarBroadcastRelu() {\n"
    "        Vector<float32,3> x = heap Vector<float32,3>(-1.0f, 2.0f, -3.0f);\n"
    "        Vector<float32,3> z = heap Vector<float32,3>(0.0f, 0.0f, 0.0f);\n"
    "        Vector<float32,3> y = (x > 0.0f).select(x, z);\n"
    "        return (int32)(y.x + y.y + y.z);\n"
    "    }\n"

    "    public static int32 run_halfAndBfloat16Elements() {\n"
    "        Vector<float16,2> a = heap Vector<float16,2>(1.0f, 2.0f);\n"
    "        Vector<float16,2> b = heap Vector<float16,2>(10.0f, 20.0f);\n"
    "        Vector<float16,2> c = a + b;\n"
    "        Vector<bfloat16,2> d = heap Vector<bfloat16,2>(3.0f, 4.0f);\n"
    "        Vector<bfloat16,2> e = d + d;\n"
    "        return (int32)(float32) c.y + (int32)(float32) e.x;\n"
    "    }\n"

    "    public static int32 run_chainedOpsInt32() {\n"
    "        Vector<int32,4> a = heap Vector<int32,4>(1, 2, 3, 4);\n"
    "        Vector<int32,4> b = a + a;\n"
    "        Vector<int32,4> c = b * b;\n"
    "        return c[0];\n"
    "    }\n"

    "    public static int32 run_chainedOpsInt64x8() {\n"
    "        Vector<int64,8> a = heap Vector<int64,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
    "        Vector<int64,8> b = a + a;\n"
    "        Vector<int64,8> c = b * b;\n"
    "        return (int32) c[3];\n"
    "    }\n"

    "    public static int32 run_reassignVectorLocalPersists() {\n"
    "        Vector<int32,4> a = heap Vector<int32,4>(1, 2, 3, 4);\n"
    "        a = a + a;\n"
    "        a = a + a;\n"
    "        return a[0];\n"
    "    }\n"

    "    public static int32 run_reassignVectorLocalPersistsInt64x8() {\n"
    "        Vector<int64,8> a = heap Vector<int64,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
    "        a = a + a;\n"
    "        a = a + a;\n"
    "        return (int32) a[1];\n"
    "    }\n"

    "    public static int32 run_selfReferentialVectorOpCompiles() {\n"
    "        Vector<int64,8> v = heap Vector<int64,8>(2, 2, 2, 2, 2, 2, 2, 2);\n"
    "        Vector<int64,8> w = v * v;\n"
    "        Vector<int64,8> x = v + v;\n"
    "        return (int32)(w[0] + x[0]);\n"
    "    }\n"

    "    public static int32 run_manyLiveVectorLocals() {\n"
    "        Vector<int64,8> a = heap Vector<int64,8>(7, 7, 7, 7, 7, 7, 7, 7);\n"
    "        Vector<int64,8> k = heap Vector<int64,8>(3, 3, 3, 3, 3, 3, 3, 3);\n"
    "        Vector<int64,8> key = a ^ k;\n"
    "        Vector<int64,8> lo = key & 0xFFFFFFFFL;\n"
    "        Vector<int64,8> hi = key >>> 1;\n"
    "        Vector<int64,8> prod = lo * hi;\n"
    "        Vector<int64,8> sum = a + key + prod;\n"
    "        return (int32) sum[0];\n"
    "    }\n"

    // methodReturnReassignPersists: needs the `dbl` helper below.
    "    public static Vector<int64,8> dbl(Vector<int64,8> v) { return v + v; }\n"
    "    public static int32 run_methodReturnReassignPersists() {\n"
    "        Vector<int64,8> a = heap Vector<int64,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
    "        int32 i = 0;\n"
    "        while (i < 3) { a = D.dbl(a); i = i + 1; }\n"
    "        return (int32) a[0];\n"
    "    }\n"

    "    public static int32 run_accStripeIntrinsicChain() {\n"
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

} // namespace

// Value-asserting tests: one shared compile, per-test entry lookup.
class VectorTests : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        jit = CajetaJit::compile(MODULE_SRC, "test.D");
    }
    static void TearDownTestSuite() {
        jit.reset();
    }
    static int32_t i32(const char* name) {
        auto fn = jit->lookup<int32_t (*)()>(name);
        return fn();
    }
    static float f32(const char* name) {
        auto fn = jit->lookup<float (*)()>(name);
        return fn();
    }
    static std::unique_ptr<cajeta_test::CajetaJit> jit;
};

std::unique_ptr<cajeta_test::CajetaJit> VectorTests::jit;

// Construct and read a component by name (.y) and by color alias (.g == .y).

// SIMD eqMask: per-lane equality packed into a bitmask. Lanes 2 and 5 -> 0b100100.
TEST_F(VectorTests, eqMaskBitmask) { EXPECT_EQ(i32("run_eqMaskBitmask"), 36); }

// eqMask + Cajeta.ctz64 -> index of the first matching lane (== 2).
TEST_F(VectorTests, eqMaskFirstViaCtz) { EXPECT_EQ(i32("run_eqMaskFirstViaCtz"), 2); }

// vload16 loads a 16-byte block; eqMask classifies it. Bytes 2 and 5 -> 0b100100.
TEST_F(VectorTests, vload16ThenEqMask) { EXPECT_EQ(i32("run_vload16ThenEqMask"), 36); }

// .r/.g/.b/.a alias the first four lanes.

// Element-wise add of two same-shape vectors.

// Scalar broadcast: vec * scalar and scalar * vec.

// Integer vectors: element-wise subtract.

// Indexed read v[i], constant and dynamic index.

// Component assignment v.x = e.

// Indexed assignment v[i] = e.

// dot(a, b) over a float vector.

// Integer dot (DP4a): Vector<int8,4> -> int32.

// Fused integer dot-add: a.dot(b, acc) = acc + dot(a,b).

// Unsigned dot reads the bytes as uint8 (no sign extension).

// length((3,4)) == 5.

// normalize((0,4)) == (0,1); the y lane is 1.
TEST_F(VectorTests, normalizeHelper) { EXPECT_EQ(i32("run_normalizeHelper"), 1); }

// min((1,5,3),(4,2,6)) = (1,2,3) -> sum 6.

// max((1,5,3),(4,2,6)) = (4,5,6) -> sum 15.

// clamp((-2,5,20), 0, 10) = (0,5,10) -> sum 15.

// lerp((0,0),(10,20), 0.5) = (5,10) -> sum 15.

// cross((1,0,0),(0,1,0)) = (0,0,1) -> z lane 1.

// reflect((1,-1), (0,1)) = (1,1) -> sum 2.

// distance((0,0),(3,4)) = 5.

// refract straight-down through eta=1 is the unchanged ray -> sum -1.

// Multi-component swizzle reads.
TEST_F(VectorTests, swizzleReads) { EXPECT_EQ(i32("run_swizzleReads"), 327); }

// Color aliases swizzle too: c.rgb = (10,20,30).
TEST_F(VectorTests, swizzleColorAlias) { EXPECT_EQ(i32("run_swizzleColorAlias"), 60); }

// Comparison masks + all/any/select.

// select: pick a where (a < b), else b -> the element-wise min.

// A scalar RHS broadcasts: (x > 0).select(x, 0) is a ReLU -> sum 2.

// half (float16) and bfloat16 vector element types.

// Chained ops, distinct single-assignment locals.
TEST_F(VectorTests, chainedOpsInt32) { EXPECT_EQ(i32("run_chainedOpsInt32"), 4); }
TEST_F(VectorTests, chainedOpsInt64x8) { EXPECT_EQ(i32("run_chainedOpsInt64x8"), 64); }

// Reassigning the same vector local must persist across statements.
TEST_F(VectorTests, reassignVectorLocalPersists) { EXPECT_EQ(i32("run_reassignVectorLocalPersists"), 4); }
TEST_F(VectorTests, reassignVectorLocalPersistsInt64x8) { EXPECT_EQ(i32("run_reassignVectorLocalPersistsInt64x8"), 8); }

// Self-referential op must not crash the compiler.
TEST_F(VectorTests, selfReferentialVectorOpCompiles) { EXPECT_EQ(i32("run_selfReferentialVectorOpCompiles"), 8); }

// Many live vector locals + a longer chain (the xxhash accumulate shape).
TEST_F(VectorTests, manyLiveVectorLocals) { EXPECT_EQ(i32("run_manyLiveVectorLocals"), 19); }

// Passing a Vector value to a method and reassigning the return must persist.
TEST_F(VectorTests, methodReturnReassignPersists) { EXPECT_EQ(i32("run_methodReturnReassignPersists"), 8); }

// The exact xxhash accStripe shape with SIMD load/swap intrinsics in a chain.
TEST_F(VectorTests, accStripeIntrinsicChain) { EXPECT_EQ(i32("run_accStripeIntrinsicChain"), 1036); }

// ===== Rejection tests: each deliberately fails to compile, so it keeps its
// own per-test compile and must stay OUT of the shared module. =====

// cross requires 3-component vectors.

// Integer min/max is a follow-on — rejected for now with a clean diagnostic.

// A swizzle referencing a lane beyond the source is rejected (.xyz on N=2).
TEST(VectorRejectTests, swizzleOutOfRangeRejected) {
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

// A component beyond the lane count is rejected (.w on N=3).
TEST(VectorRejectTests, componentOutOfRangeRejected) {
    try {
        runI32(
            "        Vector<float32,3> v = heap Vector<float32,3>(1.0f, 2.0f, 3.0f);\n"
            "        return (int32) v.w;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_COMPONENT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_COMPONENT");
    }
}

// Constructor arity must equal N.
TEST(VectorRejectTests, constructorArityMismatchRejected) {
    try {
        runI32(
            "        Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f);\n"
            "        return 0;\n");
        FAIL() << "expected CAJETA_ERROR_VECTOR_CONSTRUCT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_CONSTRUCT");
    }
}
