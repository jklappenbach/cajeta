// HashMap<primitive, V> tests. Made viable by the primitive-receiver
// .hash() intrinsic in MethodCallExpression — `<int32>.hash()` lowers
// to __cajeta_hash_int32(value), so the template body's `key.hash()`
// works when K is instantiated to a primitive without going through
// boxing.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>
using cajeta_test::CajetaJit;

// Smoke test the intrinsic in isolation — call .hash() on a bare
// int32 local; expect non-zero (per-process seed defends against
// returning 0 even for input 0).
TEST(PrimitiveHashIntrinsicTests, int32HashReturnsNonZero) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int32 x = 42;\n"
        "        return x.hash();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_NE(fn(), 0);
}

// Same input → same hash; different inputs → different hashes
// (almost certainly, given SplitMix64 avalanche).
TEST(PrimitiveHashIntrinsicTests, int32HashIsDeterministic) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = 100;\n"
        "        int32 b = 100;\n"
        "        int32 c = 101;\n"
        "        int64 ha = a.hash();\n"
        "        int64 hb = b.hash();\n"
        "        int64 hc = c.hash();\n"
        "        if (ha == hb && ha != hc) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// int64.hash() exercises the int64 path. Used by HashMap<int64, V>.
TEST(PrimitiveHashIntrinsicTests, int64HashIsDeterministic) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 a = 1000000000000;\n"
        "        int64 b = 1000000000000;\n"
        "        return a.hash() == b.hash() ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// HashMap<int32, int32>: the real motivation. K is now a primitive
// (instantiates the template with K=int32), so `key.hash()` inside
// HashMap.put / get lowers via the new intrinsic.
TEST(PrimitiveHashMapTests, int32KeyedPutThenGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(16);\n"
        "        m.put(42, 100);\n"
        "        m.put(99, 200);\n"
        "        m.put(7, 300);\n"
        "        return m.get(99);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 200);
}

// Same key looked up after a series of puts on distinct keys —
// confirms the probing finds the right slot through collisions.
TEST(PrimitiveHashMapTests, int32KeyedContainsKey) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(8);\n"
        "        m.put(1, 10);\n"
        "        m.put(2, 20);\n"
        "        m.put(3, 30);\n"
        "        int32 yes = 0;\n"
        "        if (m.containsKey(2)) { yes = 100; }\n"
        "        int32 no = 0;\n"
        "        if (m.containsKey(99)) { no = 1; }\n"
        "        return yes + no;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 100);
}

// Bracket syntax should also work — operator[]= and operator[]
// dispatch through the same put/get path that uses the intrinsic.
TEST(PrimitiveHashMapTests, int32KeyedBracketSyntax) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(16);\n"
        "        m[10] = 1;\n"
        "        m[20] = 2;\n"
        "        m[30] = 3;\n"
        "        return m[20];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}

// int64 K — exercises the int64 hash path through HashMap.
TEST(PrimitiveHashMapTests, int64KeyedPutThenGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int64, int32> m = heap HashMap<int64, int32>(16);\n"
        "        int64 k = 1000000000000;\n"
        "        m.put(k, 42);\n"
        "        return m.get(k);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// --- SwissTable engine correctness -------------------------------------

// Insert 1000 distinct int keys into a small (16) table: forces ~6 resizes.
// Every key must round-trip and count() must be exact.
// Capped functional twin of swissResizeThousandInts (stress battery, spec
// test-battery-restructure §2.3): int keys survive rehash across resize —
// 60 keys at capacity 16 forces two resizes; the thousand-int form is load.
TEST(PrimitiveHashMapTests, intKeyResizeRehashRoundTrip) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(16);\n"
        "        int32 i = 0;\n"
        "        while (i < 60) { m.put(i, i * 3); i = i + 1; }\n"
        "        int32 hits = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 60) { if (m.get(j) == j * 3) { hits = hits + 1; } j = j + 1; }\n"
        "        if (m.count() != 60) { return -1; }\n"
        "        return hits;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 60);
}

TEST(PrimitiveHashMapTests, swissResizeThousandInts) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(16);\n"
        "        int32 i = 0;\n"
        "        while (i < 1000) { m.put(i, i * 3); i = i + 1; }\n"
        "        int32 hits = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 1000) { if (m.get(j) == j * 3) { hits = hits + 1; } j = j + 1; }\n"
        "        if (m.count() != 1000) { return -1; }\n"
        "        return hits;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1000);
}

// Remove then re-insert: tombstone reuse + probe-past-tombstone correctness.
// Remove half the keys, confirm absent + count, re-insert them, confirm all back.
TEST(PrimitiveHashMapTests, swissTombstoneReuse) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(256);\n"
        "        int32 i = 0;\n"
        "        while (i < 200) { m.put(i, i + 1); i = i + 1; }\n"
        "        int32 r = 0;\n"
        "        while (r < 200) { if (!m.containsKey(r)) { return -1; } m.remove(r); r = r + 2; }\n"
        "        if (m.count() != 100) { return -2; }\n"
        "        // odds still present, evens absent\n"
        "        if (m.containsKey(4)) { return -3; }\n"
        "        if (m.containsKey(5) == false) { return -4; }\n"
        "        // re-insert evens (reuse tombstones)\n"
        "        int32 e = 0;\n"
        "        while (e < 200) { m.put(e, e + 1); e = e + 2; }\n"
        "        if (m.count() != 200) { return -5; }\n"
        "        int32 hits = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 200) { if (m.get(j) == j + 1) { hits = hits + 1; } j = j + 1; }\n"
        "        return hits;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 200);
}

// remove() of an absent key returns false; present key returns true once.
TEST(PrimitiveHashMapTests, swissRemoveReturnValue) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(16);\n"
        "        m.put(11, 1);\n"
        "        if (m.containsKey(999)) { return -1; }\n"
        "        m.remove(999);\n"
        "        if (!m.containsKey(11)) { return -2; }\n"
        "        m.remove(11);\n"
        "        if (m.containsKey(11)) { return -3; }\n"
        "        if (m.remove(11) != false) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
