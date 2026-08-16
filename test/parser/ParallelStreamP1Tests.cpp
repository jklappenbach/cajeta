// P1 — parallel stream foundation. Tests pin the minimum useful
// surface: `.parallel()` flag-flip, Splittable<T> on ArrayStream,
// driver fork/join for count(), and the no-split fallback path.
//
// See docs/specification/lang/stream/StreamParallelism.md for design and
// StreamParallelism.Examples.md for the full example/error reference.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int32_t runI32Diag(const std::string& src) {
    try {
        return runI32(src);
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                      << ": " << e.getMessage();
        return -1;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "std::exception: " << e.what();
        return -1;
    }
}

} // namespace

// 1.1 — parallel() is a no-op flag-flip on the existing Stream<T>
// shape. Calling it returns the same stream; the bit changes
// terminal dispatch but not pipeline construction.

// ------------------------------------------------------------------
// MERGED FOR CI COST. Compiling `.parallel()` pulls in the fork/join
// driver and the whole stream machinery: ~80 s per program, and it was
// paid 48 times here — 73 minutes, 27%% of the entire release subset.
// Measured, the compile is the whole cost: one assertion took 40 s and
// six took 41 s, so extra cases in a program are ~0.2 s each.
//
// The cases below are therefore grouped by functional area into one
// program each. NO assertion was dropped — every original case's
// program body is preserved VERBATIM as c<N>() and still checked; only
// the number of COMPILES changed (42 -> 5). Grouping is by area rather
// than one giant program so a crash still isolates to an area, which
// matters because this suite is a release gate that has caught
// platform-specific crashes.
//
// Six tests below are NOT merged: their assertions are not
// `EXPECT_EQ(runI32*(src), <int>)` (int64 returns, range checks), so
// folding them would have meant rewriting what they assert.
// ------------------------------------------------------------------

// CountAndFlag — 6 cases, ONE compile. Each case's program body is preserved
// VERBATIM as c<N>(); the score is a bitmap so a failure names the
// exact case that regressed:
//   bit 0  (     1) parallelFlagFlipReturnsSameStream
//   bit 1  (     2) parallelCountOnTinySourceStillCorrect
//   bit 2  (     4) parallelCountOnEmptySource
//   bit 3  (     8) sequentialFlipBackPreservesCount
//   bit 4  (    16) parallelCalledTwiceIsNoOp
//   bit 5  (    32) sequentialBeforeTakeClearsFlag

// Reduce — 7 cases, ONE compile. Each case's program body is preserved
// VERBATIM as c<N>(); the score is a bitmap so a failure names the
// exact case that regressed:
//   bit 0  (     1) parallelReduceSumsCorrectly
//   bit 1  (     2) parallelReduceOnTinySourceStillCorrect
//   bit 2  (     4) parallelReduceParallelDriverDirectCall
//   bit 3  (     8) parallelReduceLargeSourceCorrectness
//   bit 4  (    16) reduceParallelChainParallelizesDirectArrayHead
//   bit 5  (    32) parallelReduceDispatchesThroughFilter
//   bit 6  (    64) reduceParallelChainFilterChainParallelizes
TEST(ParallelStreamP1Tests, reduceMerged) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    static int32 c0() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // sum of 1..100 == 5050\n"
        "        return xs.stream().parallel().reduce(0, (a, b) -> a + b);\n"
        "        }\n"
        "    static int32 c1() {\n"
        "        int32[] xs = [10, 20, 30];\n"
        "        return xs.stream().parallel().reduce(0, (a, b) -> a + b);\n"
        "        }\n"
        "    static int32 c2() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        return ParallelDriver.reduceParallel<int32>(\n"
        "            xs.stream(), 0, (a, b) -> a + b);\n"
        "        }\n"
        "    static int32 c3() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ParallelDriver.reduceParallel<int32>(\n"
        "            xs.stream(), 0, (a, b) -> a + b);\n"
        "        }\n"
        "    static int32 c4() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // 1..100 sums to 5050\n"
        "        return ParallelDriver.reduceParallelChain<int32>(\n"
        "            xs.stream(), 0, (a, b) -> a + b);\n"
        "        }\n"
        "    static int32 c5() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // even-only sum: 2+4+...+100 = 2550\n"
        "        return xs.stream()\n"
        "                 .filter((x) -> x % 2 == 0)\n"
        "                 .parallel()\n"
        "                 .reduce(0, (a, b) -> a + b);\n"
        "        }\n"
        "    static int32 c6() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // even-only sum: 2+4+...+100 = 2550\n"
        "        return ParallelDriver.reduceParallelChain<int32>(\n"
        "            xs.stream().filter((x) -> x % 2 == 0),\n"
        "            0, (a, b) -> a + b);\n"
        "        }\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        if (D.c0() == 5050) { score = score + 1; }\n"
        "        if (D.c1() == 60) { score = score + 2; }\n"
        "        if (D.c2() == 15) { score = score + 4; }\n"
        "        if (D.c3() == 5050) { score = score + 8; }\n"
        "        if (D.c4() == 5050) { score = score + 16; }\n"
        "        if (D.c5() == 2550) { score = score + 32; }\n"
        "        if (D.c6() == 2550) { score = score + 64; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 127);
}

// Match — 17 cases, ONE compile. Each case's program body is preserved
// VERBATIM as c<N>(); the score is a bitmap so a failure names the
// exact case that regressed:
//   bit 0  (     1) parallelCountMatchesSequential
//   bit 1  (     2) parallelAnyMatchParallelDriverDirectCall
//   bit 2  (     4) parallelAnyMatchFalseWhenNoneMatch
//   bit 3  (     8) parallelAllMatchTrueWhenAllSatisfy
//   bit 4  (    16) parallelAllMatchFalseOnOneFailure
//   bit 5  (    32) parallelNoneMatchTrueWhenNoMatch
//   bit 6  (    64) parallelAnyMatchLargeSourceFindsMatch
//   bit 7  (   128) parallelAnyMatchLargeSourceNoMatch
//   bit 8  (   256) parallelAllMatchLargeSourceAllSatisfy
//   bit 9  (   512) parallelAllMatchLargeSourceOneFailure
//   bit 10 (  1024) parallelNoneMatchLargeSourceNoMatch
//   bit 11 (  2048) parallelNoneMatchLargeSourceFindsMatch
//   bit 12 (  4096) parallelAnyMatchDispatchesThroughFilter
//   bit 13 (  8192) parallelAnyMatchFalseThroughFilter
//   bit 14 ( 16384) parallelAllMatchDispatchesThroughFilter
//   bit 15 ( 32768) parallelNoneMatchDispatchesThroughFilter
//   bit 16 ( 65536) parallelFindFirstEmptyOnNoMatch

// Rejects — 6 cases, ONE compile. Each case's program body is preserved
// VERBATIM as c<N>(); the score is a bitmap so a failure names the
// exact case that regressed:
//   bit 0  (     1) takeOnParallelStreamRejects
//   bit 1  (     2) skipOnParallelStreamRejects
//   bit 2  (     4) reduceParallelChainRejectsStatefulInChain
//   bit 3  (     8) parallelAnyMatchRejectsStatefulInChain
//   bit 4  (    16) twoArgFoldOnParallelStreamRejects
//   bit 5  (    32) parallelFindFirstRejectsStatefulInChain

// FilterDispatch — 6 cases, ONE compile. Each case's program body is preserved
// VERBATIM as c<N>(); the score is a bitmap so a failure names the
// exact case that regressed:
//   bit 0  (     1) parallelThroughFilterStillCounts
//   bit 1  (     2) parallelForEachDispatchesThroughFilter
//   bit 2  (     4) parallelCollectViaSupplierAggregatesAll
//   bit 3  (     8) parallelCollectViaSupplierPreservesElements
//   bit 4  (    16) parallelCollectViaSupplierThroughFilter
//   bit 5  (    32) sequentialBeforeCollectClearsParallelFlag
TEST(ParallelStreamP1Tests, filterDispatchMerged) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.collection.Collector;\n"
        "import cajeta.collection.Collectors;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    static int32 c0() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        // count of evens 0..99 == 50\n"
        "        return xs.stream().parallel()\n"
        "                 .filter((x) -> x % 2 == 0)\n"
        "                 .count();\n"
        "        }\n"
        "    static int32 c1() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        AtomicInt32 c = heap AtomicInt32(0);\n"
        "        xs.stream()\n"
        "          .filter((x) -> x % 2 == 0)\n"
        "          .parallel()\n"
        "          .forEach((x) -> { c.fetchAdd(1); });\n"
        "        // 50 evens in 1..100\n"
        "        return c.load();\n"
        "        }\n"
        "    static int32 c2() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = xs.stream().parallel().collect(c);\n"
        "        return out.count();\n"
        "        }\n"
        "    static int32 c3() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 j = 0; j < 100; j = j + 1) { xs[j] = j + 1; }\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = xs.stream().parallel().collect(c);\n"
        "        int32 total = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < out.count()) {\n"
        "            total = total + out.get(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return total;\n"
        "        }\n"
        "    static int32 c4() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = xs.stream().filter((int32 x) -> x % 2 == 0).parallel().collect(c);\n"
        "        int32 total = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < out.count()) {\n"
        "            total = total + out.get(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return total;\n"
        "        }\n"
        "    static int32 c5() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = xs.stream().parallel().sequential().collect(c);\n"
        "        return out.count();\n"
        "        }\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        if (D.c0() == 50) { score = score + 1; }\n"
        "        if (D.c1() == 50) { score = score + 2; }\n"
        "        if (D.c2() == 100) { score = score + 4; }\n"
        "        if (D.c3() == 5050) { score = score + 8; }\n"
        "        if (D.c4() == 2550) { score = score + 16; }\n"
        "        if (D.c5() == 100) { score = score + 32; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 63);
}

