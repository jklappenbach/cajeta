//
// codegen-perf-levers Unit 1 — no-op drop elision for stack value types.
// A stack-allocated local of a class with a TRIVIAL drop (only primitive /
// non-owning fields, no user drop(), no owning ancestor) does nothing at scope
// exit, yet today still registers a drop entry (__cajeta_drop_push +
// __cajeta_drop_pop_run) per scope. In a tight loop that is the entire cost
// (time-instant-arith ~230x; see reference_noop_drop_stack_value_type_tax).
// After the fix such locals register NO drop entry (dropCount stays 0); an owning
// type (owned class-ref / String field) still drops.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Direct `stack Instant(...)` literal locals — the value-return path that the
// frontend classifies as stack-allocated (initIsStackAlloc). With trivial-drop
// elision these register NO drop entry, so dropCount stays 0.
const char* kTrivial =
    "package test;\n"
    "import cajeta.time.Instant;\n"
    "public final class H {\n"
    "    public static int64 drops(int32 n) {\n"
    "        int64 acc = 0;\n"
    "        int32 i = 0;\n"
    "        Cajeta.dropCountReset();\n"
    "        while (i < n) {\n"
    "            Instant t = stack Instant((int64) i, 0);\n"
    "            Instant t2 = stack Instant(t.getEpochSecond() + 7, 0);\n"
    "            acc = acc + t2.getEpochSecond();\n"
    "            i = i + 1;\n"
    "        }\n"
    "        if (acc != (int64) n * ((int64) n - 1) / 2 + 7 * (int64) n) { return -1; }\n"
    "        return Cajeta.dropCount();\n"
    "    }\n"
    "}\n";

// The exact time-instant-arith bench shape (static factory + instance method).
// The instance value-return (plusSeconds) is stack-classified and elided; the
// STATIC factory (ofEpochSecond) is mis-classified to the virtual-drop branch
// (type-name receiver -> targetCls unresolved), so one of the two locals still
// drops. Net: 2 drops/iter -> 1. Full elision needs the static-call
// classification fix (Unit 1.2.c, follow-up). This guards the delivered 2x.
const char* kBench =
    "package test;\n"
    "import cajeta.time.Instant;\n"
    "public final class B {\n"
    "    public static int64 drops(int32 n) {\n"
    "        int64 acc = 0;\n"
    "        int32 i = 0;\n"
    "        Cajeta.dropCountReset();\n"
    "        while (i < n) {\n"
    "            Instant t = Instant.ofEpochSecond((int64) i);\n"
    "            Instant t2 = t.plusSeconds(7);\n"
    "            acc = acc + t2.getEpochSecond();\n"
    "            i = i + 1;\n"
    "        }\n"
    "        if (acc != (int64) n * ((int64) n - 1) / 2 + 7 * (int64) n) { return -1; }\n"
    "        return Cajeta.dropCount();\n"
    "    }\n"
    "}\n";

// Genuinely owning: a String field must be reclaimed, so its stack drop is NOT
// trivial and must keep registering.
const char* kOwning =
    "package test;\n"
    "public final class Box {\n"
    "    String s;\n"
    "    public Box(String s) { this.s = s; }\n"
    "    public int64 len() { return this.s.count(); }\n"
    "}\n"
    "public final class G {\n"
    "    public static int64 drops(int32 n) {\n"
    "        int64 acc = 0;\n"
    "        int32 i = 0;\n"
    "        Cajeta.dropCountReset();\n"
    "        while (i < n) {\n"
    "            Box b = stack Box(\"abc\" + i);\n"
    "            acc = acc + b.len();\n"
    "            i = i + 1;\n"
    "        }\n"
    "        if (acc <= 0) { return -1; }\n"
    "        return Cajeta.dropCount();\n"
    "    }\n"
    "}\n";

int64_t runDrops(const char* src, const char* cls, int32_t n) {
    auto jit = CajetaJit::compile(src, cls);
    auto fn = jit->lookup<int64_t (*)(int32_t)>("drops");
    return fn ? fn(n) : -2;
}

} // namespace

// 1.1.a — trivial-drop stack value locals (stack-classified) incur ZERO drops.
TEST(NoopDropElision, trivialValueTypeNoDrops) {
    int64_t d = runDrops(kTrivial, "test.H", 1000);
    std::printf("[noop-drop] stack Instant x1000 loop: dropCount=%lld\n", (long long) d);
    ASSERT_GE(d, 0) << "result-check sentinel tripped";
    EXPECT_EQ(d, 0) << "trivial-drop stack Instant locals still register/run drop entries";
}

// 1.2.c — the exact time-instant-arith bench shape (static factory + instance
// method) fully elides: BOTH the static `Instant.ofEpochSecond(...)` and the
// instance `t.plusSeconds(...)` value-returns route to the stack branch, so a
// trivial-drop Instant loop incurs ZERO drops.
TEST(NoopDropElision, benchShapeFullyElided) {
    int64_t d = runDrops(kBench, "test.B", 1000);
    std::printf("[noop-drop] bench-shape Instant x1000 loop: dropCount=%lld\n", (long long) d);
    ASSERT_GE(d, 0) << "result-check sentinel tripped";
    EXPECT_EQ(d, 0) << "static-factory value-return not stack-classified (still drops)";
}

// 1.1.b — an owning (String-field) stack type still drops.
TEST(NoopDropElision, owningTypeStillDrops) {
    int64_t d = runDrops(kOwning, "test.G", 1000);
    std::printf("[noop-drop] Box{String} x1000 loop: dropCount=%lld\n", (long long) d);
    ASSERT_GE(d, 0) << "result-check sentinel tripped";
    EXPECT_GT(d, 0) << "owning (String-field) stack local was wrongly elided";
}
