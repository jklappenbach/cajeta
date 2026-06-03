// Return-type inference for block-body lambdas passed inline as
// arguments to generic methods.
//
// L1.5 covered parameter-type inference from the target
// CajetaFunctionType (typed-local LHS). This file extends the same
// idea to the return-type side, specifically when the target function
// type comes from a method-call argument position rather than from a
// local's LHS type ascription.
//
// Failure mode pinned here (P1 #1 in todo.md): when a block-body
// lambda is passed inline to `s.fold<R>(seed, lambda)`, the lambda's
// return type was being fixed to `void` because the call site didn't
// propagate the parameter's target-type return into the lambda
// before body resolution. The body's `return x;` then trips JIT
// verify with "Found return instr that returns non-void in Function
// of void return type".
//
// Workaround (still works, kept exercised elsewhere): hoist the
// lambda to a typed local
//   (int32, int32) -> int32 fn = (acc, x) -> { ... return ...; };
//   s.fold(0, fn);
// and pass the local. The tests below pin the INLINE form so the
// fix is observable end-to-end.

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

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.stream.ArrayStream;\n";

} // namespace

// Minimal failing shape: inline block-body lambda with a single
// return statement, passed to fold. Return type must come from R
// (bound to int32 by the seed) — there is no LHS type ascription
// available to fall back on.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodySingleReturn) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 4);\n"
        "        return s.fold(0, (int32 acc, int32 x) -> {\n"
        "            return acc + x;\n"
        "        });\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Block-body with a local + multi-statement before the return.
// Same inference path, slightly more body shape to make sure the
// return-type propagation isn't single-statement-specific.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodyMultiStatement) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 4);\n"
        "        return s.fold(0, (int32 acc, int32 x) -> {\n"
        "            int32 doubled = x + x;\n"
        "            return acc + doubled;\n"
        "        });\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// Block-body with control flow — both branches return. The lambda
// has two return sites; both must agree with R.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodyBranchingReturns) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4, 5, 6 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 6);\n"
        "        return s.fold(0, (int32 acc, int32 x) -> {\n"
        "            if (x % 2 == 0) {\n"
        "                return acc + x;\n"
        "            }\n"
        "            return acc;\n"
        "        });\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// Cross-type fold: int32 stream, int64 accumulator. R = int64 must
// flow into the lambda from the seed's int64 type so the body's
// `return acc + 1L;` (int64) matches.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodyCrossType) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = { 1, 2, 3 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        return s.fold(100L, (int64 acc, int32 x) -> {\n"
        "            return acc + 1L;\n"
        "        });\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 103LL);
}

// The original failing shape (commit aebb740 worked around this by
// hoisting to a typed local): block-body lambda with a CAPTURE
// referenced inside a nested for-loop body. The capture path appears
// to be what tips the inference into the wrong direction — bodies
// without captures (the earlier tests in this file) infer fine.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodyWithCaptureInsideForBody) {
    auto src = std::string(PRELUDE) +
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] sizes = { 3, 5, 2 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(sizes, 3);\n"
        "        Counter c = new Counter();\n"
        "        int32 total = s.fold(0, (int32 acc, int32 sz) -> {\n"
        "            int32 t = acc;\n"
        "            for (int32 i = 0; i < sz; i = i + 1) {\n"
        "                c.bump();\n"
        "                t = t + 1;\n"
        "            }\n"
        "            return t;\n"
        "        });\n"
        "        return total + c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// Same shape but capture inside an if-branch (the LambdaNestedBlock
// test pinned via a typed-local workaround). Two captures, both
// branches mutate one of them.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodyWithCapturesInsideIfElse) {
    auto src = std::string(PRELUDE) +
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4, 5, 6 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 6);\n"
        "        Counter accepted = new Counter();\n"
        "        Counter rejected = new Counter();\n"
        "        int32 sum = s.fold(0, (int32 acc, int32 x) -> {\n"
        "            if (x % 2 == 0) {\n"
        "                accepted.bump();\n"
        "                return acc + x;\n"
        "            } else {\n"
        "                rejected.bump();\n"
        "                return acc;\n"
        "            }\n"
        "        });\n"
        "        return sum + accepted.v + rejected.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// Capture at the top level of the lambda body (no nesting). Pin
// whether the bug is capture-specific or capture-inside-nested-block
// specific.
TEST(LambdaReturnInferenceTests, foldInlineBlockBodyWithTopLevelCapture) {
    auto src = std::string(PRELUDE) +
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        Counter c = new Counter();\n"
        "        int32 total = s.fold(0, (int32 acc, int32 x) -> {\n"
        "            c.bump();\n"
        "            return acc + x;\n"
        "        });\n"
        "        return total + c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// Probe: returns a local declared in the body (no captures, no
// nested blocks). Isolates whether the bug is local-binding-
// during-lambda-body-resolve or specifically nested-block /
// capture-driven.
TEST(LambdaReturnInferenceTests, probeReturnBareLocalNoCapturesNoNesting) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        return s.fold(0, (int32 acc, int32 x) -> {\n"
        "            int32 t = acc + x;\n"
        "            return t;\n"
        "        });\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// User-defined generic helper — verifies the fix isn't fold-specific
// but lives in the call-site / lambda-binding resolution path.
TEST(LambdaReturnInferenceTests, userGenericHelperInlineBlockBodyLambda) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static R apply<R>(R seed, (R) -> #R fn) {\n"
        "        return fn(seed);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return apply(7, (int32 x) -> {\n"
        "            return x + x;\n"
        "        });\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 14);
}

// Bare-identifier lambdas passed to an OVERLOADED method-template
// (Stream<T>.fold has both `fold<R>(R, (R,T)->R)` and
// `fold<R>(R, (R,T)->R, (R,R)->R)`). With explicit type-args
// `.fold<int64>(...)`, the call site uniquely identifies the
// 3-arg overload by value-arity (3 args vs the 2-arg form's 2). The
// lambda-expectedType propagator in MethodCallExpression must
// disambiguate by arity in the method-template candidate path; without
// that, both overloads match the type-arg count check, the propagator
// records matches=2 and skips, and the bare-identifier lambdas land at
// CAJETA_ERROR_TYPE_INFERENCE because no expectedType was pinned.
TEST(LambdaReturnInferenceTests, foldWithCombinerBareIdentifierLambdas) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 4);\n"
        "        return s.fold<int64>(0L,\n"
        "            (acc, x) -> acc + (int64) x,\n"
        "            (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(jit->lookup<int64_t (*)()>("run")(), 10LL);
}

// `ArrayList<UserClass>.stream().fold<R>(seed, (acc, p) -> ...)` —
// the bare-id lambda has paramTypes empty at MCE generateCode time.
// Pre-fix, the lambda's earlier resolveTypes produced a placeholder
// `() -> void` resolvedType (no scope for the bare-id params), and
// the MCE propagator's `!getResolvedType()` gate skipped inference
// entirely. Combined with a stale `fold<UserClass>` instantiation
// already cached on Stream<UserClass> (from `reduce` delegation),
// matches went to 2 → propagator's `matches==1` block skipped →
// lambda compiled with empty paramTypes → CAJETA_ERROR_TYPE_INFERENCE
// at generateCode.
//
// Fix: paramTypes/paramNames arity is the inference-needed signal,
// not resolvedType presence; and method-template INSTANTIATIONS
// from prior call sites are filtered out of findCandidate (they
// can't be re-bound to fresh explicit type args).
TEST(LambdaReturnInferenceTests, foldBareIdLambdaOverArrayListUserClass) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class P {\n"
        "    public int32 x;\n"
        "    public P(int32 x) { this.x = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<P> pts = heap ArrayList<P>();\n"
        "        pts.add(heap P(1));\n"
        "        pts.add(heap P(2));\n"
        "        pts.add(heap P(3));\n"
        "        return pts.stream().fold<int32>(0,\n"
        "            (acc, p) -> acc + p.x);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 6);
}
