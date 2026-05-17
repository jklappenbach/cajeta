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
