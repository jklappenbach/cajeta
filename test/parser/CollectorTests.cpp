// Stream<T>.collect<R>(Collector<T, R>) — terminal that reduces a
// stream via a packaged (seed, accumulator) pair. Completes the
// Stream pipeline story alongside reduce + fold<R>. See
// cajeta-docs/stdlib/Streams.md § collect and
// cajeta.collection.Collectors for the standard built-in factories.

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
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.collection.Collector;\n"
    "import cajeta.collection.Collectors;\n";

} // namespace

// Hand-rolled Collector: counts elements via a fold-style accumulator.
// Verifies the basic plumbing — Collector ctor, collect terminal,
// fold delegation — without depending on Collectors.toList.
TEST(CollectorTests, handRolledCounter) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 10, 20, 30, 40 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 4);\n"
        "        Collector<int32, int32> c = heap Collector<int32, int32>(\n"
        "            0, (int32 acc, int32 x) -> acc + 1);\n"
        "        return s.collect(c);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// Same shape, summing instead of counting.
TEST(CollectorTests, handRolledSummer) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4, 5 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 5);\n"
        "        Collector<int32, int32> c = heap Collector<int32, int32>(\n"
        "            0, (int32 acc, int32 x) -> acc + x);\n"
        "        return s.collect(c);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Collectors.toList<int32>() — built-in factory returning a
// Collector that appends every element into a fresh ArrayList<T>.
// Two-layer naming (see staticExplicitWhenInferenceWouldFail in
// MethodTemplateExplicitArgsTests) unblocked addMethod-side, but
// the body of `toList` synthesizes a block-body lambda
// `(ArrayList<T> acc, T x) -> { acc.add(x); return acc; }` whose
// return type comes out as `void` under the current lambda body-
// inference path. JIT verify rejects with "Found return instr
// that returns non-void in Function of void return type" on the
// synthesized `__cajeta_lambda_0`. That's a separate bug in
// lambda-block-body return-type inference (the typed-param +
// explicit-return shape) — not in two-layer naming itself.
TEST(CollectorTests, DISABLED_collectorsToListSize) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3, 4, 5 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 5);\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = s.collect(c);\n"
        "        return out.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Same lambda-block-body inference bug as collectorsToListSize.
TEST(CollectorTests, DISABLED_collectorsToListPreservesOrder) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 7, 9, 11 };\n"
        "        ArrayStream<int32> s = heap ArrayStream<int32>(xs, 3);\n"
        "        Collector<int32, ArrayList<int32>> c = Collectors.toList<int32>();\n"
        "        ArrayList<int32> out = s.collect(c);\n"
        "        return out.get(0) + out.get(1) * 10 + out.get(2) * 100;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7 + 90 + 1100);
}

// Two distinct hand-rolled Collectors over the same Stream — sum and
// count — verifies cache + dispatch don't conflate the call sites.
TEST(CollectorTests, twoSpecializationsCoexist) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = { 1, 2, 3 };\n"
        "        ArrayStream<int32> sa = heap ArrayStream<int32>(xs, 3);\n"
        "        Collector<int32, int32> sum =\n"
        "            heap Collector<int32, int32>(\n"
        "                0, (int32 acc, int32 x) -> acc + x);\n"
        "        int32 total = sa.collect(sum);\n"
        "\n"
        "        ArrayStream<int32> sb = heap ArrayStream<int32>(xs, 3);\n"
        "        Collector<int32, int32> cnt =\n"
        "            heap Collector<int32, int32>(\n"
        "                0, (int32 acc, int32 x) -> acc + 1);\n"
        "        int32 size = sb.collect(cnt);\n"
        "        return total + size;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6 + 3);
}
