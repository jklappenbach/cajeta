// Stream<T> terminals + intermediates over class-typed T. This
// covers ground that PrimitiveHashMap / Stream tests didn't:
// inline lambdas with class T params, dispatched calls to forEach /
// findFirst / anyMatch / reduce with class T flowing through the
// argument and (sometimes) return slot.
//
// Two underlying compiler fixes landed alongside these tests:
//   1. MethodCallExpression's lambda-as-arg expectedType
//      propagation now walks the receiver's parent chain to find
//      inherited methods like Stream<T>::forEach. Without this,
//      `ArrayStream<Counter>.getMethods()` doesn't include forEach
//      (declared on Stream<T>), the lambda's expectedType stays
//      null, and an expression-bodied lambda's inferred return
//      type doesn't match the method's `(T) -> void` signature —
//      causing the call to silently drop from the IR.
//   2. Stream<T>::findFirst no longer keeps a `T v = o.get();`
//      local. The local registered a drop entry for `v`, but class-
//      typed elements are owned by the stream source (not by the
//      stream's iteration locals); the drop freed the element and
//      use-after-free crashed the caller. Calling `o.get()` twice
//      (in the predicate + the return) keeps both results as
//      immediate temporaries without drop entries.

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

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.stream.ArrayStream;\n"
    "public class Counter {\n"
    "    public int32 v;\n"
    "    public Counter(int32 i) { this.v = i; }\n"
    "}\n";

} // namespace

// --- forEach over class T ---------------------------------------------

TEST(StreamClassTTests, forEachInlineLambdaExpressionBody) {
    auto src = std::string(PRELUDE) +
        "public class Total { public static int32 sum = 0; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        s.forEach((Counter c) -> Total.sum = Total.sum + c.v);\n"
        "        return Total.sum;\n"  // 6
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

TEST(StreamClassTTests, forEachInlineLambdaBlockBody) {
    auto src = std::string(PRELUDE) +
        "public class Total { public static int32 sum = 0; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        s.forEach((Counter c) -> { Total.sum = Total.sum + c.v; });\n"
        "        return Total.sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

TEST(StreamClassTTests, forEachPreDeclaredLambda) {
    auto src = std::string(PRELUDE) +
        "public class Total { public static int32 sum = 0; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        (Counter) -> void addv = (Counter c) -> { Total.sum = Total.sum + c.v; };\n"
        "        s.forEach(addv);\n"
        "        return Total.sum;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// --- findFirst over class T -------------------------------------------

TEST(StreamClassTTests, findFirstFindsMatch) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        Optional<Counter> result = s.findFirst((Counter c) -> { return c.v == 2; });\n"
        "        if (result.isPresent()) { return result.get().v; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(StreamClassTTests, findFirstReturnsEmptyWhenNoMatch) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        Optional<Counter> result = s.findFirst((Counter c) -> { return c.v > 99; });\n"
        "        if (result.isPresent()) { return 99; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// --- anyMatch / noneMatch / allMatch over class T ----------------------

TEST(StreamClassTTests, anyMatchTrueWhenOneMatches) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        if (s.anyMatch((Counter c) -> { return c.v == 2; })) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamClassTTests, allMatchFalseWhenOneFails) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        if (s.allMatch((Counter c) -> { return c.v < 3; })) { return 99; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(StreamClassTTests, noneMatchTrueWhenNoneMatches) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        if (s.noneMatch((Counter c) -> { return c.v > 99; })) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- reduce over class T -----------------------------------------------
//
// reduce(seed, (T, T) -> T) — for class T, the lambda receives two
// class pointers and returns one. Body can construct a new instance
// or return one of its args.

TEST(StreamClassTTests, reduceReturnsBNoNew) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        Counter seed = heap Counter(0);\n"
        "        Counter result = s.reduce(seed, (Counter a, Counter b) -> {\n"
        "            return b;\n"  // last element wins
        "        });\n"
        "        return result.v;\n"  // 3
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamClassTTests, reduceAccumulatesViaNew) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter[] xs = [ heap Counter(1), heap Counter(2), heap Counter(3) ];\n"
        "        ArrayStream<Counter> s = heap ArrayStream<Counter>(xs, 3);\n"
        "        Counter seed = heap Counter(0);\n"
        "        Counter result = s.reduce(seed, (Counter a, Counter b) -> {\n"
        "            return heap Counter(a.v + b.v);\n"
        "        });\n"
        "        return result.v;\n"  // 0 + 1 + 2 + 3 = 6
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}
