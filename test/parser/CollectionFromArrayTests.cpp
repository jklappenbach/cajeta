//
// array-literals Unit 5: from-array constructors on the sequence collections,
// so an array literal initializes a collection —
// `heap ArrayList<int32>([1,2,3])` (spec §6). The `[...]` lowers to a `T[]`
// (array-literals §2) whose unified element type matches the ctor's `T[]`
// parameter; the ctor copies the elements in.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n" + imports +
        "public final class D {\n" + body + "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
} // namespace

// 5.1.1 — ArrayList from a literal: size 3, order preserved.
TEST(CollectionFromArrayTests, ArrayListFromLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>([1, 2, 3]);\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 1*10 + 3 = 313
    EXPECT_EQ(r, 313);
}

// 5.1.2 — HashSet from a literal: duplicates collapse to {1,2,3}.
TEST(CollectionFromArrayTests, HashSetFromLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.HashSet;\n",
        "    public static int32 run() {\n"
        "        HashSet<int32> s = heap HashSet<int32>([1, 2, 2, 3]);\n"
        "        int32 c = (int32) s.count();\n"          // 3 (deduped)
        "        int32 has = 0;\n"
        "        if (s.contains(1)) { has = has + 1; }\n"
        "        if (s.contains(2)) { has = has + 1; }\n"
        "        if (s.contains(3)) { has = has + 1; }\n"
        "        if (s.contains(9)) { has = has + 10; }\n"  // absent -> unchanged
        "        return c * 10 + has;\n"                    // 3*10 + 3 = 33
        "    }\n");
    EXPECT_EQ(r, 33);
}

// 5.1.3 — LinkedList from a literal: order preserved.
TEST(CollectionFromArrayTests, LinkedListFromLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.LinkedList;\n",
        "    public static int32 run() {\n"
        "        LinkedList<int32> xs = heap LinkedList<int32>([10, 20, 30]);\n"
        "        return xs.get(0) * 100 + xs.get(1) * 10 + xs.get(2);\n"
        "    }\n");                                        // 10*100 + 20*10 + 30 = 1230
    EXPECT_EQ(r, 1230);
}

// 5.1.4a — ImmutableList from a literal: right-sized, ordered.
TEST(CollectionFromArrayTests, ImmutableListFromLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ImmutableList;\n",
        "    public static int32 run() {\n"
        "        ImmutableList<int32> xs = heap ImmutableList<int32>([7, 8, 9]);\n"
        "        return (int32) xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                        // 3*100 + 7*10 + 9 = 379
    EXPECT_EQ(r, 379);
}

// 5.1.4b — ImmutableSet from a literal: frozen + deduped.
TEST(CollectionFromArrayTests, ImmutableSetFromLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ImmutableSet;\n",
        "    public static int32 run() {\n"
        "        ImmutableSet<int32> s = heap ImmutableSet<int32>([1, 2, 2, 3]);\n"
        "        int32 c = (int32) s.count();\n"          // 3 (deduped)
        "        int32 has = 0;\n"
        "        if (s.contains(2)) { has = 1; }\n"
        "        if (s.contains(9)) { has = has + 10; }\n"  // absent
        "        return c * 10 + has;\n"                    // 3*10 + 1 = 31
        "    }\n");
    EXPECT_EQ(r, 31);
}
