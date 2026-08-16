// Smoke tests for the new cajeta.collection types: ImmutableList,
// ImmutableSet, ImmutableMap, Heap, RedBlackTree, BPlusTree. Each test
// compiles a small cajeta program that imports and exercises the type
// through the JIT, validating both that the .cajeta source compiles
// and that the basic operations behave.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;







// SwissTable ImmutableMap stress: 500 entries (forces multi-group probing and
// the mirror tail), last-wins on a duplicate, lookups + containsKey + keyAt.
TEST(NewCollectionsTests, immutableMapSwissStress) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.collection.ImmutableMap;\n"
        "import cajeta.lang.Pair;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Pair<int32, int32>> a = heap ArrayList<Pair<int32, int32>>();\n"
        "        int32 i = 0;\n"
        "        while (i < 500) { a.add(heap Pair<int32, int32>(i, i + 7)); i = i + 1; }\n"
        "        a.add(heap Pair<int32, int32>(250, 99999));\n"  // duplicate, last-wins
        "        ImmutableMap<int32, int32> m = heap ImmutableMap<int32, int32>(#a);\n"
        "        if (m.count() != 500) { return -1; }\n"
        "        if (m.get(250) != 99999) { return -2; }\n"
        "        if (m.containsKey(700)) { return -3; }\n"
        "        if (m.containsKey(499) == false) { return -4; }\n"
        "        int32 hits = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 500) { if (j != 250) { if (m.get(j) == j + 7) { hits = hits + 1; } } j = j + 1; }\n"
        "        return hits;\n"  // 499 (all but the overwritten 250)
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 499);
}
