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
TEST(ParallelStreamP1Tests, parallelFlagFlipReturnsSameStream) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        // Just check that .parallel() compiles + returns a Stream\n"
        "        // we can dispatch on. count() on the parallel head\n"
        "        // gives the same answer as sequential.\n"
        "        return xs.stream().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 1.2 — parallel().count() on a sufficiently large source actually
// forks. Result must equal sequential count.
TEST(ParallelStreamP1Tests, parallelCountMatchesSequential) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[1000];\n"
        "        for (int32 i = 0; i < 1000; i = i + 1) { xs[i] = i; }\n"
        "        return xs.stream().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1000);
}

// 1.3 — too-small source falls back to sequential (no fibers
// spawned). Result still correct.
TEST(ParallelStreamP1Tests, parallelCountOnTinySourceStillCorrect) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [7, 8, 9];\n"
        "        return xs.stream().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 1.4 — empty source.
TEST(ParallelStreamP1Tests, parallelCountOnEmptySource) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[0];\n"
        "        return xs.stream().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// 1.5 — `.sequential()` flips the flag back.
TEST(ParallelStreamP1Tests, sequentialFlipBackPreservesCount) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5, 6, 7, 8];\n"
        "        return xs.stream().parallel().sequential().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// 1.6 — Idempotent .parallel(): calling twice is the same as once.
TEST(ParallelStreamP1Tests, parallelCalledTwiceIsNoOp) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        return xs.stream().parallel().parallel().count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 3);
}

// 1.7a — parallel reduce via Stream<T>.reduce. Stream<T>.reduce
// delegates to fold which walks next() through the vtable, so the
// existing path Just Works on a Splittable source. (Wrapper-aware
// parallel dispatch — actually forking work — lands in P2 alongside
// the unwind logic.)
TEST(ParallelStreamP1Tests, parallelReduceSumsCorrectly) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // sum of 1..100 == 5050\n"
        "        return xs.stream().parallel().reduce(0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 5050);
}

// 1.7b — parallel reduce on a tiny source still gives the right
// answer. Today this is just Stream<T>.reduce → fold → next(); once
// P2's split-and-spawn driver lands, sources below MIN_PER_SPLIT
// will fall back to the same sequential walk.
TEST(ParallelStreamP1Tests, parallelReduceOnTinySourceStillCorrect) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [10, 20, 30];\n"
        "        return xs.stream().parallel().reduce(0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 60);
}

// 1.7c — direct call into ParallelDriver.reduceParallel<T>. This is
// the test that explicitly exercises the dispatch fix: a method-
// templated static (`reduceParallel<T>`) taking a `Splittable<T>`
// formal, called with an `ArrayStream<int32>` arg, which dispatches
// `source.next()` (a Stream<T> class method called through a
// Splittable<T> interface fat pointer) via the class-ancestor
// fall-through into hash-vtable lookup.
TEST(ParallelStreamP1Tests, parallelReduceParallelDriverDirectCall) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        return ParallelDriver.reduceParallel<int32>(\n"
        "            xs.stream(), 0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 15);
}

// Large source: 100 elements crosses MIN_PER_SPLIT*2, so a real
// fork/join driver would split-and-spawn. The driver currently
// walks sequentially (the worker template body still trips a JIT
// codegen loop on the share.next() call site), but the sum must
// still be correct because the sequential-walk fallback covers
// every parallel-reduce shape.
TEST(ParallelStreamP1Tests, parallelReduceLargeSourceCorrectness) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            xs[i] = i + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ParallelDriver.reduceParallel<int32>(\n"
        "            xs.stream(), 0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    // sum(1..100) = 5050
    EXPECT_EQ(runI32Diag(src), 5050);
}

// 1.7d — anyMatch via ParallelDriver direct call. Returns true (3
// satisfies `x > 2`). Same iface-formal dispatch path as
// reduceParallel, with a predicate-shaped lambda.
TEST(ParallelStreamP1Tests, parallelAnyMatchParallelDriverDirectCall) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        boolean b = ParallelDriver.anyMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 2);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

// 1.7e — anyMatch short-circuits with false on an all-negative
// source. Empty-set boundary.
TEST(ParallelStreamP1Tests, parallelAnyMatchFalseWhenNoneMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        boolean b = ParallelDriver.anyMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 99);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

// 1.7f — allMatch returns true only when every element satisfies the
// predicate. The seq-prefix here is all positive.
TEST(ParallelStreamP1Tests, parallelAllMatchTrueWhenAllSatisfy) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [2, 4, 6, 8];\n"
        "        boolean b = ParallelDriver.allMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x % 2 == 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

// 1.7g — allMatch returns false on the first failing element.
TEST(ParallelStreamP1Tests, parallelAllMatchFalseOnOneFailure) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [2, 4, 7, 8];\n"
        "        boolean b = ParallelDriver.allMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x % 2 == 0);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

// 1.7h — noneMatch returns true when no element satisfies the
// predicate. (Equivalent to !anyMatch but tests the explicit driver
// entry point.)
TEST(ParallelStreamP1Tests, parallelNoneMatchTrueWhenNoMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        boolean b = ParallelDriver.noneMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 99);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

// Large-source fork-path tests for the match terminals. 100 elements
// crosses MIN_PER_SPLIT*2, so the driver picks up to MAX_SPLITS
// workers via scope { spawn findHitWorker / findFailWorker }. These
// verify correctness on the spawn path; the cancellation flag is
// implicit (single-carrier scheduler runs workers sequentially today,
// so cancel saves siblings' work once the first worker triggers).

TEST(ParallelStreamP1Tests, parallelAnyMatchLargeSourceFindsMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

TEST(ParallelStreamP1Tests, parallelAnyMatchLargeSourceNoMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

TEST(ParallelStreamP1Tests, parallelAllMatchLargeSourceAllSatisfy) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

TEST(ParallelStreamP1Tests, parallelAllMatchLargeSourceOneFailure) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

TEST(ParallelStreamP1Tests, parallelNoneMatchLargeSourceNoMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

TEST(ParallelStreamP1Tests, parallelNoneMatchLargeSourceFindsMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

// 1.7l — .take(n) on a parallel-flagged stream throws Exception at
// runtime because ordered "first N" is incompatible with
// split-and-spawn parallelism (StreamParallelism.md § Per-terminal
// rules — `take` / `skip` are stateful intermediates and need
// ordered traversal). Remediation: call .sequential() to flip the
// flag back. The cajeta source uses try/catch to convert the
// throw into a sentinel return so the harness sees a normal exit.
TEST(ParallelStreamP1Tests, takeOnParallelStreamRejects) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        try {\n"
        "            return xs.stream().parallel().take(2).count();\n"
        "        } catch (Exception e) {\n"
        "            return -42;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -42);
}

// 1.7m — .skip(n) on a parallel-flagged stream throws too. Same
// reason as take.
TEST(ParallelStreamP1Tests, skipOnParallelStreamRejects) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        try {\n"
        "            return xs.stream().parallel().skip(2).count();\n"
        "        } catch (Exception e) {\n"
        "            return -42;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -42);
}

// 1.7n — .sequential() clears the parallel flag so take/skip
// become legal again. The escape hatch documented in the rejection
// messages.
TEST(ParallelStreamP1Tests, sequentialBeforeTakeClearsFlag) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        return xs.stream().parallel().sequential().take(2).count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// 1.8 — parallel() through a wrapper (filter) propagates the flag.
// Result equals sequential filter + count.
TEST(ParallelStreamP1Tests, parallelThroughFilterStillCounts) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        // count of evens 0..99 == 50\n"
        "        return xs.stream().parallel()\n"
        "                 .filter((x) -> x % 2 == 0)\n"
        "                 .count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}

// Wrapper-chain unwind v1: the driver entry
// ParallelDriver.reduceParallelChain<T> handles a Stream<T> head
// (not Splittable<T>) — walks unwrap() to find the chain root and
// forks via `scope { spawn reduceWorker<T> }` when the head IS the
// root. Stream<T>.reduce does NOT dispatch to it today because the
// JIT module linker doesn't dedup the async Task<T> drop symbol
// across stdlib + user modules (see Stream.cajeta § reduce).
TEST(ParallelStreamP1Tests, reduceParallelChainParallelizesDirectArrayHead) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // 1..100 sums to 5050\n"
        "        return ParallelDriver.reduceParallelChain<int32>(\n"
        "            xs.stream(), 0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 5050);
}

// Fluent filter chain — the headline target. xs.stream().filter(p)
// .parallel().reduce(...) goes through Stream.reduce → driver →
// per-share head.cloneChainOver(piece) → workers fork.
TEST(ParallelStreamP1Tests, parallelReduceDispatchesThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // even-only sum: 2+4+...+100 = 2550\n"
        "        return xs.stream()\n"
        "                 .filter((x) -> x % 2 == 0)\n"
        "                 .parallel()\n"
        "                 .reduce(0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 2550);
}

// Filter-chain parallelism — the headline case. depth=1 (FilterStream
// above ArrayStream root); driver pulls splits from the root and
// rebuilds the FilterStream chain over each via
// `head.cloneChainOver(piece)`. Each worker runs the same filter over
// its slice.
TEST(ParallelStreamP1Tests, reduceParallelChainFilterChainParallelizes) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // even-only sum: 2+4+...+100 = 2550\n"
        "        return ParallelDriver.reduceParallelChain<int32>(\n"
        "            xs.stream().filter((x) -> x % 2 == 0),\n"
        "            0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 2550);
}

// Stateful intermediate above the driver — chain walk hits
// TakeStream's isStatefulWrapper and throws REJECT_STATEFUL.
TEST(ParallelStreamP1Tests, reduceParallelChainRejectsStatefulInChain) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ParallelDriver;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        try {\n"
        "            return ParallelDriver.reduceParallelChain<int32>(\n"
        "                xs.stream().take(5), 0, (a, b) -> a + b);\n"
        "        } catch (Exception e) {\n"
        "            return -77;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -77);
}

// --- Wrapper-aware predicate + forEach terminals --------------------
// Same dispatch shape as reduce: Stream<T>.<terminal> consults
// isParallel; the driver's *ParallelChain<T> entry walks unwrap,
// rejects stateful, rebuilds each share via head.cloneChainOver, and
// forks workers. Tests pin (a) the fluent dispatch through filter
// chains and (b) stateful rejection in those chains.

TEST(ParallelStreamP1Tests, parallelAnyMatchDispatchesThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        boolean hit = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .anyMatch((x) -> x == 50);\n"
        "        return hit ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

TEST(ParallelStreamP1Tests, parallelAnyMatchFalseThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // 200 isn't in the evens 2..100 range\n"
        "        boolean hit = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .anyMatch((x) -> x == 200);\n"
        "        return hit ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 0);
}

TEST(ParallelStreamP1Tests, parallelAllMatchDispatchesThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // every even is > 0\n"
        "        boolean all = xs.stream()\n"
        "                        .filter((x) -> x % 2 == 0)\n"
        "                        .parallel()\n"
        "                        .allMatch((x) -> x > 0);\n"
        "        return all ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

TEST(ParallelStreamP1Tests, parallelNoneMatchDispatchesThroughFilter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        // 7 is odd — filtered out — so noneMatch is true\n"
        "        boolean none = xs.stream()\n"
        "                         .filter((x) -> x % 2 == 0)\n"
        "                         .parallel()\n"
        "                         .noneMatch((x) -> x == 7);\n"
        "        return none ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
}

TEST(ParallelStreamP1Tests, parallelForEachDispatchesThroughFilter) {
    // forEach has no return value; test by accumulating into a counter
    // the lambda bumps. The counter must be an AtomicInt32: workers
    // invoke fn CONCURRENTLY (forEachWorker takes no lock), so a plain
    // `c.v = c.v + 1` is a racy read-modify-write — it passed on x86 by
    // luck and lost increments on aarch64-darwin in the v0.16.0 release
    // run. Order across workers is unspecified (documented v1 contract).
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        AtomicInt32 c = heap AtomicInt32(0);\n"
        "        xs.stream()\n"
        "          .filter((x) -> x % 2 == 0)\n"
        "          .parallel()\n"
        "          .forEach((x) -> { c.fetchAdd(1); });\n"
        "        // 50 evens in 1..100\n"
        "        return c.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 50);
}

// Stateful in chain — each terminal should reject when a take/skip
// sits above .parallel() in the chain.
TEST(ParallelStreamP1Tests, parallelAnyMatchRejectsStatefulInChain) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        try {\n"
        "            boolean hit = xs.stream().take(5).parallel()\n"
        "                            .anyMatch((x) -> x > 2);\n"
        "            return hit ? 1 : 0;\n"
        "        } catch (Exception e) {\n"
        "            return -77;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -77);
}

// --- 3-arg fold(seed, fn, combiner) parallelization -----------------
// The 3-arg overload pairs the per-share accumulator `fn: (R,T)->R`
// with a `combiner: (R,R)->R` that merges worker partials. Both must
// be associative + identity-respecting under `seed` per the Java
// reduce contract (StreamParallelism.md § Per-terminal rules).
//
// Tests pin: (a) the new overload resolves & runs sequentially, (b)
// dispatch through .parallel() on a direct-array head, (c) dispatch
// through a filter chain, (d) stateful-reject in the chain.

// Sequential 3-arg fold: cross-type int32 stream → int64 accumulator,
// combiner unused on this path. Pins that the overload resolves at
// parse time and the sequential body runs.
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

// Parallel 3-arg fold on a direct-array head. 100 elements crosses
// MIN_PER_SPLIT*2 so the driver picks up to MAX_SPLITS workers; the
// combiner merges per-worker int64 partials.
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

// Parallel 3-arg fold dispatched through a filter chain — the
// headline payoff for combiner. depth=1 (FilterStream above the
// ArrayStream root); driver pulls splits from the root and rebuilds
// the FilterStream chain over each via head.cloneChainOver(piece).
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

// --- findFirst under .parallel() — semantics shift to findAny -------
// Per StreamParallelism.md § R1 + § Per-terminal rules, findFirst on
// a parallel stream returns AN arbitrary matching element (findAny),
// not necessarily the first in encounter order. Order across workers
// is unspecified; the test checks the returned element is IN the
// expected match set, not that it's the lowest such.

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

TEST(ParallelStreamP1Tests, parallelFindFirstEmptyOnNoMatch) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Optional<int32> o = xs.stream().parallel()\n"
        "                              .findFirst((x) -> x > 9999);\n"
        "        if (o.isPresent()) { return o.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), -1);
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

// --- 2-arg fold rejected on parallel head ---------------------------
// Per § 2.2, .parallel().fold(seed, fn) (2-arg) is incoherent:
// no combiner means no way to merge partials. The 2-arg overload
// throws at the call site; users should switch to fold(seed, fn,
// combiner) or call .sequential() first. Mirrors take/skip rejects.
TEST(ParallelStreamP1Tests, twoArgFoldOnParallelStreamRejects) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3, 4, 5];\n"
        "        try {\n"
        "            return xs.stream().parallel().fold(\n"
        "                0, (int32 a, int32 x) -> a + x);\n"
        "        } catch (Exception e) {\n"
        "            return -42;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -42);
}

// --- parallel collect via Collector.supplier ------------------------
// `Collector<T, R>` now carries `supplier: () -> R` (replacing the
// pre-parallel `seed` field). `.parallel().collect(...)` routes
// through `ParallelDriver.collectParallelChain<T, R>` which calls
// `supplier()` per worker partial — fresh R instance per worker,
// no aliasing on mutable accumulators like ArrayList. The driver
// orchestrator combines partials via `c.combiner`.
TEST(ParallelStreamP1Tests, parallelCollectViaSupplierAggregatesAll) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Collector;\n"
        "import cajeta.collection.Collectors;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = xs.stream().parallel().collect(c);\n"
        "        return out.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 100);
}

// Verify every element survived the parallel collect: sum-of-elements
// catches losses that a count check can't (e.g. a worker's partial
// silently dropped during the combiner walk).
TEST(ParallelStreamP1Tests, parallelCollectViaSupplierPreservesElements) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Collector;\n"
        "import cajeta.collection.Collectors;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 5050);
}

// Parallel collect through a filter chain — the wrapper-chain unwind
// rebuilds the filter per worker (cloneChainOver) so each worker's
// share also filters before accumulating. The supplier-fresh-per-
// worker discipline still holds; the combiner just merges fewer.
TEST(ParallelStreamP1Tests, parallelCollectViaSupplierThroughFilter) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Collector;\n"
        "import cajeta.collection.Collectors;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 2550);
}

// .sequential() before .collect() — even now that parallel collect
// works, .sequential() should still flip the flag so collect takes
// the per-element walk path instead of the driver dispatch.
TEST(ParallelStreamP1Tests, sequentialBeforeCollectClearsParallelFlag) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Collector;\n"
        "import cajeta.collection.Collectors;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = heap int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i + 1; }\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = xs.stream().parallel().sequential().collect(c);\n"
        "        return out.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 100);
}

TEST(ParallelStreamP1Tests, parallelFindFirstRejectsStatefulInChain) {
    auto src =
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
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
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -77);
}

// Stateful intermediate above .parallel() — chain walk in
// foldParallelChain hits TakeStream.isStatefulWrapper and throws
// REJECT_STATEFUL. Mirrors the other terminals' stateful-reject test.
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
