//
// array-literals Unit 1: `[e1, e2, ...]` as a real value-producing expression.
// Element type is inferred by unify (no target-typing yet — that's Unit 2);
// storage is heap; both `[...]` and the existing `{...}` form share one store
// loop (spec §7), so the last test guards that the brace form is unchanged.
//
// Harness mirrors test/parser/ArrayInitializerTests.cpp: compile a snippet,
// look up `run`, call it.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// 1.1.1 — three int32 literals; sum all slots. Smallest end-to-end case.
TEST(ArrayLiteralTests, PrimitiveLiteral) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [10, 20, 30];\n"
        "        return xs[0] + xs[1] + xs[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

// 1.1.2 — count() reads the runtime length from the array header.
TEST(ArrayLiteralTests, LengthFromHeader) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        return (int32) xs.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 1.1.3 — element slots are arbitrary expressions, evaluated in order.
TEST(ArrayLiteralTests, ExpressionsAsElements) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        int32[] xs = [base, base + 5, base * 2];\n"
        "        return xs[0] + xs[1] + xs[2];\n"  // 10 + 15 + 20
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 45);
}

// 1.1.4 — String elements: each slot holds a String reference, in order.
// size() is the byte length of the window.
TEST(ArrayLiteralTests, StringLiteralArray) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String[] s = [\"ann\", \"bo\"];\n"
        "        return (int32) (s[0].size() + s[1].size());\n"  // 3 + 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 1.1.5 — unify widens a mixed-width numeric literal to the wider type. `big`
// is int64 holding 2^40 (beyond int32 range); if the element type were int32
// the value would not survive. runI64 confirms the full 64-bit value round-trips.
TEST(ArrayLiteralTests, UnifyWidensToInt64) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32 a = 1;\n"
        "        int64 big = 1;\n"
        "        big = big << 40;\n"
        "        int64[] xs = [a, big];\n"  // unify {int32, int64} -> int64
        "        return xs[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 1099511627776LL);  // 2^40
}

// 1.1.6 — owned object elements: each `heap Box(...)` is stored into its slot
// and the field reads back (spec §2.2.3). Mirrors the working `{...}` object
// array pattern (StreamClassTTests).
TEST(ArrayLiteralTests, OwnedObjectElements) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box[] xs = [heap Box(7), heap Box(9)];\n"
        "        return xs[0].v + xs[1].v;\n"  // 16
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 16);
}

// 1.1.7 — an empty literal with no target type cannot be inferred. Argument
// position never receives target propagation (spec §1.4.2/§3.3.2), so `g([])`
// is a stable no-target site: unify over zero elements must fail to compile.
TEST(ArrayLiteralTests, EmptyLiteralNoTargetErrors) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 g(int32[] xs) { return (int32) xs.count(); }\n"
        "    public static int32 run() {\n"
        "        return g([]);\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 1.1.8 — the existing `{...}` brace form still works after being routed
// through the shared store loop (§7 regression guard).
TEST(ArrayLiteralTests, BraceFormStillWorks) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30};\n"
        "        return xs[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// ---- Unit 2: target-typed inference ----

// 2.1.1 — the declared int64[] target widens int32 literals: the array is laid
// out with int64 slots, so reading back through the int64[] variable is
// self-consistent and sums correctly (an int32 layout would misread at the
// int64 stride). Spec §3.2.2.
TEST(ArrayLiteralTests, TargetWidensDeclaration) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64[] xs = [100, 200, 300];\n"  // literals int32; target int64[]
        "        return xs[0] + xs[1] + xs[2];\n"   // 600 iff int64 layout
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 600);
}

// 2.1.2 — return-position target: the method's int64[] return type widens the
// returned literal. Read back through an int64[] local.
TEST(ArrayLiteralTests, TargetFromReturn) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64[] make() { return [1, 2, 3]; }\n"
        "    public static int64 run() {\n"
        "        int64[] xs = make();\n"
        "        return xs[0] + xs[1] + xs[2];\n"  // 6 iff int64 layout
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 6);
}

// 2.1.3 — assignment-position target: `int64[] xs; xs = [...]` widens from the
// LHS array type.
TEST(ArrayLiteralTests, TargetFromAssignment) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64[] xs;\n"
        "        xs = [10, 20, 30];\n"
        "        return xs[0] + xs[1] + xs[2];\n"  // 60 iff int64 layout
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 60);
}

// 2.1.4 — an empty literal is legal when a target type is present: `int32[] xs
// = []` is a length-0 int32[] (spec §3.4.1 exception). resolveTypes defers the
// empty case to codegen, where the declaration has pushed the target.
TEST(ArrayLiteralTests, EmptyWithTargetOk) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [];\n"
        "        return (int32) xs.count();\n"  // 0
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 2.1.5 — elements with no common type and no target fail to compile (spec
// §3.4.2). Two unrelated classes in an argument-position literal (no target).
TEST(ArrayLiteralTests, NoCommonTypeNoTargetErrors) {
    auto src =
        "package test;\n"
        "public class A { public A() {} }\n"
        "public class B { public B() {} }\n"
        "public final class D {\n"
        "    public static int32 g(A[] xs) { return (int32) xs.count(); }\n"
        "    public static int32 run() {\n"
        "        return g([heap A(), heap B()]);\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 2.1.6 — argument position is never target-propagated (spec §1.4.2/§3.3.2):
// the literal is typed by unify (int32[]) and the concrete type drives overload
// resolution. `g` takes int32[]; the unified int32[] matches.
TEST(ArrayLiteralTests, ArgPositionUnifyOnly) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 g(int32[] xs) {\n"
        "        return xs[0] + xs[1] + xs[2];\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return g([4, 5, 6]);\n"  // 15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// ---- Unit 4: nested literals ----

// 4.1.1 — nested rectangular: `int32[][]` holds inner `int32[]` arrays; double
// indexing reads the right element.
TEST(ArrayLiteralTests, NestedRectangular) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[][] g = [[1, 2], [3, 4]];\n"
        "        return g[1][0];\n"  // 3
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 4.1.2 — jagged: inner arrays of differing lengths, each sized to its own literal.
TEST(ArrayLiteralTests, NestedJagged) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[][] g = [[1], [2, 3]];\n"
        "        return (int32)(g[0].count() * 100 + g[1].count() * 10 + g[1][1]);\n"
        "    }\n"                                     // 1*100 + 2*10 + 3 = 123
        "}\n";
    EXPECT_EQ(runI32(src), 123);
}

// 4.1.3 — nested WIDENING via target (the review C2 fix): `int64[][]` must push
// int64 into each inner literal, else the inner arrays stay int32-laid-out and
// reads at the int64 stride are OOB/garbage.
TEST(ArrayLiteralTests, NestedTargetWidens) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64[][] g = [[1, 2], [3, 4]];\n"
        "        return g[1][0] + g[1][1];\n"  // 7 iff inner arrays are int64
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 7);
}

// 4.1.4 — no-target nested unify: `[[..],[..]]` in argument position (never
// target-propagated) unifies to int32[][] from the inner array types and
// matches the int32[][] parameter.
TEST(ArrayLiteralTests, NestedUnifyNoTarget) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 g(int32[][] m) { return m[1][0] + m[0][1]; }\n"
        "    public static int32 run() {\n"
        "        return g([[10, 20], [30, 40]]);\n"  // 30 + 20 = 50
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}
