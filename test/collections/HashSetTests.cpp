// Smoke tests for cajeta.collection.HashSet — thin wrapper around
// cajeta.collection.HashMap<T, T>. Uses the same v1 constraint as
// HashMap: T must be a class type (primitives don't carry hash() or
// override ==).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

TEST(HashSetTests, constructOnly) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Tag> s = new HashSet<Tag>(16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(HashSetTests, addThenContains) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Tag> s = new HashSet<Tag>(16);\n"
        "        Tag t = new Tag(7);\n"
        "        s.add(t);\n"
        "        if (s.contains(t)) { return 1; } else { return 0; }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(HashSetTests, addRemoveThenNotContains) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Tag> s = new HashSet<Tag>(16);\n"
        "        Tag t = new Tag(7);\n"
        "        s.add(t);\n"
        "        s.remove(t);\n"
        "        if (s.contains(t)) { return 1; } else { return 0; }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

TEST(HashSetTests, countMatchesAddCount) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Tag> s = new HashSet<Tag>(16);\n"
        "        s.add(new Tag(1));\n"
        "        s.add(new Tag(2));\n"
        "        s.add(new Tag(3));\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// Set semantics: adding the same VALUE-EQUAL key twice counts once.
// Using identity (the same Tag instance) since v1 hash() defaults to
// pointer identity — value semantics would need @AutoHash on Tag.
TEST(HashSetTests, doubleAddOfSameInstanceCountsOnce) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Tag> s = new HashSet<Tag>(16);\n"
        "        Tag t = new Tag(7);\n"
        "        s.add(t);\n"
        "        s.add(t);\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// remove of an absent key returns false (matches HashMap.remove
// contract — propagated through the wrapper).
TEST(HashSetTests, removeAbsentReturnsFalse) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Tag> s = new HashSet<Tag>(16);\n"
        "        Tag t = new Tag(7);\n"
        "        if (s.remove(t)) { return 99; } else { return 0; }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}
