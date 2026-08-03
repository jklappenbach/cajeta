// Smoke tests for cajeta.collection.HashSet — thin wrapper around
// cajeta.collection.HashMap<T, int8> (1-byte presence sentinel as
// value side). Works with both class T (identity-equality) and
// primitive T (via the compiler's primitive-`.hash()` intrinsic
// that lowers to __cajeta_hash_X runtime helpers).

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
        "        HashSet<Tag> s = heap HashSet<Tag>(16);\n"
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
        "        HashSet<Tag> s = heap HashSet<Tag>(16);\n"
        "        Tag t = heap Tag(7);\n"
        "        s.add(#t);\n"
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
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<String> s = heap HashSet<String>(16);\n"
        "        String t = \"tag\";\n"
        "        s.add(#t);\n"
        "        s.remove(\"tag\");\n"
        "        if (s.contains(\"tag\")) { return 1; } else { return 0; }\n"
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
        "        HashSet<Tag> s = heap HashSet<Tag>(16);\n"
        "        s.add(heap Tag(1));\n"
        "        s.add(heap Tag(2));\n"
        "        s.add(heap Tag(3));\n"
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
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<String> s = heap HashSet<String>(16);\n"
        "        String a = \"tag\";\n"
        "        String b = \"tag\";\n"
        "        s.add(#a);\n"
        "        s.add(#b);\n"
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
        "        HashSet<Tag> s = heap HashSet<Tag>(16);\n"
        "        Tag t = heap Tag(7);\n"
        "        if (s.remove(t)) { return 99; } else { return 0; }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// --- Primitive T (now works via primitive .hash() intrinsic) -------------

TEST(HashSetTests, primitiveTypeWorks) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<int32> s = heap HashSet<int32>(16);\n"
        "        s.add(7);\n"
        "        s.add(11);\n"
        "        s.add(13);\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

TEST(HashSetTests, primitiveAddDuplicateCountsOnce) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<int32> s = heap HashSet<int32>(16);\n"
        "        s.add(42);\n"
        "        s.add(42);\n"
        "        s.add(42);\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(HashSetTests, primitiveContainsAndRemove) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<int32> s = heap HashSet<int32>(16);\n"
        "        s.add(10);\n"
        "        s.add(20);\n"
        "        s.add(30);\n"
        "        if (!s.contains(20)) { return -1; }\n"
        "        s.remove(20);\n"
        "        if (s.contains(20)) { return -2; }\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}
