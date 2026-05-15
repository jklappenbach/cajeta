// Smoke tests for cajeta.collection.HashMap. The class is in stdlib
// (runtime/src/cajeta/collection/HashMap.cajeta) so it's already
// loaded by the time these tests JIT — they instantiate it with
// concrete K, V types and exercise both the method form (put/get/
// containsKey/size) and the bracket form (m[k] / m[k] = v).
//
// v1 constraint: K must be a class type. Primitives don't carry
// hash() (they're not Objects), so HashMap<int32, V> doesn't
// compile today.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

// Minimal: just instantiate, don't put or get.
TEST(HashMapTests, constructOnly) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag { public Tag() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(HashMapTests, putThenGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        Tag t = new Tag(7);\n"
        "        m.put(t, 42);\n"
        "        return m.get(t);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(HashMapTests, getReturnsZeroForAbsent) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        Tag t = new Tag();\n"
        "        return m.get(t);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

TEST(HashMapTests, containsKeyReportsPresence) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(8);\n"
        "        Tag inserted = new Tag();\n"
        "        Tag missing = new Tag();\n"
        "        m.put(inserted, 1);\n"
        "        int32 yes = 0;\n"
        "        if (m.containsKey(inserted)) { yes = 10; }\n"
        "        int32 no = 0;\n"
        "        if (m.containsKey(missing)) { no = 20; }\n"
        "        return yes + no;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 10);
}

TEST(HashMapTests, sizeTracksInsertions) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        m.put(new Tag(), 1);\n"
        "        m.put(new Tag(), 2);\n"
        "        m.put(new Tag(), 3);\n"
        "        return m.size();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

TEST(HashMapTests, replaceUpdatesExistingValue) {
    // Insert a key, overwrite it, observe the new value. Size stays
    // at 1 because put on an existing key updates instead of adding.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        Tag t = new Tag();\n"
        "        m.put(t, 10);\n"
        "        m.put(t, 99);\n"
        "        int32 v = m.get(t);\n"
        "        int64 sz = m.size();\n"
        "        if (sz == 1) { return v; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

TEST(HashMapTests, distinctInstancesAreDifferentKeys) {
    // Identity-based hash + ==: two distinct Tag instances are
    // different keys even though they're "equivalent." Both inserts
    // succeed; size grows to 2.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        m.put(new Tag(), 1);\n"
        "        m.put(new Tag(), 2);\n"
        "        return m.size();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// ----- Bracket subscript syntax over operator[] / operator[]= -------

TEST(HashMapTests, bracketWriteThenRead) {
    // `m[k] = v` dispatches to operator[]= → put;
    // `m[k]` dispatches to operator[] → get. Same observable behavior
    // as the method form above, just with subscript syntax.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        Tag t = new Tag(7);\n"
        "        m[t] = 42;\n"
        "        return m[t];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(HashMapTests, bracketReplaceUpdatesValue) {
    // `m[t] = 10` then `m[t] = 99` on the same key — second write
    // replaces, size stays 1.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = new HashMap<Tag, int32>(16);\n"
        "        Tag t = new Tag();\n"
        "        m[t] = 10;\n"
        "        m[t] = 99;\n"
        "        int32 v = m[t];\n"
        "        int64 sz = m.size();\n"
        "        if (sz == 1) { return v; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}
