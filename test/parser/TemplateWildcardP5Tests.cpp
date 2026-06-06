// Step 5b of template wildcards. The payoff: the parallel chain walker
// now sees through type-changing wrappers (MapStream, FlatMapStream,
// MapOr*Stream) because `unwrap()` returns `Stream<?>` and the cursor
// locals in ParallelDriver are wildcard-typed. A `.map().parallel()`
// stream that previously fell back to sequential (the walker stopped
// at the MapStream and reported splittableSize=-1) now reaches the
// splittable root, forks per share, and rebuilds the MapStream chain
// over each worker's slice via cloneChainOver.
//
// These tests use `.reduce` as the terminal because it's the simplest
// parallel terminal with a verifiable scalar return.

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
    "import cajeta.lang.stream.ArrayStream;\n";

} // namespace

// .map().parallel().reduce — the headline case. Doubles every element
// then sums under parallel reduce. Sum of [1..N]*2 with the
// 0-seeded reduce equals N*(N+1).
TEST(TemplateWildcardP5Tests, mapThenParallelReduceSums) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        int32 r = s.map((int32 v) -> v * 2).parallel().reduce(0, (int32 a, int32 b) -> a + b);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    // 1+2+3+4+5+6+7+8 = 36; doubled = 72.
    EXPECT_EQ(runI32(src), 72);
}

// .parallel().map().reduce — wrapper-after-parallel. The chain walker
// must still find the splittable root after stepping past the
// MapStream, even though the parallel flag was set on the upstream
// ArrayStream and the wrapper-walk starts at the MapStream head.
TEST(TemplateWildcardP5Tests, parallelThenMapReduceSums) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        int32 r = s.parallel().map((int32 v) -> v * 2).reduce(0, (int32 a, int32 b) -> a + b);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 72);
}

// .filter().map().parallel().reduce — two wrappers, one of them
// type-changing. Filter even numbers, double them, sum. With
// xs=[1..10], evens are [2,4,6,8,10], doubled [4,8,12,16,20], sum=60.
TEST(TemplateWildcardP5Tests, filterMapParallelReduceSums) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[10];\n"
        "        int32 i = 0;\n"
        "        while (i < 10) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        int32 r = s.filter((int32 v) -> v % 2 == 0)\n"
        "                  .map((int32 v) -> v * 2)\n"
        "                  .parallel()\n"
        "                  .reduce(0, (int32 a, int32 b) -> a + b);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

// .map().parallel().count — count terminal walks `next()` to
// exhaustion. Under .parallel(), reduceParallelChain isn't involved
// (count has its own path), but the wildcard work in
// `forEachParallelChain`-style terminals should still apply via
// whichever terminal count uses. Sanity-check: map preserves
// element count; .count() should match source length.
TEST(TemplateWildcardP5Tests, mapParallelCountUnchanged) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[12];\n"
        "        int32 i = 0;\n"
        "        while (i < 12) {\n"
        "            xs[i] = i;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        return s.map((int32 v) -> v * 3).parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}
