//
// S2 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): a @ValueType class dispatches its
// static operator overloads through the normal mechanism, and the comparison
// derivations (!= from ==, > / <= / >= from <) fire. (A @ValueType class carries
// VALUE_TYPE_FLAG but NOT PRIMITIVE_FLAG, so it already passes the dispatch gate
// like any non-primitive class; the explicit `|| VALUE_TYPE_FLAG` clause is
// defensive. These tests pin the behavior either way.)
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// A @ValueType Vec2 with the canonical static operator surface, plus a driver
// whose run() body the individual tests fill in.
std::string withVec2(const std::string& body) {
    return
        "package test;\n"
        "@ValueType public final class Vec2 {\n"
        "    public float32 x;\n"
        "    public float32 y;\n"
        "    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }\n"
        "    public static Vec2 operator+ (Vec2 a, Vec2 b) {\n"
        "        return stack Vec2(a.x + b.x, a.y + b.y);\n"
        "    }\n"
        "    public static Vec2 operator- (Vec2 a, Vec2 b) {\n"
        "        return stack Vec2(a.x - b.x, a.y - b.y);\n"
        "    }\n"
        "    public static Vec2 operator* (Vec2 v, float32 k) {\n"
        "        return stack Vec2(v.x * k, v.y * k);\n"
        "    }\n"
        "    public static boolean operator== (Vec2 a, Vec2 b) {\n"
        "        return a.x == b.x && a.y == b.y;\n"
        "    }\n"
        "    public static boolean operator< (Vec2 a, Vec2 b) {\n"
        "        return a.x < b.x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// a + b dispatches to the static operator+ (a class-typed `+` has no builtin, so
// a correct sum proves dispatch happened).
TEST(ValueTypeOperatorHostTests, binaryAddDispatches) {
    auto src = withVec2(
        "        Vec2 a = stack Vec2(1.0f, 2.0f);\n"
        "        Vec2 b = stack Vec2(3.0f, 4.0f);\n"
        "        Vec2 c = a + b;\n"
        "        return (int32)(c.x + c.y);\n");   // (4) + (6) = 10
    EXPECT_EQ(runI32(src), 10);
}

// a - b and the mixed v * scalar overload both dispatch.
TEST(ValueTypeOperatorHostTests, subAndScalarMulDispatch) {
    auto src = withVec2(
        "        Vec2 a = stack Vec2(5.0f, 7.0f);\n"
        "        Vec2 b = stack Vec2(1.0f, 2.0f);\n"
        "        Vec2 d = a - b;\n"          // (4, 5)
        "        Vec2 e = d * 2.0f;\n"        // (8, 10)
        "        return (int32)(e.x + e.y);\n");  // 18
    EXPECT_EQ(runI32(src), 18);
}

// == dispatches and != derives from it.
TEST(ValueTypeOperatorHostTests, equalityAndDerivedInequality) {
    auto src = withVec2(
        "        Vec2 a = stack Vec2(1.0f, 2.0f);\n"
        "        Vec2 b = stack Vec2(1.0f, 2.0f);\n"
        "        Vec2 c = stack Vec2(9.0f, 9.0f);\n"
        "        int32 eq = (a == b) ? 1 : 0;\n"
        "        int32 ne = (a != c) ? 10 : 0;\n"   // derived from ==
        "        return eq + ne;\n");
    EXPECT_EQ(runI32(src), 11);
}

// < dispatches; >, <=, >= derive from it.
TEST(ValueTypeOperatorHostTests, lessThanAndDerivedComparisons) {
    auto src = withVec2(
        "        Vec2 a = stack Vec2(1.0f, 0.0f);\n"
        "        Vec2 b = stack Vec2(3.0f, 0.0f);\n"
        "        int32 lt = (a < b)  ? 1 : 0;\n"   // true
        "        int32 gt = (b > a)  ? 2 : 0;\n"   // derived (a < b) -> true
        "        int32 le = (a <= b) ? 4 : 0;\n"   // derived
        "        int32 ge = (b >= a) ? 8 : 0;\n"   // derived
        "        return lt + gt + le + ge;\n");
    EXPECT_EQ(runI32(src), 15);
}
