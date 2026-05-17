//
// P2.2 (second slice) — Stream intermediate combinators. The wrapper-
// stream pattern holds another Stream by reference (`Stream<T> source`
// field) and pulls from it lazily inside its own `next()`. P1.1 fixed
// the underlying template-instantiation field codegen so this layout
// produces a `ptr` slot (matching the ctor signature's pass-by-pointer
// convention) instead of an inline `{ ptr vtable }` body that mismatches
// at JIT verify time.
//
// First wrapper landed: TakeStream<T>. Tests instantiate it directly
// (the chained `xs.stream().take(n)` form depends on P6.6 chained-form
// completion, separately tracked). Construction goes through an
// explicit ArrayStream local + upcast to Stream<int32>; the
// `xs.stream()` intrinsic returns an ArrayStream<int32> and assigning
// it directly to a `Stream<int32>` local trips a separate JIT-symbol-
// resolution issue (tracked as a follow-up; doesn't affect P1.1).
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
