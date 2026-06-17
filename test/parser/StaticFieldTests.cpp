//
// Static class field support. Cajeta lets a class declare
//
//   public class Counter {
//       public static int32 total;
//       public static int32 base = 100;
//   }
//
// and access the field as `Counter.total` for read/write. Under the
// hood: each static field becomes an LLVM global variable named
// `<class.canonical>.<fieldName>` in the declaring class's home
// llvm::Module, with an optional initializer constant. DotExpression
// with a class-name LHS resolves the RHS as a class-static property
// and emits a global load (read) or store (write).
//
// Pre-fix state: `public static int32 total = 0` parses but emits no
// global; `Counter.total = 5; return Counter.total;` segfaults at JIT
// load. Tests below pin the expected behavior.
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

// Bare write + read. No initializer; the global starts at zero
// (LLVM globals get zeroinitializer by default). Writing then reading
// yields the written value.
TEST(StaticFieldTests, writeThenRead) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public static int32 total;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter.total = 5;\n"
        "        return Counter.total;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Two distinct static fields on the same class don't alias.
TEST(StaticFieldTests, twoStaticsAreDistinct) {
    auto src =
        "package test;\n"
        "public class Slots {\n"
        "    public static int32 a;\n"
        "    public static int32 b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Slots.a = 10;\n"
        "        Slots.b = 20;\n"
        "        return Slots.a + Slots.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// Statics persist across method calls (they're module-level globals,
// not per-call state). Writing in one method and reading in another
// yields the same value.
TEST(StaticFieldTests, persistsAcrossMethodCalls) {
    auto src =
        "package test;\n"
        "public class State {\n"
        "    public static int32 value;\n"
        "}\n"
        "public final class D {\n"
        "    public static void bump() {\n"
        "        State.value = State.value + 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        State.value = 0;\n"
        "        bump();\n"
        "        bump();\n"
        "        bump();\n"
        "        return State.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// Read-modify-write through compound expressions. Verifies the field
// is a true lvalue (assignable from an arbitrary expression) and an
// rvalue (loadable in arithmetic).
TEST(StaticFieldTests, readModifyWrite) {
    auto src =
        "package test;\n"
        "public class Acc {\n"
        "    public static int32 sum;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Acc.sum = 0;\n"
        "        Acc.sum = Acc.sum + 10;\n"
        "        Acc.sum = Acc.sum + 20;\n"
        "        Acc.sum = Acc.sum * 2;\n"
        "        return Acc.sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

// int64 static. Verifies the width is honored across read/write —
// truncating to i32 silently would lose the upper bits of a large
// constant.
TEST(StaticFieldTests, int64WidthPreserved) {
    auto src =
        "package test;\n"
        "public class Big {\n"
        "    public static int64 value;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Big.value = 5000000000;\n"  // > 2^32, exceeds int32
        "        return (int32) (Big.value / 1000000);\n"  // 5000
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5000);
}

// Constant-literal initializer: `public static int32 base = 100;`
// Reading the field without any prior assignment yields the literal.
TEST(StaticFieldTests, intLiteralInitializer) {
    auto src =
        "package test;\n"
        "public class Config {\n"
        "    public static int32 base = 100;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Config.base;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

// int64 literal initializer with a value beyond int32 range.
TEST(StaticFieldTests, int64LiteralInitializer) {
    auto src =
        "package test;\n"
        "public class Limits {\n"
        "    public static int64 max = 5000000000;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return (int32) (Limits.max / 1000000);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5000);
}

// Negative literal initializer.
TEST(StaticFieldTests, negativeLiteralInitializer) {
    auto src =
        "package test;\n"
        "public class Sign {\n"
        "    public static int32 offset = -7;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Sign.offset;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -7);
}

// P6.2 — computed (non-literal) initializer: arithmetic on integer
// literals. Today only `<literal>` and `-<literal>` constant-fold
// into the global's initializer; anything else silently zeroes the
// global. The fix is a per-module clinit-style init function
// registered via `llvm.global_ctors` that runs the user expression
// and stores into the global at module load.
TEST(StaticFieldTests, computedIntInitializer) {
    auto src =
        "package test;\n"
        "public class Calc {\n"
        "    public static int32 sum = 1 + 2;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Calc.sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// P6.2 — initializer referencing another static. Order matters: the
// referenced static must already be initialized when this one runs.
// The clinit function emits stores in declaration order; cross-class
// is fine because each class's clinit is independently registered.
TEST(StaticFieldTests, initReferencesAnotherStatic) {
    auto src =
        "package test;\n"
        "public class Two {\n"
        "    public static int32 a = 10;\n"
        "    public static int32 b = a + 5;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Two.b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// P6.2 — multi-term arithmetic.
TEST(StaticFieldTests, multiTermArithmeticInitializer) {
    auto src =
        "package test;\n"
        "public class Math {\n"
        "    public static int32 v = (2 * 3) + (10 / 2);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Math.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Cross-class read/write — class B touches A's statics. Confirms the
// global is module-scoped, not class-instance-scoped, and accessible
// by canonical name from any compilation unit that names the class.
TEST(StaticFieldTests, crossClassAccess) {
    auto src =
        "package test;\n"
        "public class Shared {\n"
        "    public static int32 ticket;\n"
        "}\n"
        "public class Issuer {\n"
        "    public static int32 next() {\n"
        "        Shared.ticket = Shared.ticket + 1;\n"
        "        return Shared.ticket;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Shared.ticket = 100;\n"
        "        int32 a = Issuer.next();\n"  // 101
        "        int32 b = Issuer.next();\n"  // 102
        "        return a + b;\n"             // 203
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 203);
}

// Regression: an UNQUALIFIED same-class static-field read (`N`, the shorthand
// for `D.N`) must load the field's VALUE. IdentifierExpression returns the
// field's GlobalVariable slot — the same l-value shape as a local's alloca —
// so loadIfLValue / the call-arg coercion must load through it. Pre-fix the
// read yielded the global's ADDRESS (a ptr), giving garbage / a segfault on use.

// (a) bare read into a local.
TEST(StaticFieldTests, bareUnqualifiedStaticReadIntoLocal) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static final int32 N = 42;\n"
        "    public static int32 run() {\n"
        "        int32 x = N;\n"
        "        return x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// (b) bare read passed directly as a call argument (the `hasAnnotation(TAG)`
// shape that surfaced the bug). Exercises the per-call entry-coercion path.
TEST(StaticFieldTests, bareUnqualifiedStaticReadAsCallArg) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static final int32 N = 42;\n"
        "    static int32 echo(int32 v) { return v; }\n"
        "    public static int32 run() {\n"
        "        return echo(N);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// (c) bare read in a binary-op (both operands coerced through loadIfLValue).
TEST(StaticFieldTests, bareUnqualifiedStaticReadInBinaryOp) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static final int32 A = 40;\n"
        "    static final int32 B = 2;\n"
        "    public static int32 run() {\n"
        "        return A + B;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Regression: an instance method calling a same-class static method with a
// static String-field arg (plus a second arg) under source-tags used to leave
// the module's builder pointer dangling after method codegen; the later clinit
// pass (generateStaticInitializers) then dereferenced it and SIGSEGV'd the
// COMPILER. The clinit now emits through its own local builder. Mirrors the
// minimal trigger; must compile + run cleanly.
TEST(StaticFieldTests, clinitSurvivesStaticArgFromInstanceMethod) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static final String TAG = \"hi\";\n"
        "    static boolean eq(String a, String b) { return a.equals(b); }\n"
        "    public int32 f() { return eq(TAG, \"hi\") ? 1 : 0; }\n"
        "    public static int32 run() {\n"
        "        D d = heap D();\n"
        "        return d.f();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Regression: a BARE (unqualified) static String field used as a METHOD
// RECEIVER — `TAG.equals(...)` inside an instance method. IdentifierExpression
// returns the field's GlobalVariable slot (its address); the l-value coercion
// must load through it to materialize the String pointer used as `this`.
// Pre-fix the global's ADDRESS was passed as the receiver (and the vtable load
// read the String pointer instead of its vtable) → garbage dispatch / SIGSEGV.
// The IdentifierExpression analogue of the DotExpression `Class.STATIC.method()`
// fix (a49714dc).
TEST(StaticFieldTests, bareUnqualifiedStaticFieldAsReceiver) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static final String TAG = \"hi\";\n"
        "    public int32 f() {\n"
        "        if (TAG.equals(\"hi\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        D d = heap D();\n"
        "        return d.f();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// And the negative branch: the receiver's VALUE must drive the comparison
// (not a garbage pointer), so a non-matching arg returns 0.
TEST(StaticFieldTests, bareUnqualifiedStaticFieldReceiverNegative) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static final String TAG = \"hi\";\n"
        "    public int32 f() {\n"
        "        if (TAG.equals(\"bye\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        D d = heap D();\n"
        "        return d.f();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
