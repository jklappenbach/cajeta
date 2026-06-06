// Tests for cajeta's local-variable-declaration borrow detection.
// The borrow-detection branch in LocalVariableDeclaration::generateCode
// suppresses the local's drop entry when the RHS is a borrow shape:
//   - DotExpression on a class-typed field
//   - ArrayIndexExpression returning a class-typed element
//   - IdentifierExpression naming another local or method parameter
// All three of those mean the local is just a second pointer to a
// heap object owned elsewhere; registering its own drop entry would
// double-free at scope exit.
//
// These tests build on the pass-by-pointer convention for arrays —
// `T[] xs` parameters arrive as the heap-header pointer, not a copy.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>
using cajeta_test::CajetaJit;

// Sanity check: arrays pass by pointer (callee mutation is visible
// to caller). Companion to the struct-by-pointer probes in
// StructViewTests.
TEST(BorrowDetectionTests, arrayParamMutationVisibleToCaller) {
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static void bump(int32[] xs) {\n"
        "        xs[0] = xs[0] + 100;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[3];\n"
        "        xs[0] = 7;\n"
        "        S.bump(xs);\n"
        "        return xs[0];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 107);
}

// Class-ref bound from another local: `Foo f = original;`
//
// Without Identifier-borrow detection, both `original` and `f`
// register drop entries pointing at the same heap object — the
// first drop frees the instance, the second drop double-frees.
// With the detection in place, only the original local owns the
// drop; `f` is a borrow. A double-free would crash glibc's heap
// consistency check.
TEST(BorrowDetectionTests, classRefAliasedFromLocalDoesNotDoubleFree) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 v;\n"
        "    public Tag(int32 i) { this.v = i; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Tag original = heap Tag(42);\n"
        "        Tag alias = original;\n"
        "        return alias.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Array bound from a method parameter: `int32[] local = paramArr;`
//
// Without Identifier-borrow detection, the local would register
// __cajeta_free_array on the heap header that the param already
// holds — heap corruption at scope exit when the local drops
// before the caller's still-live array.
TEST(BorrowDetectionTests, arrayAliasedFromParamDoesNotDoubleFree) {
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 readFirst(int32[] arr) {\n"
        "        int32[] alias = arr;\n"
        "        return alias[0];\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[3];\n"
        "        xs[0] = 99;\n"
        "        return S.readFirst(xs);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// Class ref bound from a method parameter — same pattern, class
// type instead of array. Same double-free risk without
// Identifier-borrow detection.
TEST(BorrowDetectionTests, classRefAliasedFromParamDoesNotDoubleFree) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 v;\n"
        "    public Tag(int32 i) { this.v = i; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 unwrap(Tag t) {\n"
        "        Tag alias = t;\n"
        "        return alias.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tag t = heap Tag(7);\n"
        "        return S.unwrap(t);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}
