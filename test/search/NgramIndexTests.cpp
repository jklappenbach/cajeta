// TDD for cajeta.search.ngram.Index (cajeta.search spec §2, plan U1).
//
// These compile a small cajeta program that imports the stdlib package and
// exercises it through the JIT, asserting on an int32 return — the standard
// stdlib test pattern (see test/collections/NewCollectionsTests.cpp).
//
// RUN-DEFERRED: requires the full toolchain (LLVM-23 fork compiler + JIT +
// antlr-generated parser + runtime), which is not buildable in the dev
// environment these were authored in. Run with the toolchain via:
//   ./setup.sh && ./cajeta_tests.sh NgramIndexTests

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

// uc 2.2.1 — recall: a one-typo query surfaces the right value, and a key
// sharing no gram is excluded ("aple" shares "ple" with "apple"; "zebra"
// shares nothing).
TEST(NgramIndexTests, recallOnTypoExcludesUnrelated) {
    auto src =
        "package test;\n"
        "import cajeta.search.ngram.Index;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Index<int32> idx = heap Index<int32>();\n"
        "        idx.add(\"apple\", 1);\n"
        "        idx.add(\"zebra\", 2);\n"
        "        ArrayList<int32> c = idx.candidates(\"aple\");\n"
        "        int32 sum = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < c.count()) { sum = sum + c.get(i); i = i + 1; }\n"
        "        return sum * 100 + c.count();\n"  // value 1, count 1 -> 101
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 101);
}

// uc 2.2.2 — dedup: a value whose key matches the query on many grams is
// returned once.
TEST(NgramIndexTests, dedupValue) {
    auto src =
        "package test;\n"
        "import cajeta.search.ngram.Index;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Index<int32> idx = heap Index<int32>();\n"
        "        idx.add(\"banana\", 7);\n"
        "        ArrayList<int32> c = idx.candidates(\"banana\");\n"
        "        return c.count();\n"  // 1, despite many shared grams
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// uc 2.2.3 — no shared gram → empty.
TEST(NgramIndexTests, noSharedGramIsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.search.ngram.Index;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Index<int32> idx = heap Index<int32>();\n"
        "        idx.add(\"apple\", 1);\n"
        "        ArrayList<int32> c = idx.candidates(\"xyzqqq\");\n"
        "        return c.count();\n"  // 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// uc 2.2.4 — a key shorter than n is indexed under itself and is findable.
TEST(NgramIndexTests, shortKeyFindable) {
    auto src =
        "package test;\n"
        "import cajeta.search.ngram.Index;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Index<int32> idx = heap Index<int32>();\n"
        "        idx.add(\"io\", 9);\n"          // len 2 < 3 → single gram "io"
        "        ArrayList<int32> c = idx.candidates(\"io\");\n"
        "        if (c.count() == 1) { return c.get(0); }\n"  // 9
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9);
}

// uc 2.2.5 — custom gram size (bigrams).
TEST(NgramIndexTests, customGramSize) {
    auto src =
        "package test;\n"
        "import cajeta.search.ngram.Index;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Index<int32> idx = heap Index<int32>(2);\n"  // bigrams
        "        idx.add(\"abc\", 5);\n"                        // bigrams: ab, bc
        "        ArrayList<int32> c = idx.candidates(\"xbc\");\n" // bigrams: xb, bc → shares bc
        "        if (c.count() == 1) { return c.get(0); }\n"   // 5
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}
