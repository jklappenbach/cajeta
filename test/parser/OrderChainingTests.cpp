//
// A7: @Order-driven advice chaining.
//
// Two pieces:
//   1. matchingAdvice is sorted by @Order(n) in resolveAdviceMatches,
//      so @Before / @After / @AfterReturning / @AfterThrowing all
//      fire in the declared sequence (the emit helpers iterate in
//      vector order).
//   2. emitAroundWrapper builds nested closures + adapters: the
//      outermost @Order @Around wraps the next, which wraps the
//      next, ... ending at llvmOriginalFunction.
//
// Tests rely on non-commutative operations (multiply + add) for the
// @Around chain and a reset-vs-drop side-channel for @Before
// ordering. Without sorted matchingAdvice, both produce the wrong
// answer.
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

} // namespace

// Two @Around advices chain in @Order. With outer = add 100, inner =
// multiply by 2, target returns 5:
//   inner(5) = 5*2 = 10
//   outer(10) = 10 + 100 = 110
// If order were reversed: outer(5) = 5+100 = 105; inner(105) = 210.
// So 110 vs 210 distinguishes the chain direction.
TEST(OrderChainingTests, twoAroundChainInOrder) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class OuterAspect {\n"
        "    @Order(1)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 100;\n"
        "    }\n"
        "}\n"
        "@Aspect public class InnerAspect {\n"
        "    @Order(2)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) * 2;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x; }\n"
        "    public static int32 run() { return work(5); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 110);
}

// Three @Around advices chain. Outermost adds 100, middle adds 10,
// innermost adds 1. Target returns 5. With correct outer-to-inner
// chaining: 5 + 1 (innermost) + 10 (middle) + 100 (outermost) = 116.
// All three addends commute, but if @Order chaining were broken (no
// chain — only the first @Around fires) the result would be 5+100
// = 105. So 116 confirms all three fire AND the chain reaches the
// original.
TEST(OrderChainingTests, threeAroundChainReachesOriginal) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A1 {\n"
        "    @Order(1)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 100;\n"
        "    }\n"
        "}\n"
        "@Aspect public class A2 {\n"
        "    @Order(2)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 10;\n"
        "    }\n"
        "}\n"
        "@Aspect public class A3 {\n"
        "    @Order(3)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x; }\n"
        "    public static int32 run() { return work(5); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 116);
}

// A @Around chain can short-circuit: an intermediate advice that
// doesn't call proceed stops the chain. With OuterAspect calling
// proceed and InnerAspect returning a constant without calling
// proceed, the result is the InnerAspect's constant, transformed
// by Outer. Target's return is never reached.
//   inner(5) ignores proceed, returns 42.
//   outer(42 — via proceed) returns 42 + 100 = 142.
TEST(OrderChainingTests, intermediateAroundShortCircuits) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class OuterAspect {\n"
        "    @Order(1)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 100;\n"
        "    }\n"
        "}\n"
        "@Aspect public class InnerAspect {\n"
        "    @Order(2)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return 42;\n"   // doesn't call proceed
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) {\n"
        "        throw 999;\n"   // would surface if reached
        "        return x;\n"
        "    }\n"
        "    public static int32 run() { return work(5); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 142);
}

// @Before ordering: @Order(1) resets the drop counter; @Order(2)
// allocates and drops an int32[]. Sorted execution: reset first
// (count goes to 0), then the drop fires (count goes to 1). Final
// dropCount after target returns: 1.
//
// If @Order weren't honored and @Order(2) fired first, the drop
// would happen before the reset, leaving count at 0.
TEST(OrderChainingTests, beforeOrderResetThenDrop) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A1 {\n"
        "    @Order(1)\n"
        "    @Before(Audited.class)\n"
        "    public static void resetFirst() {\n"
        "        Cajeta.dropCountReset();\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "@Aspect public class A2 {\n"
        "    @Order(2)\n"
        "    @Before(Audited.class)\n"
        "    public static void dropSecond() {\n"
        "        int32[] tmp = heap int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() { return 0; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// @Order absent: advice without @Order is placed AFTER advice with
// @Order in the sorted list. Here A2 has @Order(2), A_unordered
// has no @Order. Sort puts A2 first, then A_unordered. The chain
// is A2 → A_unordered → original. Target returns 7; A_unordered
// adds 1 (proceed=original); A2 multiplies by 10. Result: (7+1)*10
// = 80. If A_unordered were sorted first instead: 7*10+1 = 71.
TEST(OrderChainingTests, unorderedAdviceFollowsOrdered) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A2 {\n"
        "    @Order(2)\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) * 10;\n"
        "    }\n"
        "}\n"
        "@Aspect public class AUnordered {\n"
        "    @Around(Audited.class)\n"
        "    public static int32 around((int32) -> int32 proceed, int32 x) {\n"
        "        return proceed(x) + 1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work(int32 x) { return x; }\n"
        "    public static int32 run() { return work(7); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 80);
}
