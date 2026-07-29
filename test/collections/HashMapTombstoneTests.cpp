//
// title-tracking 6.2.6b — tombstone-reuse staleness. HashMap.remove used to
// drop an owned String key but LEAVE the stale pointer in the DELETED
// slot's key field. The String field-store emitter unconditionally drops
// the old field value ("the field owns its wrapper"), so a later put that
// reused the tombstone old-dropped the stale pointer — and when malloc had
// recycled that address for the INCOMING key (same-size alloc, the common
// case), the live-set claim succeeded and the store freed the key it was
// storing. Symptoms: entries present per count() but unfindable, values
// vanishing after unrelated removes, and the DnsCache 4-resolve SIGSEGV
// (third eviction) — all allocation-timing dependent.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// Alternating remove + reinsert of two String keys through tombstones.
// After put0 put1 rm0 put0b rm1 put1b rm0b the map must hold exactly
// key-1 (count 1, key-1 findable with the last value, key-0 absent).
// Encoded oracle: count*100 + has0*10 + has1 → expect 0*?; 1*100+0+1=101...
// plus the surviving value read back.
TEST(HashMapTombstoneTests, removeReinsertAltKeysStaysConsistent) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 nn) { this.n = nn; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #String keyFor(int32 i) {\n"
        "        return \"key-\" + i;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        HashMap<String, Cell> map = heap HashMap<String, Cell>(16);\n"
        "        map.put(#D.keyFor(0), #heap Cell(100));\n"
        "        map.put(#D.keyFor(1), #heap Cell(101));\n"
        "        String r0 = D.keyFor(0);\n"
        "        map.remove(r0);\n"
        "        map.put(#D.keyFor(0), #heap Cell(102));\n"
        "        String r1 = D.keyFor(1);\n"
        "        map.remove(r1);\n"
        "        map.put(#D.keyFor(1), #heap Cell(103));\n"
        "        String r0b = D.keyFor(0);\n"
        "        map.remove(r0b);\n"
        "        String k0 = D.keyFor(0);\n"
        "        String k1 = D.keyFor(1);\n"
        "        int32 has0 = map.containsKey(k0) ? 1 : 0;\n"
        "        int32 has1 = map.containsKey(k1) ? 1 : 0;\n"
        "        Cell v1 = map.get(k1);\n"
        "        int32 n1 = (v1 != null) ? v1.n : -1;\n"
        // expect: count=1, has0=0, has1=1, n1=103 → 1*1000+0+1*10+103... keep
        // it readable: count*1000 + has0*100 + has1*10 + (n1==103 ? 1 : 0)
        "        int32 ok103 = (n1 == 103) ? 1 : 0;\n"
        "        return (int32) map.count() * 1000 + has0 * 100 + has1 * 10 + ok103;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1011);
}

// Same-key churn through a tombstone many times — every generation must
// stay findable immediately after its reinsert, and the final count is 1.
TEST(HashMapTombstoneTests, repeatedRemoveReinsertSameKeyFindable) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 nn) { this.n = nn; }\n"
        "}\n"
        "public final class D {\n"
        "    public static #String keyFor(int32 i) {\n"
        "        return \"key-\" + i;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        HashMap<String, Cell> map = heap HashMap<String, Cell>(16);\n"
        "        int32 good = 0;\n"
        "        int32 g = 0;\n"
        "        while (g < 8) {\n"
        "            map.put(#D.keyFor(7), #heap Cell(200 + g));\n"
        "            String probe = D.keyFor(7);\n"
        "            Cell v = map.get(probe);\n"
        "            if (v != null && v.n == 200 + g) { good = good + 1; }\n"
        "            String rk = D.keyFor(7);\n"
        "            map.remove(rk);\n"
        "            g = g + 1;\n"
        "        }\n"
        "        return good * 10 + (int32) map.count();\n"   // 8 generations, empty map
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 80);
}
