//
// Aggregate initializers and ARRAY-typed fields.
//
// A record could declare an array field but never be constructed: the
// initializer coerced toward `prop->getType()->getLlvmType()`, and for a heap
// array `T[]` that reports the array's OWN `{i64, [0 x T]}` struct — while the
// field slot holding it is a plain pointer. Every array-typed binding
// therefore looked like a type mismatch and hit the `isAggregateType()`
// rejection with CAJETA_ERROR_AGGREGATE_INIT_TYPE, so such a record had no
// value form at all.
//
// The fix coerces toward the SLOT's real type, read from the class body
// struct. Found via nucleo's list columns, where `Table<T>.rowAt` rebuilds a
// record whose field is derived from a `T[]`.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// The shape that failed: an owned array result bound straight into the
// initializer.
TEST(AggregateInitArrayFieldTests, arrayFieldBindsFromAnOwnedResult) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public record R { float64[] a; float64 b; }\n"
        "public final class D {\n"
        "    static #float64[] mk() {\n"
        "        float64[] out = heap float64[3];\n"
        "        out[0] = 1.0; out[1] = 2.0; out[2] = 3.0;\n"
        "        return #out;\n"
        "    }\n"
        "    public static float64 run() {\n"
        "        R r = R { a: D.mk(), b: 7.5 };\n"
        "        return r.a[1] + r.b + (float64) r.a.count();\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 2.0 + 7.5 + 3.0);
}

// ...and the same through a local, which failed identically (so this was
// never the owned-temp rule).
TEST(AggregateInitArrayFieldTests, arrayFieldBindsFromALocal) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public record R { int64[] xs; int64 n; }\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64[] tmp = heap int64[2];\n"
        "        tmp[0] = 11; tmp[1] = 31;\n"
        "        R r = R { xs: tmp, n: 5 };\n"
        "        return r.xs[0] + r.xs[1] + r.n;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 47);
}

// A record whose ONLY field is an array — no scalar to mask a bad layout.
TEST(AggregateInitArrayFieldTests, arrayOnlyRecordRoundTrips) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public record Bag { int32[] items; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] v = heap int32[4];\n"
        "        v[0] = 2; v[1] = 4; v[2] = 6; v[3] = 8;\n"
        "        Bag g = Bag { items: v };\n"
        "        int32 sum = 0;\n"
        "        int64 i = 0;\n"
        "        while (i < g.items.count()) {\n"
        "            sum = sum + g.items[i];\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 20);
}

// The rejection must still fire where it ALWAYS fired: a scalar bound to a
// field whose LLVM type is a real aggregate (an inline @ValueType struct).
// The fix narrowed the slot-type substitution to ARRAY fields precisely so
// this path stays untouched. Both branches below were determined by
// MEASUREMENT against the compiler, not by reading the ladder — two earlier
// versions of this test asserted rejections that never existed.
//
// Two PRE-EXISTING holes this deliberately does not assert (both verified
// against a pristine compiler, both out of scope here — they want a
// declared-type compatibility check rather than an LLVM-type comparison):
//   * a wrong POINTER bound to a reference-typed field is accepted (slot and
//     value are both plain pointers, so the aggregate check never sees them);
//   * a wrong POINTER bound to a @ValueType field is accepted, because the
//     "value-type source reached by address" branch loads whatever it is
//     given as the field's struct.
TEST(AggregateInitArrayFieldTests, scalarIntoValueTypeFieldStillRejected) {
    // ANY_THROW, not EXPECT_THROW(std::exception): cajeta's Exception does not
    // derive from std::exception, so the typed form silently missed a
    // rejection that WAS firing (CAJETA_ERROR_AGGREGATE_INIT_TYPE).
    EXPECT_ANY_THROW({
        CajetaJit::compile(
            "package test;\n"
            "@ValueType public final class Vec2 { public float64 x; public float64 y; }\n"
            "public record Holder { Vec2 v; }\n"
            "public final class D {\n"
            "    public static int32 run() {\n"
            "        Holder h = Holder { v: 5.0 };\n"
            "        return 0;\n"
            "    }\n"
            "}\n", "test.D");
    });
}
