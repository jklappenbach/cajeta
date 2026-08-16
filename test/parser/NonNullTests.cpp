// Tests for @NonNull on parameters (docs/specification/reflect/Annotations.md
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

// Non-null arg flows through normally.

// Method with both @NonNull and ordinary args — only the @NonNull
// one is checked.

// Primitive params with @NonNull are silently skipped (primitives can't
// be null). Compiles and runs normally.

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

// @RequiredArgsConstructor picks up @NonNull fields (alongside `final`).
