//
// Ternary and instanceof expression tests.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class T {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- ternary -------------------------------------------------------------------

TEST(TernaryTests, simpleTrue) {
    auto jit = CajetaJit::compile(makeSource("int32", "return true ? 7 : 11;"), "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 7);
}

TEST(TernaryTests, simpleFalse) {
    auto jit = CajetaJit::compile(makeSource("int32", "return false ? 7 : 11;"), "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

TEST(TernaryTests, conditionFromComparison) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "int32 b = 10;\n"
        "return a < b ? a : b;"), "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

TEST(TernaryTests, floatBranches) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "boolean pick = true;\n"
        "return pick ? 1.5 : 2.5;"), "test.T");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 1.5);
}

TEST(TernaryTests, intCoercedToFloat) {
    // Then-branch is float (driven by literal), else is int — codegen narrows/extends to
    // the then type.
    auto jit = CajetaJit::compile(makeSource("float64",
        "boolean cond = false;\n"
        "return cond ? 1.5 : 2;"), "test.T");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 2.0);
}

TEST(TernaryTests, sideEffectOnSelectedBranch) {
    // Verify that only the selected branch's side effect fires by writing into a local
    // and observing the result.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 x = 0;\n"
        "boolean pick = true;\n"
        "int32 result = pick ? (x = 100) : (x = 200);\n"
        "return x;"), "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

TEST(TernaryTests, nestedTernary) {
    // `a < b ? a : (b < c ? b : c)` — min of three.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "int32 b = 3;\n"
        "int32 c = 8;\n"
        "return a < b ? a : (b < c ? b : c);"), "test.T");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// --- instanceof ----------------------------------------------------------------
//
// instanceof currently is a compile-time static-type match against the lhs's resolved
// type. Real RTTI / class-hierarchy walks are future work; these tests pin down the
// observable behavior of the present implementation.

TEST(InstanceOfTests, matchingPrimitive) {
    // int32 a is statically int32, so `a instanceof int32` is compile-time true.
    auto jit = CajetaJit::compile(makeSource("boolean",
        "int32 a = 5;\n"
        "return a instanceof int32;"), "test.T");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(InstanceOfTests, mismatchingPrimitive) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "int32 a = 5;\n"
        "return a instanceof float64;"), "test.T");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

// nucleo-nn U2 — the "real RTTI / class-hierarchy walks are future work" note
// above is now half-retired: a CLASS-typed lhs whose static type differs from
// the target (the plain downcast question — an Object-typed value from a
// reflective walk, a base-typed ref) asks the RTTI at runtime instead of
// folding to false, and the runtime is-a walk is TRANSITIVE (parent names
// resolve through the class registry, level by level).
TEST(InstanceOfTests, objectLhsAsksRttiAndChainIsTransitive) {
    std::string src =
        "package test;\n"
        "public class A { public int32 tag() { return 1; } }\n"
        "public class B extends A { }\n"
        "public class C extends B { }\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        C c = heap C();\n"
        "        Object o = c;\n"
        "        int32 acc = 0;\n"
        "        if (o instanceof C x) { acc = acc + 1; }\n"       // exact
        "        if (o instanceof B y) { acc = acc + 10; }\n"      // one level
        "        if (o instanceof A z) { acc = acc + 100; }\n"     // transitive
        "        A a = heap A();\n"
        "        Object oa = a;\n"
        "        if (oa instanceof C w) { acc = acc + 1000; }\n"   // downcast miss
        "        return acc;\n"
        "    }\n"
        "}\n";
    auto jit = cajeta_test::CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 111);
}
