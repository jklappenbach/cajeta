//
// P2.2 (first slice) — Stream terminal combinators: anyMatch, allMatch,
// noneMatch, findFirst, reduce. All take a lambda and walk next() to
// either exhaustion or short-circuit. No wrapper classes — they're
// methods on the base Stream<T> reused by every subclass via inheritance
// (ArrayStream<T> here).
//
// Lambdas follow the named-local pattern that StreamTests.cpp already
// uses for forEach: declare a typed local, then pass it to the
// terminal. Direct in-call lambdas hit a pre-existing capture
// limitation noted in StreamTests' forEach test comment.
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

TEST(StreamTerminalTests, anyMatchFindsMatchingElement) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean isFour = (int32 v) -> { return v == 4; };\n"
        "        if (s.anyMatch(isFour)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, anyMatchReturnsFalseWhenNoneMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean isHundred = (int32 v) -> { return v == 100; };\n"
        "        if (s.anyMatch(isHundred)) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, anyMatchOnEmptyStreamIsFalse) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[0];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean any = (int32 v) -> { return true; };\n"
        "        if (s.anyMatch(any)) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, allMatchAllPass) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {2, 4, 6};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };\n"
        "        if (s.allMatch(isEven)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, allMatchShortCircuitsOnFailure) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {2, 3, 4};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean isEven = (int32 v) -> { return (v % 2) == 0; };\n"
        "        if (s.allMatch(isEven)) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, allMatchOnEmptyStreamIsTrue) {
    // Vacuous truth — Java's Stream.allMatch matches.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[0];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean impossible = (int32 v) -> { return false; };\n"
        "        if (s.allMatch(impossible)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, noneMatchAllFail) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean isNegative = (int32 v) -> { return v < 0; };\n"
        "        if (s.noneMatch(isNegative)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, noneMatchShortCircuitsOnFirstMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean isTwo = (int32 v) -> { return v == 2; };\n"
        "        if (s.noneMatch(isTwo)) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, findFirstReturnsMatchingElement) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean over2 = (int32 v) -> { return v > 2; };\n"
        "        Optional<int32> hit = s.findFirst(over2);\n"
        "        if (hit.isPresent()) { return hit.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(StreamTerminalTests, findFirstReturnsEmptyWhenNoneMatch) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32) -> boolean over99 = (int32 v) -> { return v > 99; };\n"
        "        Optional<int32> hit = s.findFirst(over99);\n"
        "        if (hit.isEmpty()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(StreamTerminalTests, reduceSumsElements) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };\n"
        "        return s.reduce(0, add);\n"  // 0+1+2+3+4+5 = 15
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

TEST(StreamTerminalTests, reduceOnEmptyStreamReturnsSeed) {
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = new int32[0];\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32, int32) -> int32 add = (int32 a, int32 b) -> { return a + b; };\n"
        "        return s.reduce(42, add);\n"  // empty → seed
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(StreamTerminalTests, reduceMultipliesElements) {
    // Different operator to confirm reduce isn't hard-wired to addition
    // somewhere in the codegen path.
    auto src =
        "package test;\n"
        "import cajeta.lang.stream.ArrayStream;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {2, 3, 4};\n"
        "        ArrayStream<int32> s = xs.stream();\n"
        "        (int32, int32) -> int32 mul = (int32 a, int32 b) -> { return a * b; };\n"
        "        return s.reduce(1, mul);\n"  // 1*2*3*4 = 24
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 24);
}
