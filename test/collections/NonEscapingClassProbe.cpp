// Non-escaping class instances (Phase B piece 1, plan nonescaping-class-instances).
// GOAL: a local `heap ClassName(...)` that never escapes the frame is promoted to
// STACK allocation (entry alloca, no malloc, no __cajeta_live_set_add) so LLVM sees
// it as non-escaping. liveCount() observes the path (promoted: +0; heap: >=1).
//
// DISABLED (2026-06-25): the promotion is NOT active. A test-first attempt
// implemented an intraprocedural escape gate (never returned/stored/#moved +
// argPassed bar + lambda-capture bar + a per-CLASS "methods never let `this`
// escape" check over the superclass chain). It still DOUBLE-FREES in
// HashMapStreamParallelTests.parallelEntriesCountMatchesSize — a class local in the
// parallel-stream machinery escapes via a vector none of those gates caught (the
// clean build passes that test, confirming the promotion is the cause). Sound class-
// instance stack-promotion needs more complete INTERPROCEDURAL escape/retention
// analysis (callee summaries: does a called method retain its receiver/args through
// nested calls + spawn?). Kept here, disabled, as the behavioral spec + the soundness
// contract for that follow-up. See agents/nonescaping-class-instances-plan.md.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
// Two-class source: a tiny primitive-field Counter (trivial drop) + the A harness.
std::unique_ptr<CajetaJit> jitOf(const char* aBody) {
    std::string src =
        std::string("package test;\n") +
        "public final class Counter {\n"
        "    public int32 v;\n"
        "    public Counter(int32 init) { this.v = init; }\n"
        "    public void inc() { this.v = this.v + 1; }\n"
        "    public int32 get() { return this.v; }\n"
        "}\n"
        "public final class A {\n"
        "    static Counter held;\n"
        "    static int32 sink(Counter c) { return c.get(); }\n" +
        aBody +
        "}\n";
    return CajetaJit::compile(src, "test.A");
}
}

// 1.1.a — eligible: a non-escaping local (only receiver-position calls + local
// reads) is stack-promoted, so it never touches the live-set, and still computes
// correctly (ctor + methods + drop work under stack allocation).
TEST(NonEscapingClassProbe, DISABLED_eligibleLocalStackPromoted) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 lbase = Cajeta.liveCount();\n"
        "        Counter c = heap Counter(n);\n"   // non-escaping
        "        c.inc();\n"
        "        c.inc();\n"
        "        int64 lused = Cajeta.liveCount() - lbase;\n"
        "        int32 val = c.get();\n"           // local use keeps c live
        "        if (val != n + 2) { return -100; }\n"
        "        return lused;\n"                  // 0 when stack-promoted
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GE(r, 0) << "result-check sentinel tripped (wrong value under promotion)";
    EXPECT_EQ(r, 0) << "non-escaping local was not stack-promoted (touched live-set)";
}

// 1.1.b (escape via store) — a local stored into a static field escapes and must
// stay on the heap (live-set +1), and the stored value remains valid.
TEST(NonEscapingClassProbe, DISABLED_storedLocalStaysHeap) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 lbase = Cajeta.liveCount();\n"
        "        Counter c = heap Counter(n);\n"
        "        A.held = c;\n"                    // escapes -> heap
        "        int64 lused = Cajeta.liveCount() - lbase;\n"
        "        if (A.held.get() != n) { return -100; }\n"
        "        return lused;\n"                  // >=1
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GE(r, 1) << "escaping (stored) local was wrongly stack-promoted";
}

// 1.1.b (escape via non-receiver arg) — passing the local as a call ARGUMENT may
// retain it as a borrow (e.g. list.add stores a borrow), so it is conservatively
// NOT promoted (stays heap), and the call still computes correctly.
TEST(NonEscapingClassProbe, DISABLED_argPassedLocalStaysHeap) {
    auto jit = jitOf(
        "    public static int64 probe(int32 n) {\n"
        "        int64 lbase = Cajeta.liveCount();\n"
        "        Counter c = heap Counter(n);\n"
        "        int32 x = sink(c);\n"             // non-receiver arg -> conservative heap
        "        int64 lused = Cajeta.liveCount() - lbase;\n"
        "        if (x != n) { return -100; }\n"
        "        return lused;\n"                  // >=1 (conservative)
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("probe");
    int64_t r = fn(7);
    EXPECT_GE(r, 1) << "arg-passed local was wrongly stack-promoted (borrow may be retained)";
}
