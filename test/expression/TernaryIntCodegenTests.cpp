//
// Integer-result ternary (`cond ? a : b`) codegen soundness (ternary-int-codegen
// plan Unit 1; spec §2). `int32 x = boolCond ? a*2 : 128;` emitted a malformed
// ICmp (mismatched operands) -> LLVM assertion at codegen. These pin the fix:
// valid IR + correct value across both branches, for field/local/comparison
// conditions, arithmetic arms, and mixed-width arms.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

// 2.1.1 / 2.1.2 — the exact StringBuilder.grow shape: boolean FIELD condition,
// arithmetic arm, literal arm. flag=true -> cap*2; flag=false -> 128.
TEST(TernaryIntCodegenTests, BoolFieldCondArithVsLiteralArm) {
    const char* src =
        "package test;\n"
        "public final class T {\n"
        "    boolean flag;\n"
        "    int32 cap;\n"
        "    public T(boolean f, int32 c) { this.flag = f; this.cap = c; }\n"
        "    public int32 pick() { int32 x = this.flag ? this.cap * 2 : 128; return x; }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 runTrue()  { T t = heap T(true, 50);  return t.pick(); }\n"
        "    public static int32 runFalse() { T t = heap T(false, 50); return t.pick(); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto t = jit->lookup<int32_t (*)()>("runTrue");
    auto f = jit->lookup<int32_t (*)()>("runFalse");
    ASSERT_NE(t, nullptr);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(t(), 100); // 50 * 2
    EXPECT_EQ(f(), 128);
}

// 2.1.2 — boolean LOCAL condition + arithmetic arm.
TEST(TernaryIntCodegenTests, BoolLocalCondArithArm) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static int32 pick(boolean c, int32 a) { int32 x = c ? a * 2 : 128; return x; }\n"
        "    public static int32 runTrue()  { return pick(true, 7); }\n"
        "    public static int32 runFalse() { return pick(false, 7); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runTrue")(), 14);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runFalse")(), 128);
}

// 2.1.2 — comparison condition (`a > b`).
TEST(TernaryIntCodegenTests, ComparisonCond) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static int32 pick(int32 a, int32 b) { int32 x = a > b ? a * 2 : 128; return x; }\n"
        "    public static int32 runHi() { return pick(10, 3); }\n"
        "    public static int32 runLo() { return pick(3, 10); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runHi")(), 20);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runLo")(), 128);
}

// 2.1.3 — mixed-width arms: int32 result with an int64 arithmetic arm + int8 arm.
TEST(TernaryIntCodegenTests, MixedWidthArms) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static int32 pick(boolean c, int64 a) {\n"
        "        int32 x = c ? (int32)(a * 2) : 7;\n"
        "        return x;\n"
        "    }\n"
        "    public static int32 runTrue()  { return pick(true, 21); }\n"
        "    public static int32 runFalse() { return pick(false, 21); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runTrue")(), 42);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runFalse")(), 7);
}

// 2.1.5 — simple identifier/literal-arm ternary keeps working (non-regression).
TEST(TernaryIntCodegenTests, SimpleLiteralArmsUnchanged) {
    const char* src =
        "package test;\n"
        "public final class A {\n"
        "    static int32 pick(boolean c) { int32 x = c ? 1 : 2; return x; }\n"
        "    public static int32 runTrue()  { return pick(true); }\n"
        "    public static int32 runFalse() { return pick(false); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runTrue")(), 1);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("runFalse")(), 2);
}
