// Smoke tests for cajeta.collection.HashMap. The class is in stdlib
// (runtime/src/cajeta/collection/HashMap.cajeta) so it's already
// loaded by the time these tests JIT — they instantiate it with
// concrete K, V types and exercise both the method form (put/get/
// containsKey/count) and the bracket form (m[k] / m[k] = v).
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag t = heap Tag(7);\n"
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag t = heap Tag();\n"
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(8);\n"
        "        Tag inserted = heap Tag();\n"
        "        Tag missing = heap Tag();\n"
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

TEST(HashMapTests, countTracksInsertions) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        m.put(heap Tag(), 1);\n"
        "        m.put(heap Tag(), 2);\n"
        "        m.put(heap Tag(), 3);\n"
        "        return m.count();\n"
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag t = heap Tag();\n"
        "        m.put(t, 10);\n"
        "        m.put(t, 99);\n"
        "        int32 v = m.get(t);\n"
        "        int64 sz = m.count();\n"
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        m.put(heap Tag(), 1);\n"
        "        m.put(heap Tag(), 2);\n"
        "        return m.count();\n"
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag t = heap Tag(7);\n"
        "        m[t] = 42;\n"
        "        return m[t];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// ----- auto-resize (load factor > 0.75) ---------------------------------

TEST(HashMapTests, growsBeyondInitialCapacityAndKeepsAllEntries) {
    // Start with cap=4 so we trip the 0.75 load factor at the
    // fourth insert (4 * 4 > 4 * 3 → 16 > 12). The map must
    // double, reinsert, and keep all entries findable with the
    // right values.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(4);\n"
        "        Tag t1 = heap Tag(1);\n"
        "        Tag t2 = heap Tag(2);\n"
        "        Tag t3 = heap Tag(3);\n"
        "        Tag t4 = heap Tag(4);\n"
        "        Tag t5 = heap Tag(5);\n"
        "        m.put(t1, 10);\n"
        "        m.put(t2, 20);\n"
        "        m.put(t3, 30);\n"
        "        m.put(t4, 40);\n"
        "        m.put(t5, 50);\n"
        "        int64 sz = m.count();\n"
        "        int32 v1 = m.get(t1);\n"
        "        int32 v2 = m.get(t2);\n"
        "        int32 v3 = m.get(t3);\n"
        "        int32 v4 = m.get(t4);\n"
        "        int32 v5 = m.get(t5);\n"
        "        if (sz == 5 && v1 == 10 && v2 == 20 && v3 == 30\n"
        "                && v4 == 40 && v5 == 50) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(HashMapTests, resizeClearsTombstones) {
    // Insert N keys, remove some (leaving tombstones), insert more
    // until resize fires. Resize walks oldState looking only at
    // OCCUPIED slots — tombstones are dropped on the floor and
    // the new table starts clean. Verify the live entries (those
    // not removed) survive resize with correct values.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(4);\n"
        "        Tag keep1 = heap Tag();\n"
        "        Tag drop1 = heap Tag();\n"
        "        Tag keep2 = heap Tag();\n"
        "        Tag drop2 = heap Tag();\n"
        "        m.put(keep1, 100);\n"
        "        m.put(drop1, 200);\n"
        "        m.put(keep2, 300);\n"
        "        m.put(drop2, 400);\n"
        "        // Remove two — leaves tombstones at usedSlots=4.\n"
        "        m.remove(drop1);\n"
        "        m.remove(drop2);\n"
        "        // Insert more to push usedSlots over threshold and\n"
        "        // trigger resize.\n"
        "        Tag x1 = heap Tag();\n"
        "        Tag x2 = heap Tag();\n"
        "        m.put(x1, 1);\n"
        "        m.put(x2, 2);\n"
        "        int64 sz = m.count();\n"
        "        int32 vk1 = m.get(keep1);\n"
        "        int32 vk2 = m.get(keep2);\n"
        "        int32 vx1 = m.get(x1);\n"
        "        int32 vx2 = m.get(x2);\n"
        "        int32 d1 = 0;\n"
        "        if (m.containsKey(drop1)) { d1 = 1; }\n"
        "        int32 d2 = 0;\n"
        "        if (m.containsKey(drop2)) { d2 = 1; }\n"
        "        if (sz == 4 && vk1 == 100 && vk2 == 300\n"
        "                && vx1 == 1 && vx2 == 2\n"
        "                && d1 == 0 && d2 == 0) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// ----- remove(K key) ---------------------------------------------------

TEST(HashMapTests, removeReturnsTrueAndShrinksSize) {
    // Put a key, remove it, observe: remove returns true, size
    // drops to 0, containsKey returns false, get returns 0.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag t = heap Tag();\n"
        "        m.put(t, 42);\n"
        "        int32 removed = 0;\n"
        "        if (m.remove(t)) { removed = 1; }\n"
        "        int64 sz = m.count();\n"
        "        int32 stillThere = 0;\n"
        "        if (m.containsKey(t)) { stillThere = 1; }\n"
        "        int32 score = (removed * 100) + (sz == 0 ? 10 : 0) + (stillThere == 0 ? 1 : 0);\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 100 (removed=true) + 10 (size=0) + 1 (containsKey=false) = 111
    EXPECT_EQ(fn(), 111);
}

TEST(HashMapTests, removeReturnsFalseWhenAbsent) {
    // remove on a never-inserted key returns false; size unchanged.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public Tag() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag inserted = heap Tag();\n"
        "        Tag missing = heap Tag();\n"
        "        m.put(inserted, 1);\n"
        "        int32 falseyRemove = 0;\n"
        "        if (m.remove(missing)) { falseyRemove = 1; }\n"
        "        int64 sz = m.count();\n"
        "        if (falseyRemove == 0 && sz == 1) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(HashMapTests, removeThenPutReusesTombstoneSlot) {
    // After remove leaves a tombstone, a subsequent put on a key
    // that probes through that slot should land in the tombstone
    // (compaction). We can't directly observe the slot used, but
    // we can verify the table behaves correctly under remove+put
    // cycles: insert many keys, remove some, re-insert, all
    // remaining keys must still be findable.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag a = heap Tag(1);\n"
        "        Tag b = heap Tag(2);\n"
        "        Tag c = heap Tag(3);\n"
        "        m.put(a, 10);\n"
        "        m.put(b, 20);\n"
        "        m.put(c, 30);\n"
        "        m.remove(b);\n"
        "        m.put(b, 99);\n"
        "        int32 va = m.get(a);\n"
        "        int32 vb = m.get(b);\n"
        "        int32 vc = m.get(c);\n"
        "        int64 sz = m.count();\n"
        "        if (sz == 3 && va == 10 && vb == 99 && vc == 30) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
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
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>(16);\n"
        "        Tag t = heap Tag();\n"
        "        m[t] = 10;\n"
        "        m[t] = 99;\n"
        "        int32 v = m[t];\n"
        "        int64 sz = m.count();\n"
        "        if (sz == 1) { return v; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// Class-typed V: HashMap<Tag, Box>. The miss path of get() used to
// return literal `0` which lowers to `i64 0`, mismatching the
// function's `ptr` return type when V is a class — JIT verifier
// rejected the module at compile time. After the fix (return null
// in the miss path), class-typed V compiles and works end-to-end.
TEST(HashMapTests, classTypedValueWorks) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Tag {\n"
        "    public int32 id;\n"
        "    public Tag(int32 i) { this.id = i; }\n"
        "}\n"
        "public class Box {\n"
        "    public int32 payload;\n"
        "    public Box(int32 p) { this.payload = p; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, Box> m = heap HashMap<Tag, Box>(16);\n"
        "        Tag t = heap Tag(7);\n"
        "        Box b = heap Box(99);\n"
        "        m.put(t, b);\n"
        "        Box got = m.get(t);\n"
        "        return got.payload;\n"  // 99
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// String keys, 1000 distinct entries: exercises XXH3 string hashing and
// the SwissTable group probe / resize / wraparound under many collisions.
// Build with a small initial capacity (16) so the table resizes repeatedly.
TEST(HashMapTests, stringKeysThousandRoundTrip) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(16);\n"
        "        int32 i = 0;\n"
        "        while (i < 1000) { m.put(\"k\" + i, i); i = i + 1; }\n"
        "        int32 hits = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 1000) {\n"
        "            if (m.get(\"k\" + j) == j) { hits = hits + 1; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        if (m.count() != 1000) { return -1; }\n"
        "        return hits;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1000);
}
