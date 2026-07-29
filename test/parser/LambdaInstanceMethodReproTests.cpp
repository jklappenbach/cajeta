// Regression coverage for the lambda-capture walker gap exposed by
// the Task #41 correlation tests.
//
// Original symptom (now fixed): a block-body lambda with a non-void
// return type calling an instance method on a captured class
// reference dropped the `this` argument at codegen; LLVM verify
// flagged the call as having the wrong arity.
//
// Root cause: `collectFreeIdentifiers` / `collectTransferNames` /
// `enforceValueCaptureImmutability` in Expression.cpp walked the
// lambda body via the generic `getChildren()` fallback for any
// Statement type they didn't have an explicit handler for. Several
// Statement subclasses (LabelStatement — the wrapper for an
// `if`-branch block — plus ScopeStatement, ForStatement,
// EnhancedForStatement, WhileStatement, DoStatement) hold their
// inner block / sub-statements as private members that are NOT in
// the `children` list. The walker silently skipped them. Free
// identifiers inside those nested bodies were never collected, so
// the lambda's captures struct was missing the corresponding slots
// and runtime identifier lookup against the lambda's main scope
// returned null. The downstream MethodCallExpression read a null
// receiver, `thisValue` stayed null, and `CajetaClass::invokeMethod`
// emitted a call with zero arguments instead of one (this).
//
// Fix: explicit walker handlers for each of those Statement types
// + getters on the Statement subclasses so the walkers can descend.
//
// These tests pin the four shapes the bug originally surfaced
// through: bool-return + arg-bearing instance call inside an
// if-then block, bool-return + no-arg call inside if-then,
// and the passing baselines (expr-body, block-body, void-return
// equivalents) that the bug never affected.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

static int32_t runI32(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        return jit->lookup<int32_t (*)()>("run")();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                      << ": " << e.getMessage();
        return -1;
    }
}

// Expression-body lambda, direct call — known to work.
TEST(LambdaInstanceMethodReproTests, exprBodyDirectCall) {
    auto src = std::string(
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 0; }\n"
        "    public void bump(int32 x) {\n"
        "        this.v = this.v + x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        (int32) -> void fn = (x) -> b.bump(x);\n"
        "        fn(7);\n"
        "        fn(35);\n"
        "        return b.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}

// Block-body lambda calling captured instance method via direct
// call. This is the shape that the correlation tests originally hit
// inside findFirst — does it fail in isolation too?
TEST(LambdaInstanceMethodReproTests, blockBodyDirectCall) {
    auto src = std::string(
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 0; }\n"
        "    public void bump(int32 x) {\n"
        "        this.v = this.v + x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        (int32) -> void fn = (x) -> { b.bump(x); };\n"
        "        fn(7);\n"
        "        fn(35);\n"
        "        return b.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}

// Block-body lambda calling captured instance method, passed to a
// helper that accepts a fn parameter and invokes it. Closer to the
// shape inside a stream terminal.
TEST(LambdaInstanceMethodReproTests, blockBodyPassedToHelper) {
    auto src = std::string(
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 0; }\n"
        "    public void bump(int32 x) {\n"
        "        this.v = this.v + x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static void apply((int32) -> void f, int32 x) {\n"
        "        f(x);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        apply((x) -> { b.bump(x); }, 42);\n"
        "        return b.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}

// Block-body lambda, void return, calling a NO-ARG instance method
// on captured class. Narrows whether the trigger is no-arg method
// or boolean-return-block.
TEST(LambdaInstanceMethodReproTests, blockBodyVoidWithNoArgInstanceCall) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static void apply((int32) -> void f, int32 x) {\n"
        "        f(x);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        apply((x) -> { c.bump(); }, 0);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 1);
}

// Block-body lambda, BOOLEAN return, calling instance method that
// TAKES AN ARG (b.bump(x)). Does the boolean-return-block trigger
// fire even with arg-bearing method? If this passes and the
// no-arg-method version fails, trigger is the no-arg method.
TEST(LambdaInstanceMethodReproTests, blockBodyBooleanReturnWithArgInstanceCall) {
    auto src = std::string(
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 0; }\n"
        "    public void bump(int32 x) {\n"
        "        this.v = this.v + x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 apply((int32) -> boolean pred, int32 x) {\n"
        "        if (pred(x)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        int32 r = apply((x) -> {\n"
        "            if (x == 42) {\n"
        "                b.bump(x);\n"
        "                return true;\n"
        "            }\n"
        "            return false;\n"
        "        }, 42);\n"
        "        return b.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}

// Narrower: block-body lambda returns boolean, calls instance
// method NO-ARG, no `if` at all.
TEST(LambdaInstanceMethodReproTests, blockBodyBoolNoIfNoArg) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 apply((int32) -> boolean pred, int32 x) {\n"
        "        if (pred(x)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        int32 r = apply((x) -> {\n"
        "            c.bump();\n"
        "            return true;\n"
        "        }, 42);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 1);
}

// Narrower: block-body lambda returns boolean, has `if`, but
// instance call is OUTSIDE the if.
TEST(LambdaInstanceMethodReproTests, blockBodyBoolIfPresentInstanceOutside) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 apply((int32) -> boolean pred, int32 x) {\n"
        "        if (pred(x)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        int32 r = apply((x) -> {\n"
        "            c.bump();\n"
        "            if (x == 42) { return true; }\n"
        "            return false;\n"
        "        }, 42);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 1);
}

// Original failing case: block-body lambda returns boolean
// (predicate shape), calls captured-instance method NO-ARG
// INSIDE the if-then-branch. Mirrors the findFirst predicate
// that mislowered.
TEST(LambdaInstanceMethodReproTests, blockBodyBooleanReturnWithInstanceCall) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 apply((int32) -> boolean pred, int32 x) {\n"
        "        if (pred(x)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        int32 r = apply((x) -> {\n"
        "            if (x == 42) {\n"
        "                c.bump();\n"
        "                return true;\n"
        "            }\n"
        "            return false;\n"
        "        }, 42);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 1);
}

// ---------------------------------------------------------------------
// Symmetric coverage for the OTHER Statement subtypes whose private
// inner block / sub-statements the walker fix taught itself to
// descend into. The if-then (LabelStatement) case above was the
// one originally surfaced; these pin the matching shape inside
// while / for / do / scope / enhanced-for bodies so the gap can't
// silently reopen for any single one of them.
// ---------------------------------------------------------------------

// while-body: captured-instance call from inside a `while` loop body.
// The walker descends into WhileStatement::getBody() via the explicit
// handler. Pre-fix this dropped `this` exactly like the if-branch case.
TEST(LambdaInstanceMethodReproTests, whileBodyInstanceCallOnCapturedClass) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        (int32) -> void fn = (n) -> {\n"
        "            int32 i = 0;\n"
        "            while (i < n) {\n"
        "                c.bump();\n"
        "                i = i + 1;\n"
        "            }\n"
        "        };\n"
        "        fn(5);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 5);
}

// for-body: captured-instance call inside a C-style `for` loop body.
// Walker descends via ForStatement::getBody().
TEST(LambdaInstanceMethodReproTests, forBodyInstanceCallOnCapturedClass) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void add(int32 x) { this.v = this.v + x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        (int32) -> void fn = (n) -> {\n"
        "            for (int32 i = 1; i <= n; i = i + 1) {\n"
        "                c.add(i);\n"
        "            }\n"
        "        };\n"
        "        fn(4);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 1 + 2 + 3 + 4);
}

// do-body: captured-instance call inside a `do { } while (cond)` loop.
// Walker descends via DoStatement::getBody().
TEST(LambdaInstanceMethodReproTests, doBodyInstanceCallOnCapturedClass) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        (int32) -> void fn = (n) -> {\n"
        "            int32 i = 0;\n"
        "            do {\n"
        "                c.bump();\n"
        "                i = i + 1;\n"
        "            } while (i < n);\n"
        "        };\n"
        "        fn(3);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 3);
}

// scope-body: captured-instance call inside a bare `scope { }` block.
// Walker descends via ScopeStatement::getBlock(). Common shape for
// orchestrator-style lambdas that delimit a drop-batching region.
TEST(LambdaInstanceMethodReproTests, scopeBodyInstanceCallOnCapturedClass) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void add(int32 x) { this.v = this.v + x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        () -> void fn = () -> {\n"
        "            scope {\n"
        "                c.add(11);\n"
        "                c.add(31);\n"
        "            }\n"
        "        };\n"
        "        fn();\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}

// enhanced-for body: captured-instance call inside a `for (x : iter)`
// body. Walker descends via EnhancedForStatement::getBody(). Iterates
// a primitive array so we don't drag in iterator-protocol surface.
TEST(LambdaInstanceMethodReproTests, enhancedForBodyInstanceCallOnCapturedClass) {
    auto src = std::string(
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void add(int32 x) { this.v = this.v + x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        int32[] xs = [ 10, 20, 12 ];\n"
        "        () -> void fn = () -> {\n"
        "            for (int32 x : xs) {\n"
        "                c.add(x);\n"
        "            }\n"
        "        };\n"
        "        fn();\n"
        "        return c.v;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}
