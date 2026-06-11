// Tests for @NonNull on parameters (docs/stdlib/Annotations.md
// § Null safety). v1 wires only parameter-level checks; field-level
// is half-supported via @RequiredArgsConstructor's `final`-or-@NonNull
// predicate; return-type checks deferred.
//
// The lowered check throws CAJETA_ERROR_NULL_PARAM_ARG (integer code 2)
// at method entry on a null arg, matching the existing integer-throw
// shape used by Optional.get() (Q11).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Null arg → throws code 2 (caught and returned by the test).
TEST(NonNullTests, nullArgThrowsCode2) {
    auto src =
        "package test;\n"
        "public class S {\n"
        "    public static int32 take(@NonNull String s) { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = null;\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            result = S.take(s);\n"
        "        } catch (Exception e) {\n"
        "            result = (int32) e;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// Non-null arg flows through normally.
TEST(NonNullTests, nonNullArgPassesThrough) {
    auto src =
        "package test;\n"
        "public class S {\n"
        "    public static int32 take(@NonNull String s) { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return S.take(\"hello\");\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Method with both @NonNull and ordinary args — only the @NonNull
// one is checked.
TEST(NonNullTests, onlyNonNullParamsChecked) {
    auto src =
        "package test;\n"
        "public class S {\n"
        "    public static int32 take(String a, @NonNull String b) {\n"
        "        return 1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String a = null;\n"  // not annotated — allowed null
        "        return S.take(a, \"x\");\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Primitive params with @NonNull are silently skipped (primitives can't
// be null). Compiles and runs normally.
TEST(NonNullTests, primitiveParamSkipsCheck) {
    auto src =
        "package test;\n"
        "public class S {\n"
        "    public static int32 take(@NonNull int32 n) { return n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return S.take(7); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Class-typed param check fires too (not just String).
TEST(NonNullTests, classRefParamCheckedToo) {
    auto src =
        "package test;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public class S {\n"
        "    public static int32 take(@NonNull Tag t) { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag t = null;\n"
        "        int32 r = -1;\n"
        "        try { r = S.take(t); } catch (Exception e) { r = (int32) e; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// Instance method with @NonNull — `this` is never checked, only the
// annotated arg.
TEST(NonNullTests, instanceMethodNonNullArg) {
    auto src =
        "package test;\n"
        "public class Handler {\n"
        "    public int32 handle(@NonNull String input) { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Handler h = heap Handler();\n"
        "        String s = null;\n"
        "        int32 r = -1;\n"
        "        try { r = h.handle(s); } catch (Exception e) { r = (int32) e; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// @RequiredArgsConstructor picks up @NonNull fields (alongside `final`).
TEST(NonNullTests, requiredArgsCtorIncludesNonNullField) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor public class P {\n"
        "    @NonNull public String name;\n"
        "    public int32 counter;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(\"hi\");\n"  // ctor takes only `name` (@NonNull)
        "        return p.counter;\n"        // not in ctor, stays 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}
