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
