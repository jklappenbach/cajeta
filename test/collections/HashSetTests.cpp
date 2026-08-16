// Smoke tests for cajeta.collection.HashSet — thin wrapper around
// cajeta.collection.HashMap<T, int8> (1-byte presence sentinel as
// value side). Works with both class T (identity-equality) and
// primitive T (via the compiler's primitive-`.hash()` intrinsic
// that lowers to __cajeta_hash_X runtime helpers).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;





// Set semantics: adding the same VALUE-EQUAL key twice counts once.
// Using identity (the same Tag instance) since v1 hash() defaults to
// pointer identity — value semantics would need @AutoHash on Tag.

// remove of an absent key returns false (matches HashMap.remove
// contract — propagated through the wrapper).

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
