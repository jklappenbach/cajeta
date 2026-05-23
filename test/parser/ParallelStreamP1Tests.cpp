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

// 1.7 — parallel() through a wrapper (filter) propagates the flag.
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
