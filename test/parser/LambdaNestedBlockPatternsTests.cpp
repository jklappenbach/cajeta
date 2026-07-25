// Higher-level patterns the lambda-capture walker fix made writable
// in their natural form. Before the fix, each of the shapes below
// would have either failed at codegen (null-receiver method call on
// captured class) or silently produced wrong results (capture
// silently dropped, ownership transfer demoted). Each test here
// exercises a specific Statement subtype the walker fix added a
// handler for:
//
//   - foldCallbackWithLoopOverBatch:        ForStatement::getBody
//   - collectorAccumulatorConditionalBranch: LabelStatement (else)
//   - scopeSpawnInsideLambda:                ScopeStatement::getBlock
//
// Each captures a class reference from the enclosing method's scope
// and touches it from inside the nested block — the exact path the
// pre-fix walker silently skipped. Observable assertions verify the
// captured state was actually mutated by the lambda body, not just
// that compilation succeeded.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

} // namespace

// Hand-rolled fold whose accumulator lambda runs an INNER LOOP over
// each element's worth of work and bumps a captured Counter from
// inside the loop body. The lambda captures `c` and references it
// inside a `for` body — the walker must descend into
// ForStatement::getBody() to register the capture. Pre-fix this
// either crashed (null receiver) or silently dropped the bump.
TEST(LambdaNestedBlockPatternsTests, foldCallbackWithLoopOverBatch) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] sizes = [ 3, 5, 2 ];\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(sizes, 3);\n"
        "        Counter c = heap Counter();\n"
        "        // Lambda hoisted to a typed local — inline block-body\n"
        "        // lambdas passed to generic fold<R> don't yet infer R\n"
        "        // from the seed argument. The typed local pins R = int32\n"
        "        // so the body's `return t;` matches; the capture-walker\n"
        "        // mechanics being tested are the same either way.\n"
        "        (int32, int32) -> int32 accFn = (int32 acc, int32 sz) -> {\n"
        "            int32 t = acc;\n"
        "            for (int32 i = 0; i < sz; i = i + 1) {\n"
        "                c.bump();\n"
        "                t = t + 1;\n"
        "            }\n"
        "            return t;\n"
        "        };\n"
        "        int32 total = s.fold(0, accFn);\n"
        "        // total == sum of sizes; c.v also == sum of sizes (one\n"
        "        // bump per inner iteration). Returning total + c.v\n"
        "        // (== 20) means the capture WAS visible inside the\n"
        "        // for-body. If the walker had skipped the capture the\n"
        "        // lambda would either crash or silently leave c.v == 0\n"
        "        // (result would be 10 not 20).\n"
        "        return total + c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// Accumulator with a conditional branch — the natural Collector-style
// shape: \"if this element passes the predicate, accumulate it AND
// note that we accepted it; otherwise note that we rejected it\".
// Exercises both branches of LabelStatement (if/else are both
// LabelStatement-wrapped blocks). The lambda captures two outer
// Counters and mutates both from inside their respective branches.
TEST(LambdaNestedBlockPatternsTests, collectorAccumulatorConditionalBranchCapturesBoth) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 6);\n"
        "        Counter accepted = heap Counter();\n"
        "        Counter rejected = heap Counter();\n"
        "        // Hoisted to a typed local for the same reason as the\n"
        "        // foldCallbackWithLoopOverBatch test above.\n"
        "        (int32, int32) -> int32 accFn = (int32 acc, int32 x) -> {\n"
        "            if (x % 2 == 0) {\n"
        "                accepted.bump();\n"
        "                return acc + x;\n"
        "            } else {\n"
        "                rejected.bump();\n"
        "                return acc;\n"
        "            }\n"
        "        };\n"
        "        int32 sum = s.fold(0, accFn);\n"
        "        // Evens 2+4+6 = 12; accepted=3, rejected=3.\n"
        "        // Both captures observed iff walker descended into\n"
        "        // both LabelStatement-wrapped branches.\n"
        "        if (sum != 12) { return -1; }\n"
        "        if (accepted.v != 3) { return -2; }\n"
        "        if (rejected.v != 3) { return -3; }\n"
        "        return sum + accepted.v + rejected.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// Orchestrator-style: lambda body contains `scope { spawn work(c, n); }`.
// The captured `c` is read inside the ScopeStatement's block (passed
// as a spawn argument) — the walker must descend into
// ScopeStatement::getBlock() to register the capture. Pre-fix the
// lambda would have called spawn with a null receiver-class argument
// (or crashed during identifier lookup at codegen).
//
// Uses sync-lowered spawn (per SpawnDropTests) so the worker runs
// inline and we can observe its side effects synchronously.
TEST(LambdaNestedBlockPatternsTests, scopeSpawnInsideLambdaSeesCapture) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { this.v = 0; }\n"
        "    public void bump() { this.v = this.v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static async int32 work(Counter shared, int32 times) {\n"
        "        for (int32 i = 0; i < times; i = i + 1) {\n"
        "            shared.bump();\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        (int32) -> void fn = (n) -> {\n"
        "            scope {\n"
        "                int32 unused = await spawn work(c, n);\n"
        "            }\n"
        "        };\n"
        "        fn(7);\n"
        "        return c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
