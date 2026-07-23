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
