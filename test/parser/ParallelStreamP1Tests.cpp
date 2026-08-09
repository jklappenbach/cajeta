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
TEST(ParallelStreamP1Tests, countAndFlagMerged) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static int32 c0() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        // Just check that .parallel() compiles + returns a Stream\n"
        "        // we can dispatch on. count() on the parallel head\n"
        "        // gives the same answer as sequential.\n"
        "        return xs.stream().parallel().count();\n"
        "        }\n"
        "    static int32 c1() {\n"
        "        int32[] xs = [7, 8, 9];\n"
        "        return xs.stream().parallel().count();\n"
        "        }\n"
        "    static int32 c2() {\n"
        "        int32[] xs = heap int32[0];\n"
        "        return xs.stream().parallel().count();\n"
        "        }\n"
        "    static int32 c3() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5, 6, 7, 8];\n"
        "        return xs.stream().parallel().sequential().count();\n"
        "        }\n"
        "    static int32 c4() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        return xs.stream().parallel().parallel().count();\n"
        "        }\n"
        "    static int32 c5() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        return xs.stream().parallel().sequential().take(2).count();\n"
        "        }\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        if (D.c0() == 5) { score = score + 1; }\n"
        "        if (D.c1() == 3) { score = score + 2; }\n"
        "        if (D.c2() == 0) { score = score + 4; }\n"
        "        if (D.c3() == 8) { score = score + 8; }\n"
        "        if (D.c4() == 3) { score = score + 16; }\n"
        "        if (D.c5() == 2) { score = score + 32; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 63);
}

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
TEST(ParallelStreamP1Tests, matchMerged) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    static int32 c0() {\n"
        "        int32[] xs = heap int32[1000];\n"
        "        for (int32 i = 0; i < 1000; i = i + 1) { xs[i] = i; }\n"
        "        return xs.stream().parallel().count();\n"
        "        }\n"
        "    static int32 c1() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        boolean b = ParallelDriver.anyMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 2);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c2() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        boolean b = ParallelDriver.anyMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 99);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c3() {\n"
        "        int32[] xs = [2, 4, 6, 8];\n"
        "        boolean b = ParallelDriver.allMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x % 2 == 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c4() {\n"
        "        int32[] xs = [2, 4, 7, 8];\n"
        "        boolean b = ParallelDriver.allMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x % 2 == 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c5() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        boolean b = ParallelDriver.noneMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 99);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c6() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = ParallelDriver.anyMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x == 73);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c7() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = ParallelDriver.anyMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 9999);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c8() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = ParallelDriver.allMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c9() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        xs[57] = -1;\n"
        "        boolean b = ParallelDriver.allMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c10() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = ParallelDriver.noneMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 9999);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c11() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        boolean b = ParallelDriver.noneMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x == 42);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "        }\n"
        "    static int32 c12() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        boolean hit = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .anyMatch((x) -> x == 50);\n"
        "        return hit ? 1 : 0;\n"
        "        }\n"
        "    static int32 c13() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // 200 isn't in the evens 2..100 range\n"
        "        boolean hit = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .anyMatch((x) -> x == 200);\n"
        "        return hit ? 1 : 0;\n"
        "        }\n"
        "    static int32 c14() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // every even is > 0\n"
        "        boolean all = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .allMatch((x) -> x > 0);\n"
        "        return all ? 1 : 0;\n"
        "        }\n"
        "    static int32 c15() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // 7 is odd â filtered out â so noneMatch is true\n"
        "        boolean none = xs.stream()\n"
        "                         .filter((x) -> x % 2 == 0)\n"
        "                         .parallel()\n"
        "                         .noneMatch((x) -> x == 7);\n"
        "        return none ? 1 : 0;\n"
        "        }\n"
        "    static int32 c16() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Optional<int32> o = xs.stream().parallel()\n"
        "                              .findFirst((x) -> x > 9999);\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "        }\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        if (D.c0() == 1000) { score = score + 1; }\n"
        "        if (D.c1() == 1) { score = score + 2; }\n"
        "        if (D.c2() == 0) { score = score + 4; }\n"
        "        if (D.c3() == 1) { score = score + 8; }\n"
        "        if (D.c4() == 0) { score = score + 16; }\n"
        "        if (D.c5() == 1) { score = score + 32; }\n"
        "        if (D.c6() == 1) { score = score + 64; }\n"
        "        if (D.c7() == 0) { score = score + 128; }\n"
        "        if (D.c8() == 1) { score = score + 256; }\n"
        "        if (D.c9() == 0) { score = score + 512; }\n"
        "        if (D.c10() == 1) { score = score + 1024; }\n"
        "        if (D.c11() == 0) { score = score + 2048; }\n"
        "        if (D.c12() == 1) { score = score + 4096; }\n"
        "        if (D.c13() == 0) { score = score + 8192; }\n"
        "        if (D.c14() == 1) { score = score + 16384; }\n"
        "        if (D.c15() == 1) { score = score + 32768; }\n"
        "        if (D.c16() == -1) { score = score + 65536; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 131071);
}

// Rejects — 6 cases, ONE compile. Each case's program body is preserved
// VERBATIM as c<N>(); the score is a bitmap so a failure names the
// exact case that regressed:
//   bit 0  (     1) takeOnParallelStreamRejects
//   bit 1  (     2) skipOnParallelStreamRejects
//   bit 2  (     4) reduceParallelChainRejectsStatefulInChain
//   bit 3  (     8) parallelAnyMatchRejectsStatefulInChain
//   bit 4  (    16) twoArgFoldOnParallelStreamRejects
//   bit 5  (    32) parallelFindFirstRejectsStatefulInChain
TEST(ParallelStreamP1Tests, rejectsMerged) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    static int32 c0() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        try {\n"
        "            return xs.stream().parallel().take(2).count();\n"
        "        } catch (Exception e) {\n"
        "            return -42;\n"
        "        }\n"
        "        }\n"
        "    static int32 c1() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        try {\n"
        "            return xs.stream().parallel().skip(2).count();\n"
        "        } catch (Exception e) {\n"
        "            return -42;\n"
        "        }\n"
        "        }\n"
        "    static int32 c2() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        try {\n"
        "            return ParallelDriver.reduceParallelChain<int32>(\n"
        "                xs.stream().take(5), 0, (a, b) -> a + b);\n"
        "        } catch (Exception e) {\n"
        "            return -77;\n"
        "        }\n"
        "        }\n"
        "    static int32 c3() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        try {\n"
        "            boolean hit = xs.stream().take(5).parallel()\n"
        "                            .anyMatch((x) -> x > 2);\n"
        "            return hit ? 1 : 0;\n"
        "        } catch (Exception e) {\n"
        "            return -77;\n"
        "        }\n"
        "        }\n"
        "    static int32 c4() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        try {\n"
        "            return xs.stream().parallel().fold(\n"
        "                0, (int32 a, int32 x) -> a + x);\n"
        "        } catch (Exception e) {\n"
        "            return -42;\n"
        "        }\n"
        "        }\n"
        "    static int32 c5() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        try {\n"
        "            Optional<int32> o = xs.stream().take(5).parallel()\n"
        "                                  .findFirst((x) -> x > 2);\n"
        "            if (o.isPresent()) { return o.get(); }\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            return -77;\n"
        "        }\n"
        "        }\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        "        if (D.c0() == -42) { score = score + 1; }\n"
        "        if (D.c1() == -42) { score = score + 2; }\n"
        "        if (D.c2() == -77) { score = score + 4; }\n"
        "        if (D.c3() == -77) { score = score + 8; }\n"
        "        if (D.c4() == -42) { score = score + 16; }\n"
        "        if (D.c5() == -77) { score = score + 32; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 63);
}

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

TEST(ParallelStreamP1Tests, foldCombinerSeqI32ToI64) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        // sum widened to int64: 1+2+3+4+5 == 15\n"
        "        return xs.stream().fold(\n"
        "            0L,\n"
        "            (int64 a, int32 x) -> a + (int64) x,\n"
        "            (int64 a, int64 b) -> a + b);\n"
        "    }\n"
        "}\n";
    auto runI64 = [](const std::string& s) {
        auto jit = CajetaJit::compile(s, "test.D");
        return jit->lookup<int64_t (*)()>("run")();
    };
    EXPECT_EQ(runI64(src), 15LL);
}
TEST(ParallelStreamP1Tests, foldCombinerParallelDirectArrayHead) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // sum(1..100) widened to int64 == 5050\n"
        "        return xs.stream().parallel().fold(\n"
        "            0L,\n"
        "            (int64 a, int32 x) -> a + (int64) x,\n"
        "            (int64 a, int64 b) -> a + b);\n"
        "    }\n"
        "}\n";
    auto runI64 = [](const std::string& s) -> int64_t {
        try {
            auto jit = CajetaJit::compile(s, "test.D");
            return jit->lookup<int64_t (*)()>("run")();
        } catch (cajeta::Exception& e) {
            ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                          << ": " << e.getMessage();
            return -1;
        }
    };
    EXPECT_EQ(runI64(src), 5050LL);
}
TEST(ParallelStreamP1Tests, foldCombinerParallelThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // even-only sum widened to int64: 2+4+...+100 == 2550\n"
        "        return xs.stream()\n"
        "                 .filter((x) -> x % 2 == 0)\n"
        "                 .parallel()\n"
        "                 .fold(\n"
        "                     0L,\n"
        "                     (int64 a, int32 x) -> a + (int64) x,\n"
        "                     (int64 a, int64 b) -> a + b);\n"
        "    }\n"
        "}\n";
    auto runI64 = [](const std::string& s) -> int64_t {
        try {
            auto jit = CajetaJit::compile(s, "test.D");
            return jit->lookup<int64_t (*)()>("run")();
        } catch (cajeta::Exception& e) {
            ADD_FAILURE() << "cajeta::Exception " << e.getErrorId()
                          << ": " << e.getMessage();
            return -1;
        }
    };
    EXPECT_EQ(runI64(src), 2550LL);
}
TEST(ParallelStreamP1Tests, parallelFindFirstReturnsAMatchingElement) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // findFirst(x > 50) under parallel = findAny — any of\n"
        "        // 51..100 is a valid answer.\n"
        "        Optional<int32> o = xs.stream().parallel()\n"
        "                              .findFirst((x) -> x > 50);\n"
        "        if (!o.isPresent()) { return -1; }\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    int32_t r = runI32Diag(src);
    EXPECT_GT(r, 50);
    EXPECT_LE(r, 100);
}
TEST(ParallelStreamP1Tests, parallelFindFirstThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // Evens 2..100; findAny(x > 50) — any even in 52..100.\n"
        "        Optional<int32> o = xs.stream()\n"
        "                              .filter((x) -> x % 2 == 0)\n"
        "                              .parallel()\n"
        "                              .findFirst((x) -> x > 50);\n"
        "        if (!o.isPresent()) { return -1; }\n"
        "        return o.get();\n"
        "    }\n"
        "}\n";
    int32_t r = runI32Diag(src);
    EXPECT_GT(r, 50);
    EXPECT_LE(r, 100);
    EXPECT_EQ(r % 2, 0);
}
TEST(ParallelStreamP1Tests, foldCombinerRejectsStatefulInChain) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        try {\n"
        "            return xs.stream().take(5).parallel().fold(\n"
        "                0L,\n"
        "                (int64 a, int32 x) -> a + (int64) x,\n"
        "                (int64 a, int64 b) -> a + b);\n"
        "        } catch (Exception e) {\n"
        "            return -77L;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto runI64 = [](const std::string& s) {
        auto jit = CajetaJit::compile(s, "test.D");
        return jit->lookup<int64_t (*)()>("run")();
    };
    EXPECT_EQ(runI64(src), -77LL);
}
