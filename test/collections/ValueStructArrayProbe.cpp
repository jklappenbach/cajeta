// Probe: can we store a @ValueType struct INLINE in a heap array and use
// `arr[i].field` as an lvalue (write) and rvalue (read)? This is the codegen
// the dense-layout SwissTable (Entry<K,V>[] slots) needs. Isolates the three
// gaps: (1) concrete value-struct array + field lvalue, (2) the same with a
// GENERIC @ValueType<K,V>.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

// (1) Concrete @ValueType in a heap array, write fields via arr[i].f, read back.
TEST(ValueStructArrayProbe, concreteValueStructArrayFieldLValue) {
    auto src =
        "package test;\n"
        "@ValueType public final class IntPair {\n"
        "    public int32 key;\n"
        "    public int32 val;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IntPair[] slots = heap IntPair[16];\n"
        "        slots[3].key = 42;\n"
        "        slots[3].val = 99;\n"
        "        slots[7].key = 5;\n"
        "        return slots[3].key + slots[3].val + slots[7].key;\n"  // 146
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 146);
}

// (2) GENERIC @ValueType<K,V> in a heap array — the actual map storage shape.
TEST(ValueStructArrayProbe, genericValueStructArrayFieldLValue) {
    auto src =
        "package test;\n"
        "@ValueType public final class Entry<K, V> {\n"
        "    public K key;\n"
        "    public V val;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Entry<int32, int32>[] slots = heap Entry<int32, int32>[16];\n"
        "        slots[3].key = 42;\n"
        "        slots[3].val = 99;\n"
        "        return slots[3].key + slots[3].val;\n"  // 141
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 141);
}
