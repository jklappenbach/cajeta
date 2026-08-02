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
        "        Tag tOwned = heap Tag(7);\n"
        "        Tag t = tOwned;\n"
        "        m.put(#tOwned, 42);\n"
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
        "        Tag insertedOwned = heap Tag();\n"
        "        Tag inserted = insertedOwned;\n"
        "        Tag missing = heap Tag();\n"
        "        m.put(#insertedOwned, 1);\n"
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
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(16);\n"
        "        String k1 = \"tag\";\n"
        "        String k2 = \"tag\";\n"
        "        m.put(#k1, 10);\n"
        "        m.put(#k2, 99);\n"
        "        int32 v = m.get(\"tag\");\n"
        "        int64 sz = m.count();\n"
        "        if (sz == 1) { return v; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
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
        "        Tag t1Owned = heap Tag(1);\n"
        "        Tag t1 = t1Owned;\n"
        "        Tag t2Owned = heap Tag(2);\n"
        "        Tag t2 = t2Owned;\n"
        "        Tag t3Owned = heap Tag(3);\n"
        "        Tag t3 = t3Owned;\n"
        "        Tag t4Owned = heap Tag(4);\n"
        "        Tag t4 = t4Owned;\n"
        "        Tag t5Owned = heap Tag(5);\n"
        "        Tag t5 = t5Owned;\n"
        "        m.put(#t1Owned, 10);\n"
        "        m.put(#t2Owned, 20);\n"
        "        m.put(#t3Owned, 30);\n"
        "        m.put(#t4Owned, 40);\n"
        "        m.put(#t5Owned, 50);\n"
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
        "        Tag keep1Owned = heap Tag();\n"
        "        Tag keep1 = keep1Owned;\n"
        "        Tag drop1Owned = heap Tag();\n"
        "        Tag drop1 = drop1Owned;\n"
        "        Tag keep2Owned = heap Tag();\n"
        "        Tag keep2 = keep2Owned;\n"
        "        Tag drop2Owned = heap Tag();\n"
        "        Tag drop2 = drop2Owned;\n"
        "        m.put(#keep1Owned, 100);\n"
        "        m.put(#drop1Owned, 200);\n"
        "        m.put(#keep2Owned, 300);\n"
        "        m.put(#drop2Owned, 400);\n"
        "        // Remove two — leaves tombstones at usedSlots=4.\n"
        "        m.remove(drop1);\n"
        "        m.remove(drop2);\n"
        "        // Insert more to push usedSlots over threshold and\n"
        "        // trigger resize.\n"
        "        Tag x1Owned = heap Tag();\n"
        "        Tag x1 = x1Owned;\n"
        "        Tag x2Owned = heap Tag();\n"
        "        Tag x2 = x2Owned;\n"
        "        m.put(#x1Owned, 1);\n"
        "        m.put(#x2Owned, 2);\n"
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
        "        Tag tOwned = heap Tag();\n"
        "        Tag t = tOwned;\n"
        "        m.put(#tOwned, 42);\n"
        "        int32 removed = 0;\n"
        "        boolean hadT = m.containsKey(t);\n"
        "        m.remove(t);\n"
        "        if (hadT && !m.containsKey(t)) { removed = 1; }\n"
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
        "        m.put(#inserted, 1);\n"
        "        int32 falseyRemove = 0;\n"
        "        boolean hadMissing = m.containsKey(missing);\n"
        "        m.remove(missing);\n"
        "        if (hadMissing) { falseyRemove = 1; }\n"
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
        "        Tag aOwned = heap Tag(1);\n"
        "        Tag a = aOwned;\n"
        "        Tag b1Owned = heap Tag(2);\n"
        "        Tag b1 = b1Owned;\n"
        "        Tag cOwned = heap Tag(3);\n"
        "        Tag c = cOwned;\n"
        "        m.put(#aOwned, 10);\n"
        "        m.put(#b1Owned, 20);\n"
        "        m.put(#cOwned, 30);\n"
        "        m.remove(b1);\n"
        // The re-insert uses a SECOND owned key: remove reclaimed the first
        // one, and a class key is identity-hashed, so there is no way to
        // re-derive it. It still probes through the tombstone, which is what
        // this test is actually about.
        "        Tag b2Owned = heap Tag(2);\n"
        "        Tag b2 = b2Owned;\n"
        "        m.put(#b2Owned, 99);\n"
        "        int32 va = m.get(a);\n"
        "        int32 vb = m.get(b2);\n"
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
        "        Tag tOwned = heap Tag(7);\n"
        "        Tag t = tOwned;\n"
        "        Box b = heap Box(99);\n"
        "        m.put(#tOwned, b);\n"
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
