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


// ----- Bracket subscript syntax over operator[] / operator[]= -------


// ----- auto-resize (load factor > 0.75) ---------------------------------


TEST(HashMapTests, resizeClearsTombstones) {
    // Uses String (VALUE-hashed) keys: `m.remove(k)` frees the key the map
    // owned, so consulting the surrendered handle afterwards would read freed
    // memory (transfer-demotes-to-borrow 1.7). A fresh literal checks absence
    // without a dangling borrow.
    //
    // Insert N keys, remove some (leaving tombstones), insert more
    // until resize fires. Resize walks oldState looking only at
    // OCCUPIED slots — tombstones are dropped on the floor and
    // the new table starts clean. Verify the live entries (those
    // not removed) survive resize with correct values.
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(4);\n"
        "        String keep1 = \"k1\";\n"
        "        String drop1 = \"d1\";\n"
        "        String keep2 = \"k2\";\n"
        "        String drop2 = \"d2\";\n"
        "        m.put(#keep1, 100);\n"
        "        m.put(#drop1, 200);\n"
        "        m.put(#keep2, 300);\n"
        "        m.put(#drop2, 400);\n"
        "        // Remove two — leaves tombstones at usedSlots=4.\n"
        "        m.remove(\"d1\");\n"
        "        m.remove(\"d2\");\n"
        "        // Insert more to push usedSlots over threshold and\n"
        "        // trigger resize.\n"
        "        String x1 = \"x1\";\n"
        "        String x2 = \"x2\";\n"
        "        m.put(#x1, 1);\n"
        "        m.put(#x2, 2);\n"
        "        int64 sz = m.count();\n"
        "        int32 vk1 = m.get(\"k1\");\n"
        "        int32 vk2 = m.get(\"k2\");\n"
        "        int32 vx1 = m.get(\"x1\");\n"
        "        int32 vx2 = m.get(\"x2\");\n"
        "        int32 d1 = 0;\n"
        "        if (m.containsKey(\"d1\")) { d1 = 1; }\n"
        "        int32 d2 = 0;\n"
        "        if (m.containsKey(\"d2\")) { d2 = 1; }\n"
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
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(16);\n"
        "        String a = \"a\";\n"
        "        String b1 = \"b\";\n"
        "        String c = \"c\";\n"
        "        m.put(#a, 10);\n"
        "        m.put(#b1, 20);\n"
        "        m.put(#c, 30);\n"
        "        m.remove(\"b\");\n"
        // The re-insert needs a SECOND owned key — remove reclaimed the
        // first. It still probes through the tombstone, which is the point.
        "        String b2 = \"b\";\n"
        "        m.put(#b2, 99);\n"
        "        int32 va = m.get(\"a\");\n"
        "        int32 vb = m.get(\"b\");\n"
        "        int32 vc = m.get(\"c\");\n"
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

// Replace under an owning map: two writes at an EQUAL key, second wins, size
// stays 1, and the incoming duplicate key is reclaimed rather than stored.
//
// uniform-transfer 2.3 rewrote this. It used to write `m[t] = 10; m[t] = 99;`
// with one `Tag` local, which the owning `#K` makes impossible twice over: the
// lend is rejected, and surrendering `t` twice is MOVE_OF_BORROW. Replace now
// needs a SECOND key that compares equal to the first — so the key type has to
// be value-hashed, which `String` is and an identity-hashed user class is not.
//
// That asymmetry is the migration's sharpest edge and it is deliberate, not an
// oversight: see the plan's 4.2.4 and `OwnedKeyLookupTests`, which pins the
// identity MISS so the day structural equality lands for classes, it fails
// loudly. Replacing a value under an owned CLASS key has no spelling today;
// `map.update` is the open question, deferred to the collections-overhaul spec.

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
        "        m.put(#t, #b);\n"
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
// Capped functional twin of stringKeysThousandRoundTrip (stress battery,
// spec test-battery-restructure §2.3): the USE-CASE is that String keys
// survive rehash across resize — 40 keys at capacity 16 forces two resizes,
// which is all the functionality needs. The thousand-key form is load.

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
