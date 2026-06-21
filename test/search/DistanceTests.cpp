// TDD for cajeta.search.distance (cajeta.search spec §3, plan U2).
//
// Each test compiles a small cajeta program that imports the package, calls
// the static distance functions, and returns an int32 the C++ side asserts on
// (the stdlib JIT pattern — see test/collections/NewCollectionsTests.cpp and
// test/search/NgramIndexTests.cpp). Requires the full toolchain:
//   ./setup.sh && ./cajeta_tests.sh DistanceTests

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

// uc 3.2.1 — damerau is transposition-aware: "flie"->"file" is one adjacent
// swap (distance 1), while plain levenshtein counts it as 2 substitutions.
TEST(DistanceTests, damerauTranspositionVsLevenshtein) {
    auto src =
        "package test;\n"
        "import cajeta.search.distance.Distance;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 dam = Distance.damerau(\"flie\", \"file\");\n"
        "        int32 lev = Distance.levenshtein(\"flie\", \"file\");\n"
        "        return dam * 10 + lev;\n"  // 1*10 + 2 = 12
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 12);
}

// uc 3.2.2 — the classic levenshtein("kitten","sitting") == 3.
TEST(DistanceTests, levenshteinKittenSitting) {
    auto src =
        "package test;\n"
        "import cajeta.search.distance.Distance;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Distance.levenshtein(\"kitten\", \"sitting\");\n"  // 3
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// uc 3.2.3 — distance to self is 0; distance to empty is the other's length.
TEST(DistanceTests, selfZeroEmptyIsLength) {
    auto src =
        "package test;\n"
        "import cajeta.search.distance.Distance;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 self = Distance.levenshtein(\"abc\", \"abc\");\n"   // 0
        "        int32 toEmpty = Distance.levenshtein(\"abc\", \"\");\n"   // 3
        "        int32 fromEmpty = Distance.levenshtein(\"\", \"xy\");\n"  // 2
        "        int32 damSelf = Distance.damerau(\"abc\", \"abc\");\n"    // 0
        "        return self * 1000 + toEmpty * 100 + fromEmpty * 10 + damSelf;\n"  // 0320
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 320);
}

// U2.3 acceptance — symmetric in its arguments (swapping a and b is the same).
TEST(DistanceTests, symmetric) {
    auto src =
        "package test;\n"
        "import cajeta.search.distance.Distance;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 ab = Distance.damerau(\"saturday\", \"sunday\");\n"
        "        int32 ba = Distance.damerau(\"sunday\", \"saturday\");\n"
        "        if (ab == ba) { return ab; }\n"  // 3
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}
