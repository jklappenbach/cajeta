// Smoke tests for the new cajeta.collection types: ImmutableList,
// ImmutableSet, ImmutableMap, Heap, RedBlackTree, BPlusTree. Each test
// compiles a small cajeta program that imports and exercises the type
// through the JIT, validating both that the .cajeta source compiles
// and that the basic operations behave.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

TEST(NewCollectionsTests, immutableListGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.collection.ImmutableList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> a = heap ArrayList<int32>();\n"
        "        a.add(10);\n"
        "        a.add(20);\n"
        "        a.add(30);\n"
        "        ImmutableList<int32> il = heap ImmutableList<int32>(a);\n"
        "        return il.get(1) + (int32) il.count();\n"  // 20 + 3
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 23);
}

TEST(NewCollectionsTests, immutableSetDedupContains) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.collection.ImmutableSet;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> a = heap ArrayList<int32>();\n"
        "        a.add(1);\n"
        "        a.add(2);\n"
        "        a.add(2);\n"
        "        a.add(3);\n"
        "        ImmutableSet<int32> s = heap ImmutableSet<int32>(a);\n"
        "        if (s.contains(2)) { return (int32) s.count(); }\n"  // deduped -> 3
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

TEST(NewCollectionsTests, immutableMapGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.collection.ImmutableMap;\n"
        "import cajeta.lang.Pair;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Pair<int32, int32>> a = heap ArrayList<Pair<int32, int32>>();\n"
        "        a.add(heap Pair<int32, int32>(1, 100));\n"
        "        a.add(heap Pair<int32, int32>(2, 200));\n"
        "        ImmutableMap<int32, int32> m = heap ImmutableMap<int32, int32>(a);\n"
        "        return m.get(2);\n"  // 200
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 200);
}

TEST(NewCollectionsTests, heapPopsMinimum) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Heap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Heap<int32> h = heap Heap<int32>();\n"
        "        h.push(5);\n"
        "        h.push(1);\n"
        "        h.push(9);\n"
        "        h.push(3);\n"
        "        int32 a = h.pop();\n"  // 1
        "        int32 b = h.pop();\n"  // 3
        "        return a * 10 + b;\n"  // 13
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 13);
}

TEST(NewCollectionsTests, redBlackManyInsertsLookup) {
    auto src =
        "package test;\n"
        "import cajeta.collection.RedBlackTree;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RedBlackTree<int32, int32> t = heap RedBlackTree<int32, int32>();\n"
        "        int32 i = 0;\n"
        "        while (i < 100) {\n"
        "            t.put(i, i * 2);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return t.get(50) + t.min() + (int32) t.count();\n"  // 100 + 0 + 100
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 200);
}

TEST(NewCollectionsTests, bplusManyInsertsForceSplitLookup) {
    auto src =
        "package test;\n"
        "import cajeta.collection.BPlusTree;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        BPlusTree<int32, int32> t = heap BPlusTree<int32, int32>();\n"
        "        int32 i = 0;\n"
        "        while (i < 200) {\n"  // > order(32): forces leaf + internal splits
        "            t.put(i, i * 3);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return t.get(150) + t.min() + (int32) t.count();\n"  // 450 + 0 + 200
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 650);
}
