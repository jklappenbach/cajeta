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
