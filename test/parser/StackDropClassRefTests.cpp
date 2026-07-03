//
// Stack-drop of class-ref fields — stack-drop-classref plan unit 2.
//
// A stack-allocated instance whose class-ref field holds a String
// LITERAL must not free the literal's static global at scope exit
// (today: free(): invalid size / double free, SIGABRT). Field drops
// from the stack-drop path must go through the guarded
// __cajeta_class_virtual_drop (or __cajeta_string_drop for String),
// exactly like the heap path — never the unconditional per-class
// drop wrapper. See specs/stack-drop-classref-spec.md §2.
//
// Crash-prone cases run as death tests (child asserts exit 0), the
// pattern from ErrorModelTests. Leak/double-free balance uses
// Cajeta.liveCount() deltas around an inner scope.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile + run `body`; the child process must reach exit(0) — no
// SIGABRT from the drop path.
void expectRunsClean(const std::string& classes, const std::string& body) {
    std::string src =
        std::string("package test;\n") + classes +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EXIT({
        auto jit = CajetaJit::compile(src, "test.D");
        auto fn = jit->lookup<int32_t (*)()>("run");
        fn();
        ::exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// Live-object delta across an inner scope containing `body`.
// 0 = everything allocated inside was freed exactly once.
int64_t liveDelta(const std::string& classes, const std::string& body) {
    std::string src =
        std::string("package test;\n") + classes +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        if (true) {\n"
        "            " + body + "\n"
        "        }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int64_t (*)()>("run")();
}

const char* kHolderLiteral =
    "public class Holder {\n"
    "    public String value;\n"
    "    public Holder() {\n"
    "        this.value = \"hi\";\n"
    "    }\n"
    "}\n";

const char* kHolderNull =
    "public class Holder {\n"
    "    public String value;\n"
    "    public Holder() { return; }\n"
    "}\n";

} // namespace

// 2.1.1 — the minimal repro: stack instance, String field = literal.
TEST(StackDropClassRefTests, literalFieldStackDropRunsClean) {
    expectRunsClean(kHolderLiteral,
        "Holder h = stack Holder();");
}

// 2.1.2 — heap-backed field value (concat result): freed exactly
// once at field drop. Guards the fix against introducing a second
// free on the already-working heap-backed case.
TEST(StackDropClassRefTests, heapBackedFieldFreedExactlyOnce) {
    EXPECT_EQ(liveDelta(kHolderNull,
        "Holder h = stack Holder();\n"
        "String a = \"h\";\n"
        "h.value = a + \"i\";"), 0);
}

// 2.1.3 — field never assigned (null): clean, nothing freed.
TEST(StackDropClassRefTests, nullFieldStackDropRunsClean) {
    expectRunsClean(kHolderNull,
        "Holder h = stack Holder();");
}

// 2.1.4 — reassignments ending on a literal: scope exit must not
// free the literal. (Reassignment-drop semantics for the overwritten
// heap value are out of scope here; this asserts no abort.)
TEST(StackDropClassRefTests, reassignedFieldEndingOnLiteralRunsClean) {
    expectRunsClean(kHolderNull,
        "Holder h = stack Holder();\n"
        "h.value = \"a\";\n"
        "String s = \"x\";\n"
        "h.value = s + \"y\";\n"
        "h.value = \"b\";");
}

// 2.1.5 — templated class with a String field (the tour's
// Crate<String> shape).
TEST(StackDropClassRefTests, templatedStringFieldRunsClean) {
    auto cls =
        "public class Crate<T> {\n"
        "    public T value;\n"
        "    public Crate() { return; }\n"
        "}\n";
    expectRunsClean(cls,
        "Crate<String> c = stack Crate<String>();\n"
        "c.value = \"hi\";");
}

// 2.1.6 — heap instance with the same literal field: behavior
// unchanged by the fix (guarded path already; delta 0, no abort).
TEST(StackDropClassRefTests, heapInstanceBehaviorUnchanged) {
    EXPECT_EQ(liveDelta(kHolderLiteral,
        "Holder h = heap Holder();"), 0);
    expectRunsClean(kHolderLiteral,
        "Holder h = heap Holder();");
}

// ---- Unit 3: base-subobject, array, and interface fields ----------------

// 3.1.1 — a base's String field must drop when the derived stack
// instance leaves scope (own-fields-only walk leaks it today), and a
// literal in the base field must not abort.
TEST(StackDropClassRefTests, baseStringFieldDroppedOnce) {
    auto cls =
        "public class Base {\n"
        "    public String value;\n"
        "    public Base() { return; }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public Derived() { return; }\n"
        "}\n";
    EXPECT_EQ(liveDelta(cls,
        "Derived d = stack Derived();\n"
        "String a = \"h\";\n"
        "d.value = a + \"i\";"), 0);
    expectRunsClean(cls,
        "Derived d = stack Derived();\n"
        "d.value = \"lit\";");
}

// 3.1.2 — MI: both bases' class-ref fields drop.
TEST(StackDropClassRefTests, miBothBasesFieldsDropped) {
    auto cls =
        "public class P1 {\n"
        "    public String a;\n"
        "    public P1() { return; }\n"
        "}\n"
        "public class P2 {\n"
        "    public String b;\n"
        "    public P2() { return; }\n"
        "}\n"
        "public class D2 extends P1, P2 {\n"
        "    public D2() { return; }\n"
        "}\n";
    EXPECT_EQ(liveDelta(cls,
        "D2 d = stack D2();\n"
        "String x = \"h\";\n"
        "d.a = x + \"1\";\n"
        "d.b = x + \"2\";"), 0);
}

// 3.1.3 — diamond: the shared base's field drops exactly once (claim
// guard makes a second attempt a no-op; the balance must be exact).
TEST(StackDropClassRefTests, diamondSharedBaseFieldDroppedOnce) {
    auto cls =
        "public class Root {\n"
        "    public String v;\n"
        "    public Root() { return; }\n"
        "    public void setV(String s) {\n"
        "        this.v = s;\n"
        "    }\n"
        "}\n"
        "public class L extends Root {\n"
        "    public L() { return; }\n"
        "}\n"
        "public class R extends Root {\n"
        "    public R() { return; }\n"
        "}\n"
        "public class DD extends L, R {\n"
        "    public DD() { return; }\n"
        "}\n";
    EXPECT_EQ(liveDelta(cls,
        "DD d = stack DD();\n"
        "String x = \"h\";\n"
        "d.setV(x + \"!\");"), 0);
}

// 3.2.2 — array field on a stack instance: freed like the heap path
// does (__cajeta_free_array), not leaked.
TEST(StackDropClassRefTests, arrayFieldFreedOnStackDrop) {
    auto cls =
        "public class Holder2 {\n"
        "    public int32[] data;\n"
        "    public Holder2() { return; }\n"
        "}\n";
    EXPECT_EQ(liveDelta(cls,
        "Holder2 h = stack Holder2();\n"
        "h.data = heap int32[4];"), 0);
}
