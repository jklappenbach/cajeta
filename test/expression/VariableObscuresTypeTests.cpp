//
// Variable obscures type (script-units plan 6.3.3(a); the Java "obscuring"
// rule, JLS 6.4.2): a bare identifier that names BOTH an in-scope variable
// and a class means the VARIABLE.
//
// Two hazards in DotExpression before the fix:
//  1. Loud: the type-resolver pre-pass runs before body locals register in
//     the scope, so its static-reference fallback pinned the CLASS on the
//     receiver of `t.member`; codegen trusted the pin (the re-resolve was
//     gated on null) and property lookup failed with MEMBER_NOT_FOUND.
//     Discovered via the JIT host: a user class named like a stdlib
//     method's local (`t`) broke stdlib codegen — `cajeta run t.cajeta`
//     could not run.
//  2. Silent: the static-field shortcut resolved the bare name via
//     ofScoped BEFORE considering locals, so `S.total` on a local `S`
//     read the class S's STATIC total instead of the local's field.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Hazard 1 — a local named like a class: member access resolves against
// the LOCAL's type, not the same-named class.
TEST(VariableObscuresTypeTests, localObscuresSameNamedClassOnFieldRead) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class t {\n"
        "    public int32 zzz;\n"
        "}\n"
        "public class Data {\n"
        "    public int32 val;\n"
        "    public Data(int32 v) { this.val = v; }\n"
        "}\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        Data t = heap Data(7);\n"
        "        return t.val;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(7, fn());
}

// Hazard 2 — the silent static hijack: the local `S` (whose type has a
// field `total`) obscures class `S` (which has a STATIC `total`). The read
// must see the local's field, not the class's static.
TEST(VariableObscuresTypeTests, localObscuresClassWithSameNamedStatic) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class S {\n"
        "    public static int32 total;\n"
        "}\n"
        "public class Data {\n"
        "    public int32 total;\n"
        "    public Data(int32 v) { this.total = v; }\n"
        "}\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        S.total = 99;\n"
        "        Data S = heap Data(7);\n"
        "        return S.total;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(7, fn());
}

// The guard must not break genuine static access: no local named Counter
// exists, so `Counter.total` is the class's static — write then read.
TEST(VariableObscuresTypeTests, staticAccessWithoutLocalUnaffected) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class Counter {\n"
        "    public static int32 total;\n"
        "}\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        Counter.total = 41;\n"
        "        Counter.total = Counter.total + 1;\n"
        "        return Counter.total;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(42, fn());
}

// Sequential-codegen boundary: a USE of the class name BEFORE the
// same-named local is declared still means the class (the local only
// obscures from its declaration onward).
TEST(VariableObscuresTypeTests, classNameBeforeLocalDeclarationStillClass) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class S {\n"
        "    public static int32 total;\n"
        "}\n"
        "public class Data {\n"
        "    public int32 total;\n"
        "    public Data(int32 v) { this.total = v; }\n"
        "}\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        S.total = 30;\n"
        "        int32 fromClass = S.total;\n"
        "        Data S = heap Data(12);\n"
        "        return fromClass + S.total;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(42, fn());
}

// 6.3.3 sibling — the Math-namespace short-circuit ran before the
// obscuring check, so a local named `Math` was hijacked by the constant
// table: `Math.PI` returned 3.14159 while `Math` was a Data local.
TEST(VariableObscuresTypeTests, localObscuresMathNamespace) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class Data {\n"
        "    public float64 PI;\n"
        "    public Data(float64 v) { this.PI = v; }\n"
        "}\n"
        "public final class App {\n"
        "    public static float64 run() {\n"
        "        Data Math = heap Data(1.5);\n"
        "        return Math.PI;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<double (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(1.5, fn());
}

// 6.3.3 sibling — the enum-constant short-circuit had the same hole: a
// local named like an enum type lost `X.MEMBER` to the ordinal table.
TEST(VariableObscuresTypeTests, localObscuresEnumTypeName) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public enum Verb { GET, POST }\n"
        "public class Data {\n"
        "    public int32 POST;\n"
        "    public Data(int32 v) { this.POST = v; }\n"
        "}\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        Data Verb = heap Data(7);\n"
        "        return Verb.POST;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(7, fn());
}

// Non-regression: with no obscuring local, both short-circuits keep
// working exactly as before.
TEST(VariableObscuresTypeTests, namespaceAndEnumStillResolveWithoutALocal) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public enum Verb { GET, POST }\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        float64 pi = Math.PI;\n"
        "        int32 ord = Verb.POST;\n"
        "        if (pi > 3.14 && pi < 3.15) { return 10 + ord; }\n"
        "        return ord;\n"
        "    }\n"
        "}\n", "test.App");
    ASSERT_NE(nullptr, jit.get());
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(nullptr, fn);
    EXPECT_EQ(11, fn());
}
