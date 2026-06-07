//
// Cross-class static method calls: `Bar.staticMethod()` from
// inside another class.
//
// Pre-fix, the receiver evaluation in MethodCallExpression
// crashed on a dyn_cast<AllocaInst>(null) when the LHS was a
// bare class identifier — IdentifierExpression returns null
// for class names (no matching local or field). The class
// identifier carries no IR value because there's no `this` to
// pass; the receiver is a namespace marker for static dispatch.
//
// Fix:
//   - MethodCallExpression null-guards the AllocaInst coercion.
//   - When the IR receiver is null and the LHS is an
//     IdentifierExpression naming a class (looked up in
//     canonicalMap, which is keyed by short typeName), set
//     receiverType to that class. The downstream targetClass
//     derivation then routes through static dispatch.
//   - IdentifierExpression::resolveTypes intentionally does NOT
//     pin class-name receivers as their resolvedType — doing
//     so collapses the BOUND vs UNBOUND discriminator in
//     MethodReferenceExpression, which relies on a null LHS
//     resolvedType to flag UNBOUND class-name method refs.
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

// Simplest cross-class static call: `Bar.value()` returns a
// constant from inside class D's run(). Before the fix, the
// receiver-eval of `Bar` segfaulted on the AllocaInst dyn_cast.
TEST(CrossClassStaticCallTests, simpleStaticReturnsConstant) {
    auto src =
        "package test;\n"
        "public class Bar {\n"
        "    public static int32 value() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Bar.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Cross-class static with one int32 arg.
TEST(CrossClassStaticCallTests, staticWithArg) {
    auto src =
        "package test;\n"
        "public class Bar {\n"
        "    public static int32 doubled(int32 x) { return x + x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Bar.doubled(21);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Two cross-class static calls in sequence, results combined.
// Confirms the fix doesn't accidentally cache state across calls.
TEST(CrossClassStaticCallTests, twoStaticsComposed) {
    auto src =
        "package test;\n"
        "public class Tens {\n"
        "    public static int32 v() { return 10; }\n"
        "}\n"
        "public class Ones {\n"
        "    public static int32 v() { return 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Tens.v() + Ones.v();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

// The literal A9 case: __cajeta_inject() called via cross-class
// static. The synthesized helper on Bar is invoked from D, which
// previously crashed. Now the receiver-fallback in
// MethodCallExpression routes through Bar's methods map and
// finds the synthesized __cajeta_inject.
TEST(CrossClassStaticCallTests, injectFromAnotherClass) {
    auto src =
        "package test;\n"
        "@Component public class Bar {\n"
        "    public int32 value;\n"
        "    public Bar() { value = 99; return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bar b = Bar.__cajeta_inject();\n"
        "        return b.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// A control test: the BOUND-vs-UNBOUND method-reference
// discriminator that the IdentifierExpression behavior is
// supposed to preserve. `Counter::value` is a bare-class-name
// method reference (UNBOUND). If the fix accidentally pinned a
// class type as the LHS resolvedType, this case would mis-bind
// as BOUND and the unbound path would never see it. Calling the
// reference with an instance argument and reading the result is
// the simplest survival check.
TEST(CrossClassStaticCallTests, methodRefStillUnboundAfterFix) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter() { v = 0; return; }\n"
        "    public int32 value() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        (Counter) -> int32 fn = Counter::value;\n"
        "        return fn(c);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
