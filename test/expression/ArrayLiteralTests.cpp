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

// 1.1.2 — count() reads the runtime length from the array header.

// 1.1.3 — element slots are arbitrary expressions, evaluated in order.

// 1.1.4 — String elements: each slot holds a String reference, in order.
// size() is the byte length of the window.

// 1.1.5 — unify widens a mixed-width numeric literal to the wider type. `big`
// is int64 holding 2^40 (beyond int32 range); if the element type were int32
// the value would not survive. runI64 confirms the full 64-bit value round-trips.

// 1.1.6 — owned object elements: each `heap Box(...)` is stored into its slot
// and the field reads back (spec §2.2.3). Mirrors the working `{...}` object
// array pattern (StreamClassTTests).

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

// collection-literals Unit 5 — the `{...}` array brace form is retired for
// value arrays (was 1.1.8's "brace form still works"). It now fails to compile;
// `[...]` is the replacement. The dispatch-table carve-out (`((T)->R)[] ops =
// {A::f}`) keeps braces and is covered by FunctionArrayTypeTests.
TEST(ArrayLiteralTests, BraceFormRetired) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30};\n"
        "        return xs[1];\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// ---- Unit 2: target-typed inference ----

// 2.1.1 — the declared int64[] target widens int32 literals: the array is laid
// out with int64 slots, so reading back through the int64[] variable is
// self-consistent and sums correctly (an int32 layout would misread at the
// int64 stride). Spec §3.2.2.

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

// 2.1.4 — an empty literal is legal when a target type is present: `int32[] xs
// = []` is a length-0 int32[] (spec §3.4.1 exception). resolveTypes defers the
// empty case to codegen, where the declaration has pushed the target.

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
