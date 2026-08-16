// Smoke tests for cajeta.collection.LinkedList. Unlike HashMap,
// LinkedList accepts any T (primitive or class) because it only
// needs `==` for find/remove — no `hash()` requirement.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Class T -------------------------------------------------------------

TEST(LinkedListTests, addAndCountClassT) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Tag> ll = heap LinkedList<Tag>();\n"
        "        ll.add(heap Tag(1));\n"
        "        ll.add(heap Tag(2));\n"
        "        ll.add(heap Tag(3));\n"
        "        return (int32) ll.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(LinkedListTests, getReturnsValueAtIndex) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Tag> ll = heap LinkedList<Tag>();\n"
        "        ll.add(heap Tag(10));\n"
        "        ll.add(heap Tag(20));\n"
        "        ll.add(heap Tag(30));\n"
        "        Tag t = ll.get(1);\n"
        "        return t.id;\n"  // 20
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

TEST(LinkedListTests, addFirstPrepends) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Tag> ll = heap LinkedList<Tag>();\n"
        "        ll.add(heap Tag(2));\n"
        "        ll.addFirst(heap Tag(1));\n"  // [1, 2]
        "        Tag t = ll.get(0);\n"
        "        return t.id;\n"  // 1
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(LinkedListTests, removeUnlinksAndDecrementsCount) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Tag> ll = heap LinkedList<Tag>();\n"
        "        Tag a = heap Tag(1);\n"
        "        Tag b = heap Tag(2);\n"
        "        Tag c = heap Tag(3);\n"
        "        ll.add(#a);\n"
        "        ll.add(#b);\n"
        "        ll.add(#c);\n"
        "        ll.remove(b);\n"
        "        return (int32) ll.count();\n"  // 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(LinkedListTests, containsFindsAddedValue) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Tag> ll = heap LinkedList<Tag>();\n"
        "        Tag a = heap Tag(1);\n"
        "        ll.add(#a);\n"
        "        if (ll.contains(a)) { return 1; } else { return 0; }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(LinkedListTests, removeAbsentReturnsFalse) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Tag> ll = heap LinkedList<Tag>();\n"
        "        Tag a = heap Tag(1);\n"
        "        if (ll.remove(a)) { return 99; } else { return 0; }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// --- Primitive T (this is the key win — HashMap can't do this) ---------

TEST(LinkedListTests, primitiveTypeWorks) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.add(10);\n"
        "        ll.add(20);\n"
        "        ll.add(30);\n"
        "        return ll.get(2);\n"  // 30
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}

TEST(LinkedListTests, primitiveContainsAndRemove) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.add(10);\n"
        "        ll.add(20);\n"
        "        ll.add(30);\n"
        "        if (!ll.contains(20)) { return -1; }\n"
        "        ll.remove(20);\n"
        "        if (ll.contains(20)) { return -2; }\n"
        "        return (int32) ll.count();\n"  // 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// --- Edge cases -----------------------------------------------------------

TEST(LinkedListTests, emptyListCountIsZero) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        return (int32) ll.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(LinkedListTests, getOutOfRangeReturnsZero) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.add(10);\n"
        "        return ll.get(5);\n"  // 0 — miss default
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(LinkedListTests, removeFromMiddleRelinksProperly) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.add(1);\n"
        "        ll.add(2);\n"
        "        ll.add(3);\n"
        "        ll.add(4);\n"
        "        ll.add(5);\n"
        "        ll.remove(3);\n"  // remove middle
        "        return ll.get(2);\n"  // should be 4 now
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// --- Deque API: head/tail peek, addHead/addTail, popHead/popTail ---------






