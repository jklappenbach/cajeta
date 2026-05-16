//
// Control-flow statement tests: if/else, while, for, do-while, break, continue,
// nested loops. Each test JIT-compiles a small Cajeta program and asserts on the
// returned value to verify codegen correctness end-to-end.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class C {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- if / else ---------------------------------------------------------------

TEST(ControlFlowTests, ifTakesThenBranch) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 10;\n"
        "if (a > 0) {\n"
        "    return 1;\n"
        "}\n"
        "return 0;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(ControlFlowTests, ifSkipsThenBranch) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = -1;\n"
        "if (a > 0) {\n"
        "    return 1;\n"
        "}\n"
        "return 0;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

TEST(ControlFlowTests, ifElseSelectsElse) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 0;\n"
        "if (a > 0) {\n"
        "    return 100;\n"
        "} else {\n"
        "    return 200;\n"
        "}"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 200);
}

TEST(ControlFlowTests, nestedIfElse) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 5;\n"
        "if (a > 10) {\n"
        "    return 1;\n"
        "} else {\n"
        "    if (a > 0) {\n"
        "        return 2;\n"
        "    } else {\n"
        "        return 3;\n"
        "    }\n"
        "}"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// --- while -------------------------------------------------------------------

TEST(ControlFlowTests, whileBasicCounting) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "int32 sum = 0;\n"
        "while (i < 10) {\n"
        "    sum = sum + i;\n"
        "    i = i + 1;\n"
        "}\n"
        "return sum;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 0+1+2+...+9 = 45
    EXPECT_EQ(fn(), 45);
}

TEST(ControlFlowTests, whileNeverEntersOnFalseCondition) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "while (i > 10) {\n"
        "    i = i + 1;\n"
        "}\n"
        "return i;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// Restore the array-count-as-loop-bound test that was deferred.
TEST(ControlFlowTests, whileUsingArraySize) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[5];\n"
        "int32 total = 0;\n"
        "int32 i = 0;\n"
        "while (i < arr.count()) {\n"
        "    arr[i] = i + 1;\n"
        "    total = total + arr[i];\n"
        "    i = i + 1;\n"
        "}\n"
        "return total;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 1+2+3+4+5 = 15
    EXPECT_EQ(fn(), 15);
}

// --- for ---------------------------------------------------------------------

TEST(ControlFlowTests, forWithExpressionInit) {
    // for-init currently supports expression-form (`i = 0`) only; var-decl-in-init
    // is deferred. Pre-declare the loop variable.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "int32 sum = 0;\n"
        "for (i = 0; i < 5; i = i + 1) {\n"
        "    sum = sum + i;\n"
        "}\n"
        "return sum;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 0+1+2+3+4 = 10
    EXPECT_EQ(fn(), 10);
}

TEST(ControlFlowTests, forNoCondition) {
    // `for (;;)` is an infinite loop; `break` exits.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "for (;;) {\n"
        "    if (i == 7) { break; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return i;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// --- do-while ----------------------------------------------------------------

TEST(ControlFlowTests, doWhileRunsAtLeastOnce) {
    // Body runs once even when the condition is initially false.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 100;\n"
        "do {\n"
        "    i = i + 1;\n"
        "} while (i < 0);\n"
        "return i;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 101);
}

TEST(ControlFlowTests, doWhileCounts) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "int32 product = 1;\n"
        "do {\n"
        "    i = i + 1;\n"
        "    product = product * i;\n"
        "} while (i < 5);\n"
        "return product;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 5! = 120
    EXPECT_EQ(fn(), 120);
}

// --- break + continue --------------------------------------------------------

TEST(ControlFlowTests, breakExitsInnermostLoop) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "while (i < 100) {\n"
        "    if (i == 5) { break; }\n"
        "    i = i + 1;\n"
        "}\n"
        "return i;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

TEST(ControlFlowTests, continueSkipsToNextIteration) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "int32 sum = 0;\n"
        "while (i < 10) {\n"
        "    i = i + 1;\n"
        "    if (i == 5) { continue; }\n"
        "    sum = sum + i;\n"
        "}\n"
        "return sum;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 1+2+3+4+6+7+8+9+10 = 50
    EXPECT_EQ(fn(), 50);
}

TEST(ControlFlowTests, nestedLoopsBreakOuterOnly) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "int32 j = 0;\n"
        "int32 hits = 0;\n"
        "while (i < 5) {\n"
        "    j = 0;\n"
        "    while (j < 5) {\n"
        "        if (j == 2) { break; }\n"
        "        hits = hits + 1;\n"
        "        j = j + 1;\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return hits;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // Inner runs 5 times (i=0..4); each inner runs hits 2 (j=0,1) before break.
    EXPECT_EQ(fn(), 10);
}

TEST(ControlFlowTests, earlyReturnFromLoop) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 i = 0;\n"
        "while (i < 100) {\n"
        "    if (i == 42) {\n"
        "        return i;\n"
        "    }\n"
        "    i = i + 1;\n"
        "}\n"
        "return -1;"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// --- if without else returning a value (else falls through to next stmt) -----

TEST(ControlFlowTests, ifElseAsExpressionViaReturns) {
    // Both branches return; the merge block is unreachable. Verifies that
    // emitting unconditional `ret`s in both branches doesn't trip the
    // missing-terminator handling in Method::generateCode.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32 a = 7;\n"
        "if (a > 0) {\n"
        "    return 1;\n"
        "} else {\n"
        "    return 0;\n"
        "}"), "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
