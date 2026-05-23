// P1 — parallel stream foundation. Tests pin the minimum useful
// surface: `.parallel()` flag-flip, Splittable<T> on ArrayStream,
// driver fork/join for count(), and the no-split fallback path.
//
// See cajeta-docs/stdlib/StreamParallelism.md for design and
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
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
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
        "        int32[] xs = new int32[1000];\n"
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
        "        int32[] xs = {7, 8, 9};\n"
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
        "        int32[] xs = new int32[0];\n"
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
        "        int32[] xs = {1, 2, 3, 4, 5, 6, 7, 8};\n"
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
        "        int32[] xs = {1, 2, 3};\n"
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
        "        int32[] xs = new int32[100];\n"
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
        "        int32[] xs = {10, 20, 30};\n"
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
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
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
        "        int32[] xs = new int32[100];\n"
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
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
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
        "        int32[] xs = {1, 2, 3};\n"
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
        "        int32[] xs = {2, 4, 6, 8};\n"
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
        "        int32[] xs = {2, 4, 7, 8};\n"
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
        "        int32[] xs = {1, 2, 3};\n"
        "        boolean b = ParallelDriver.noneMatchParallel<int32>(\n"
        "            xs.stream(), (x) -> x > 99);\n"
        "        if (b) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Diag(src), 1);
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
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
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
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
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
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
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
        "        int32[] xs = new int32[100];\n"
        "        for (int32 i = 0; i < 100; i = i + 1) { xs[i] = i; }\n"
        "        // count of evens 0..99 == 50\n"
        "        return xs.stream().parallel()\n"
        "                 .filter((x) -> x % 2 == 0)\n"
        "                 .count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}
