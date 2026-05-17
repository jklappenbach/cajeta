//
// P2.2 — Stream intermediate combinators. The wrapper-stream pattern
// holds another Stream by reference (`Stream<T> source` field) and
// pulls from it lazily inside its own `next()`. P1.1 fixed the
// underlying template-instantiation field codegen so this layout
// produces a `ptr` slot (matching the ctor signature's pass-by-
// pointer convention) instead of an inline `{ ptr vtable }` body
// that mismatches at JIT verify time.
//
// Tests cover all five intermediate combinators: TakeStream,
// SkipStream, FilterStream, MapStream, PeekStream, and FlatMapStream.
// Each is exercised individually and (where naturally composable)
// via chained construction patterns.
//
// Constructors use the inline-`.stream()` form throughout — see
// InlineCtorArgTests.cpp / UpcastInitializerTests.cpp for the
// supporting machinery (MCE resolvedType pinning + subtype-aware
// ctor lookup + Phase 1/2 quiescence-loop). The chained-method
// form `xs.stream().take(n)` itself depends on P6.6 chained-form
// completion (separately tracked).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(StreamIntermediateTests, takeStreamLimitsElementCount) {
    // 5-element source, take(3), then count → 3.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.Stream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 5);\n"
        "        Stream<int32> src = as;\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(src, 3);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamIntermediateTests, takeStreamSumsLimitedPrefix) {
    // Confirms values come from the underlying source in order.
    // 5-element source, take(3) → {1,2,3}, reduce + → 6.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.Stream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 5);\n"
        "        Stream<int32> src = as;\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(src, 3);\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };\n"
        "        return t.reduce(0, add);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

TEST(StreamIntermediateTests, takeStreamLargerThanSourceStops) {
    // take(99) on a 3-element source still only yields 3 — the underlying
    // stream exhausts first and the wrapper propagates the empty Optional.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.Stream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30};\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 3);\n"
        "        Stream<int32> src = as;\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(src, 99);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamIntermediateTests, takeStreamZeroYieldsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.Stream;\n"
        "import cajeta.lang.TakeStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 3);\n"
        "        Stream<int32> src = as;\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(src, 0);\n"
        "        return t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(StreamIntermediateTests, takeStreamFindFirstSearchesLimitedPrefix) {
    // take(3) over {1,2,3,4,5}; findFirst(v > 3) within that window misses
    // — even though 4 satisfies the predicate, the take window stopped at 3.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.Stream;\n"
        "import cajeta.lang.TakeStream;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        ArrayStream<int32> as = heap ArrayStream<int32>(xs, 5);\n"
        "        Stream<int32> src = as;\n"
        "        TakeStream<int32> t = heap TakeStream<int32>(src, 3);\n"
        "        (int32) -> boolean over3 = (int32 v) -> { return v > 3; };\n"
        "        Optional<int32> hit = t.findFirst(over3);\n"
        "        if (hit.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// =============================================================
// SkipStream — discards the first n elements then passes through.
// =============================================================

TEST(StreamIntermediateTests, skipStreamCountAfterSkip) {
    // 5 elements, skip 2 → 3 remaining.
    auto src =
        "package test;\n"
        "import cajeta.lang.SkipStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        SkipStream<int32> s = heap SkipStream<int32>(xs.stream(), 2);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamIntermediateTests, skipStreamSumsTail) {
    // skip(2) over {1,2,3,4,5} → {3,4,5}, sum = 12.
    auto src =
        "package test;\n"
        "import cajeta.lang.SkipStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        SkipStream<int32> s = heap SkipStream<int32>(xs.stream(), 2);\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };\n"
        "        return s.reduce(0, add);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

TEST(StreamIntermediateTests, skipStreamLargerThanSource) {
    // skip(99) on a 3-element source → empty.
    auto src =
        "package test;\n"
        "import cajeta.lang.SkipStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        SkipStream<int32> s = heap SkipStream<int32>(xs.stream(), 99);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(StreamIntermediateTests, skipStreamZeroKeepsAll) {
    auto src =
        "package test;\n"
        "import cajeta.lang.SkipStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        SkipStream<int32> s = heap SkipStream<int32>(xs.stream(), 0);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// =============================================================
// FilterStream — yields only elements matching `pred`.
// =============================================================

TEST(StreamIntermediateTests, filterStreamEvensCount) {
    auto src =
        "package test;\n"
        "import cajeta.lang.FilterStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6};\n"
        "        (int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };\n"
        "        FilterStream<int32> s = heap FilterStream<int32>(xs.stream(), isEven);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamIntermediateTests, filterStreamSumOfMatching) {
    auto src =
        "package test;\n"
        "import cajeta.lang.FilterStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6};\n"
        "        (int32) -> boolean over3 = (int32 v) -> { return v > 3; };\n"
        "        FilterStream<int32> s = heap FilterStream<int32>(xs.stream(), over3);\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };\n"
        "        return s.reduce(0, add);\n"  // 4+5+6 = 15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

TEST(StreamIntermediateTests, filterStreamNoMatches) {
    auto src =
        "package test;\n"
        "import cajeta.lang.FilterStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        (int32) -> boolean impossible = (int32 v) -> { return v > 100; };\n"
        "        FilterStream<int32> s = heap FilterStream<int32>(xs.stream(), impossible);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// =============================================================
// MapStream — element-type-changing wrapper (Stream<T> → Stream<R>).
// =============================================================

TEST(StreamIntermediateTests, mapStreamDoubles) {
    auto src =
        "package test;\n"
        "import cajeta.lang.MapStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        (int32) -> int32 dbl = (int32 v) -> { return v * 2; };\n"
        "        MapStream<int32, int32> s = heap MapStream<int32, int32>(xs.stream(), dbl);\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };\n"
        "        return s.reduce(0, add);\n"  // 2+4+6 = 12
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}

TEST(StreamIntermediateTests, mapStreamCountUnchanged) {
    auto src =
        "package test;\n"
        "import cajeta.lang.MapStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30, 40};\n"
        "        (int32) -> int32 negate = (int32 v) -> { return 0 - v; };\n"
        "        MapStream<int32, int32> s = heap MapStream<int32, int32>(xs.stream(), negate);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

TEST(StreamIntermediateTests, mapStreamEmptyPassesThrough) {
    auto src =
        "package test;\n"
        "import cajeta.lang.MapStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[0];\n"
        "        (int32) -> int32 sq = (int32 v) -> { return v * v; };\n"
        "        MapStream<int32, int32> s = heap MapStream<int32, int32>(xs.stream(), sq);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// =============================================================
// PeekStream — side-effecting passthrough.
// =============================================================

TEST(StreamIntermediateTests, peekStreamFiresOnEachPull) {
    // Peek fn writes each element into a counter via a captured array
    // (closure semantics: lambdas capture references to surrounding
    // mutable state). Count the visits by checking the resulting sum.
    auto src =
        "package test;\n"
        "import cajeta.lang.PeekStream;\n"
        "public final class S {\n"
        "    public static int32 total;\n"
        "    public static int32 run() {\n"
        "        S.total = 0;\n"
        "        int32[] xs = {1, 2, 3, 4};\n"
        "        (int32) -> void bump = (int32 v) -> { S.total = S.total + v; };\n"
        "        PeekStream<int32> s = heap PeekStream<int32>(xs.stream(), bump);\n"
        "        s.count();\n"
        "        return S.total;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);  // 1+2+3+4
}

TEST(StreamIntermediateTests, peekStreamPassesValuesThrough) {
    // PeekStream's count() must equal the source count: peek doesn't
    // filter / map / drop anything.
    auto src =
        "package test;\n"
        "import cajeta.lang.PeekStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        (int32) -> void noop = (int32 v) -> {};\n"
        "        PeekStream<int32> s = heap PeekStream<int32>(xs.stream(), noop);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// =============================================================
// FlatMapStream — flattens nested streams. State between pulls.
// =============================================================

TEST(StreamIntermediateTests, flatMapStreamRepeatedSingletonStreams) {
    // For each outer element v, yield a single-element ArrayStream<int32>
    // containing v*10. Flatten → {10, 20, 30}. Count = 3.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.FlatMapStream;\n"
        "import cajeta.lang.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        (int32) -> Stream<int32> mk = (int32 v) -> {\n"
        "            int32[] one = new int32[1];\n"
        "            one[0] = v * 10;\n"
        "            ArrayStream<int32> inner = heap ArrayStream<int32>(one, 1);\n"
        "            return inner;\n"
        "        };\n"
        "        FlatMapStream<int32, int32> s =\n"
        "            heap FlatMapStream<int32, int32>(xs.stream(), mk);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamIntermediateTests, flatMapStreamEmptyInnerStreamsSkipped) {
    // Always return an empty inner stream → flattened count = 0
    // even when outer is non-empty. Verifies the inner-drain loop
    // pulls the next outer element rather than yielding empty itself.
    auto src =
        "package test;\n"
        "import cajeta.lang.ArrayStream;\n"
        "import cajeta.lang.FlatMapStream;\n"
        "import cajeta.lang.Stream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        (int32) -> Stream<int32> mkEmpty = (int32 v) -> {\n"
        "            int32[] none = new int32[0];\n"
        "            ArrayStream<int32> inner = heap ArrayStream<int32>(none, 0);\n"
        "            return inner;\n"
        "        };\n"
        "        FlatMapStream<int32, int32> s =\n"
        "            heap FlatMapStream<int32, int32>(xs.stream(), mkEmpty);\n"
        "        return s.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
