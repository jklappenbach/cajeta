// Stream<T>.fold<R> — cross-type fold via method-level templates.
// See cajeta-docs/stdlib/MethodLevelTemplate.md (the foundation) and
// cajeta-docs/stdlib/Streams.md § fold. `reduce(T, (T,T) -> T)` is
// the same-type wrapper; fold lets the accumulator type R differ
// from the element type T.

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

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.stream.ArrayStream;\n";

} // namespace

// Same-shape fold (R == T) using fold directly. Confirms the
// templated combinator works in the degenerate case that `reduce`
// covers.
TEST(StreamFoldTests, foldSameTypeMatchesReduce) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 4);\n"
        "        return s.fold(0, (int32 acc, int32 x) -> acc + x);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Cross-type fold: int32 stream into int64 accumulator. The fold's
// R binds to int64 from the seed; the accumulator stays wide enough
// to hold the sum.
TEST(StreamFoldTests, foldI32StreamToI64) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = { 1, 2, 3 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        return s.fold(100L, (int64 acc, int32 x) -> acc + 1L);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 103LL);
}

// reduce now delegates to fold<T>. Verify the same-type contract
// still produces the right answer end-to-end.
TEST(StreamFoldTests, reduceStillWorks) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 2, 3, 5 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        return s.reduce(1, (int32 a, int32 b) -> a * b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

// fold over class T with primitive R. The lambda body `acc + c.v`
// is well-typed at codegen time (acc:int32, c.v:int32 → int32), but
// LambdaExpression::resolveTypes runs without the lambda's parameter
// scope and the BinaryOpExpression for `acc + c.v` falls back to the
// RHS's type when LHS doesn't resolve. With `c` (Counter) unresolved,
// `c.v` resolves to Counter rather than int32, and the lambda's
// signature lands as `(int32, Counter) -> Counter`. The body produces
// i32 at codegen, mismatching the declared `ret ptr` signature, and
// JIT verify rejects.
//
// This is a pre-existing lambda-body-resolution issue surfaced by
// the fold<R> path — same shape would break for any lambda whose
// body references a class-typed parameter without scope. Fixing it
// requires either deferring lambda resolveTypes until after the
// outer call's T-vars are bound, OR re-resolving the lambda body
// with the parameter scope active. Tracked separately.
TEST(StreamFoldTests, DISABLED_foldClassTToPrimitiveR) {
    auto src = std::string(PRELUDE) +
        "public class Counter {\n"
        "    public int32 v;\n"
        "    public Counter(int32 i) { this.v = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] cs = { new Counter(1), new Counter(2), new Counter(3) };\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(cs, 3);\n"
        "        return s.fold(0, (int32 acc, Counter c) -> acc + c.v);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}
