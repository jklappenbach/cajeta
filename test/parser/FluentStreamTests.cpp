// Fluent stream chain tests. Stream<T> now exposes filter / take /
// skip / peek / map / flatMap as instance methods returning the
// appropriate wrapper stream type, so users can write
// `xs.stream().filter(p).map(f).count()` directly.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

TEST(FluentStreamTests, filterCount) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6};\n"
        "        return xs.stream().filter((x) -> x > 3).count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);  // 4, 5, 6
}

TEST(FluentStreamTests, takeCount) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6};\n"
        "        return xs.stream().take(4).count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 4);
}

TEST(FluentStreamTests, skipCount) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6};\n"
        "        return xs.stream().skip(2).count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 4);  // 3, 4, 5, 6
}

// skip().take().count() — Java's slice idiom
TEST(FluentStreamTests, skipTakeSlice) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6, 7, 8};\n"
        "        return xs.stream().skip(2).take(3).count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);  // 3, 4, 5
}

// filter().take().count()
TEST(FluentStreamTests, filterTakeCount) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6, 7, 8};\n"
        "        return xs.stream().filter((x) -> x > 2).take(3).count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);  // 3, 4, 5
}

// map().reduce() — element-type-changing pipeline
TEST(FluentStreamTests, mapReduce) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4};\n"
        "        return xs.stream().map<int32>((x) -> x * 10).reduce(0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);  // 10+20+30+40
}

// peek().count() — verify peek's side effect via a captured counter
TEST(FluentStreamTests, peekCountsSideEffect) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "    public void inc() { this.n = this.n + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        Counter c = heap Counter();\n"
        "        xs.stream().peek((x) -> c.inc()).count();\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

// flatMap fluent — verifies the method-level R generic on Stream<T>
// instantiates the FlatMapStream<T, R> wrapper through the chained
// receiver. {1,2,3} -> singleton inner streams -> flattened count = 3.
TEST(FluentStreamTests, flatMapCount) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.stream.Stream;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        (int32) -> Stream<int32> mk = (int32 v) -> {\n"
        "            int32[] one = new int32[1];\n"
        "            one[0] = v * 10;\n"
        "            ArrayStream<int32> inner = heap ArrayStream<int32>(one, 1);\n"
        "            return inner;\n"
        "        };\n"
        "        return xs.stream().flatMap<int32>(mk).count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// Long fluent chain: filter().map().take().reduce()
TEST(FluentStreamTests, fourStageChain) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};\n"
        "        return xs.stream()\n"
        "                 .filter((x) -> x > 3)\n"   // 4..10
        "                 .map<int32>((x) -> x * 2)\n"  // 8,10,12,..20
        "                 .take(3)\n"                // 8,10,12
        "                 .reduce(0, (a, b) -> a + b);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 30);
}
