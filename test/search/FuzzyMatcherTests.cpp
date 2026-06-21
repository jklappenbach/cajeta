// TDD for cajeta.search.fuzzy.Matcher (cajeta.search spec §4, plan U3).
//
// Compiles small cajeta programs that exercise the matcher through the JIT and
// assert on an int32 (the stdlib pattern — see NgramIndexTests/DistanceTests).
//   ./setup.sh && ./cajeta_tests.sh FuzzyMatcherTests

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

// uc 4.2.1 — match("Fiel") ranks "File" first at distance 1; a key sharing a
// gram but past threshold ("Fibonacci") is dropped, and an unrelated key
// ("Network", no shared gram) never even prefilters.
TEST(FuzzyMatcherTests, ranksFileFirstDropsFarAndUnrelated) {
    auto src =
        "package test;\n"
        "import cajeta.search.fuzzy.Matcher;\n"
        "import cajeta.search.fuzzy.Match;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matcher<int32> m = heap Matcher<int32>();\n"
        "        m.add(\"File\", 10);\n"
        "        m.add(\"Files\", 11);\n"
        "        m.add(\"Fibonacci\", 55);\n"   // shares "fi", but far → dropped
        "        m.add(\"Network\", 99);\n"     // no shared gram → not a candidate
        "        ArrayList<Match<int32>> r = m.match(\"Fiel\");\n"
        "        if (r.count() < 1) { return -1; }\n"
        "        Match<int32> top = r.get(0);\n"
        "        return top.value() * 100 + top.distance() * 10 + r.count();\n"  // 10,1,2 -> 1012
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1012);
}

// uc 4.2.3 — an exact match (distance 0) outranks a fuzzy one.
TEST(FuzzyMatcherTests, exactOutranksFuzzy) {
    auto src =
        "package test;\n"
        "import cajeta.search.fuzzy.Matcher;\n"
        "import cajeta.search.fuzzy.Match;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matcher<int32> m = heap Matcher<int32>();\n"
        "        m.add(\"Filet\", 20);\n"
        "        m.add(\"File\", 10);\n"
        "        ArrayList<Match<int32>> r = m.match(\"File\");\n"
        "        if (r.count() < 1) { return -1; }\n"
        "        Match<int32> top = r.get(0);\n"
        "        return top.value() * 10 + top.distance();\n"  // File exact -> 10,0 -> 100
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

// uc 4.2.2 — deterministic: identical queries produce identical ranked output.
TEST(FuzzyMatcherTests, deterministicRanking) {
    auto src =
        "package test;\n"
        "import cajeta.search.fuzzy.Matcher;\n"
        "import cajeta.search.fuzzy.Match;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matcher<int32> m = heap Matcher<int32>();\n"
        "        m.add(\"File\", 10);\n"
        "        m.add(\"Files\", 11);\n"
        "        ArrayList<Match<int32>> a = m.match(\"Fiel\");\n"
        "        ArrayList<Match<int32>> b = m.match(\"Fiel\");\n"
        "        if (a.count() != b.count()) { return -1; }\n"
        "        int32 i = 0;\n"
        "        while (i < a.count()) {\n"
        "            if (a.get(i).value() != b.get(i).value()) { return -2; }\n"
        "            if (a.get(i).distance() != b.get(i).distance()) { return -3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return a.count();\n"  // 2
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}
