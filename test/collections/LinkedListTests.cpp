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
        "        ll.add(a);\n"
        "        ll.add(b);\n"
        "        ll.add(c);\n"
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
        "        ll.add(a);\n"
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

TEST(LinkedListTests, headAndTailPeek) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.addTail(10);\n"
        "        ll.addTail(20);\n"
        "        ll.addTail(30);\n"
        "        if (ll.head() != 10) { return -1; }\n"
        "        if (ll.tail() != 30) { return -2; }\n"
        "        return (int32) ll.count();\n"  // peek doesn't shrink -> 3
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(LinkedListTests, addHeadAndAddTailOrder) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.addTail(2);\n"
        "        ll.addHead(1);\n"   // [1, 2]
        "        ll.addTail(3);\n"   // [1, 2, 3]
        "        if (ll.head() != 1) { return -1; }\n"
        "        if (ll.tail() != 3) { return -2; }\n"
        "        return ll.get(1);\n"  // 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(LinkedListTests, popHeadRemovesAndReturnsFront) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.addTail(10);\n"
        "        ll.addTail(20);\n"
        "        ll.addTail(30);\n"
        "        int32 f = ll.popHead();\n"  // 10 -> [20, 30]
        "        if (f != 10) { return -1; }\n"
        "        if (ll.head() != 20) { return -2; }\n"
        "        return (int32) ll.count();\n"  // 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(LinkedListTests, popTailRemovesAndReturnsBack) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.addTail(10);\n"
        "        ll.addTail(20);\n"
        "        ll.addTail(30);\n"
        "        int32 b = ll.popTail();\n"  // 30 -> [10, 20]
        "        if (b != 30) { return -1; }\n"
        "        if (ll.tail() != 20) { return -2; }\n"
        "        return (int32) ll.count();\n"  // 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

TEST(LinkedListTests, popDrainsToEmptyThenZero) {
    // Pop the last element from both ends, then pop/peek an empty list.
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.addHead(7);\n"
        "        int32 a = ll.popTail();\n"   // 7 -> []
        "        if (a != 7) { return -1; }\n"
        "        if (ll.count() != 0) { return -2; }\n"
        "        if (ll.head() != 0) { return -3; }\n"  // empty -> zero
        "        if (ll.popHead() != 0) { return -4; }\n"  // empty -> zero, no change
        "        return (int32) ll.count();\n"  // still 0
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

TEST(LinkedListTests, dequeRoundTripUsesBothEnds) {
    // Use it as a deque: push both ends, pop both ends, verify order.
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<int32> ll = heap LinkedList<int32>();\n"
        "        ll.addTail(2);\n"
        "        ll.addHead(1);\n"
        "        ll.addTail(3);\n"   // [1, 2, 3]
        "        if (ll.popHead() != 1) { return -1; }\n"  // [2, 3]
        "        if (ll.popTail() != 3) { return -2; }\n"  // [2]
        "        if (ll.popHead() != 2) { return -3; }\n"  // []
        "        return (int32) ll.count();\n"  // 0
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
