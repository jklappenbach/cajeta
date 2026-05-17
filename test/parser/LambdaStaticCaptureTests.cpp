//
// P1.2 (ToDo.md priority 1) — lambda capture of static fields.
//
// Original framing in the ToDo was "lambdas reading/writing static
// fields crash at runtime; fix the capture analysis." Investigation
// surfaced the deeper root cause: **static fields aren't
// implemented at all**. A bare non-lambda test like
//   `Counter.total = 5; return Counter.total;`
// also segfaults at JIT load — there's no StaticField class
// alongside StackField/HeapField, and no codegen path that emits
// LLVM globals for class-static properties. STATIC is honored for
// methods only.
//
// Fixing this is multi-session work: introduce a StaticField type,
// emit per-class static-property globals (with optional
// initializers), wire DotExpression / IdentifierExpression
// resolution to look up class-static-properties when the LHS
// resolves to a CajetaClass, and only THEN can the lambda capture
// analysis correctly skip statics as already-global symbols.
//
// Tests stay  as the spec for what should land. Drop the
// prefix once static fields are implemented end-to-end.
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

// A lambda body that writes to a static field. Invoked directly
// (not through Stream.forEach — chained-form `xs.stream().forEach(...)`
// is a separate P6.6 limitation that doesn't actually call forEach).
TEST(LambdaStaticCaptureTests, lambdaWritesStaticField) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public static int32 total = 0;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter.total = 0;\n"
        "        (int32) -> void bump = (int32 v) -> { Counter.total = Counter.total + v; };\n"
        "        bump(1);\n"
        "        bump(2);\n"
        "        bump(3);\n"
        "        return Counter.total;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// Read-only static field reference inside a lambda body. The lambda
// returns a function value; we invoke it directly and observe that
// the static value flows through.
TEST(LambdaStaticCaptureTests, lambdaReadsStaticField) {
    auto src =
        "package test;\n"
        "public class Config {\n"
        "    public static int32 base = 100;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Config.base = 42;\n"
        "        (int32) -> int32 add = (int32 v) -> Config.base + v;\n"
        "        return add(8);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}
