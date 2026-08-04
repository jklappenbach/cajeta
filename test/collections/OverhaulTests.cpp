// collections-overhaul (specs/collections-overhaul-spec.md) — pins for the
// review-and-rewrite pass: miss paths stay correct after the stack-zero
// rewrite (2.1.1/2.1.2), HashSet.remove answers membership in one probe
// (2.1.3), Heap survives popping its last element (2.1.7), and the new
// ArrayList surface (insert / removeAt / clear / bounds — 2.4.1) behaves.
// Ownership-drop exactness is covered suite-wide by the MALLOC_PERTURB_
// run; these tests pin the functional contracts.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

// 1.1.1 — get/remove on an absent key return the zero value and leave the
// map usable. Also exercises the remove-miss path (stack zero default).
TEST(OverhaulTests, hashMapMissPathsReturnZeroAndStayUsable) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, int32> m = heap HashMap<int32, int32>(16);\n"
        "        m.put(1, 100);\n"
        "        int32 missGet = m.get(7);\n"          // 0
        "        int32 missRemove = m.remove(8);\n"    // 0
        "        m.put(2, 200);\n"
        "        return missGet + missRemove + m.get(1) + m.get(2);\n"  // 300
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 300);
}

// 1.1.2 — HashSet.remove reports membership: true once, false after, false
// for a never-member. Pins the single-probe rewrite (2.1.3).
TEST(OverhaulTests, hashSetRemoveReportsMembership) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<int32> s = heap HashSet<int32>(16);\n"
        "        s.add(7);\n"
        "        int32 acc = 0;\n"
        "        if (s.remove(7))  { acc = acc + 1; }\n"   // present -> true
        "        if (s.remove(7))  { acc = acc + 10; }\n"  // gone -> false
        "        if (s.remove(42)) { acc = acc + 100; }\n" // never -> false
        "        return acc + (int32) s.count();\n"        // 1 + 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 1.1.3 — Heap: popping the final element (the data[0] #= data[0] edge) and
// an interleaved push/pop sequence keep the invariant.
TEST(OverhaulTests, heapPopLastElementAndInterleave) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Heap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Heap<int32> h = heap Heap<int32>();\n"
        "        h.push(5);\n"
        "        int32 only = h.pop();\n"               // 5, heap empties
        "        if (!h.isEmpty()) { return -1; }\n"
        "        h.push(3);\n"
        "        h.push(1);\n"
        "        h.push(2);\n"
        "        int32 a = h.pop();\n"                  // 1
        "        h.push(0);\n"
        "        int32 b = h.pop();\n"                  // 0
        "        int32 c = h.pop();\n"                  // 2
        "        int32 d = h.pop();\n"                  // 3
        "        return only * 10000 + a * 1000 + b * 100 + c * 10 + d;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 51023);
}

// 3.1.1 — insert at head, middle, and tail preserves order and count.
TEST(OverhaulTests, arrayListInsertHeadMiddleTail) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(2);\n"
        "        xs.add(4);\n"
        "        xs.insert(0, 1);\n"       // [1,2,4]
        "        xs.insert(2, 3);\n"       // [1,2,3,4]
        "        xs.insert(4, 5);\n"       // [1,2,3,4,5]
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < xs.count()) {\n"
        "            acc = acc * 10 + xs.get(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return acc;\n"            // 12345
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 12345);
}

// 3.1.2 — removeAt returns the element and closes the gap; removing the
// last element and the first element both keep order.
TEST(OverhaulTests, arrayListRemoveAtReturnsAndCompacts) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(1);\n"
        "        xs.add(2);\n"
        "        xs.add(3);\n"
        "        xs.add(4);\n"
        "        int32 mid  = xs.removeAt(1);\n"   // 2 -> [1,3,4]
        "        int32 last = xs.removeAt(2);\n"   // 4 -> [1,3]
        "        int32 head = xs.removeAt(0);\n"   // 1 -> [3]
        "        return mid * 1000 + last * 100 + head * 10 + xs.get(0)\n"
        "             + (int32) xs.count() * 100000;\n"  // 100000 + 2000+400+10+3
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 102413);
}

// 3.1.2b — removeAt on a String list: the returned element is owned and
// readable; a DISCARDED removeAt drops the element without corruption
// (exact-once verified by the MALLOC_PERTURB_ suite run).
TEST(OverhaulTests, arrayListRemoveAtStringOwnership) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<String> xs = heap ArrayList<String>();\n"
        "        xs.add(\"alpha\");\n"
        "        xs.add(\"beta\");\n"
        "        xs.add(\"gamma\");\n"
        "        String taken = xs.removeAt(1);\n"      // \"beta\"\n
        "        xs.removeAt(0);\n"                     // discarded \"alpha\"
        "        int32 acc = 0;\n"
        "        if (taken.equals(\"beta\")) { acc = acc + 1; }\n"
        "        if (xs.get(0).equals(\"gamma\")) { acc = acc + 10; }\n"
        "        return acc + (int32) xs.count() * 100;\n"  // 111
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 111);
}

// 3.1.3 — bounds: get/set/insert/removeAt out of range throw a catchable
// Exception; clear() empties the list and it remains usable.
TEST(OverhaulTests, arrayListBoundsThrowAndClear) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(1);\n"
        "        int32 acc = 0;\n"
        "        try { xs.get(5); } catch (Exception e) { acc = acc + 1; }\n"
        "        try { xs.set(1, 9); } catch (Exception e) { acc = acc + 10; }\n"
        "        try { xs.insert(3, 9); } catch (Exception e) { acc = acc + 100; }\n"
        "        try { xs.removeAt(1); } catch (Exception e) { acc = acc + 1000; }\n"
        "        xs.clear();\n"
        "        if (xs.isEmpty()) { acc = acc + 10000; }\n"
        "        xs.add(7);\n"
        "        return acc + xs.get(0);\n"   // 11111 + 7
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 11118);
}

// 3.1.3b — clear() on a String list drops the elements (exact-once under
// MALLOC_PERTURB_) and the list is reusable afterwards.
TEST(OverhaulTests, arrayListClearDropsOwnedElements) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<String> xs = heap ArrayList<String>();\n"
        "        xs.add(\"a\" + \"1\");\n"
        "        xs.add(\"b\" + \"2\");\n"
        "        xs.clear();\n"
        "        xs.add(\"c\" + \"3\");\n"
        "        if (xs.get(0).equals(\"c3\")) { return (int32) xs.count(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// 2.4.2 — HashSet no-arg ctor + stream() over members.
TEST(OverhaulTests, hashSetDefaultCtorAndStream) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "import cajeta.lang.stream.Stream;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<int32> s = heap HashSet<int32>();\n"
        "        s.add(3);\n"
        "        s.add(5);\n"
        "        s.add(3);\n"
        "        Stream<int32> st = s.stream();\n"
        "        int32 sum = 0;\n"
        "        Optional<int32> nx = st.next();\n"
        "        while (nx.isPresent()) {\n"
        "            sum = sum + nx.get();\n"
        "            nx = st.next();\n"
        "        }\n"
        "        return sum + (int32) s.count() * 100;\n"  // 8 + 200
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 208);
}

// 2.1.6 — Cache.evict with TTL: expired entries go, fresh entries stay.
// (The early-break is an internal detail; this pins the behavior it must
// preserve: expiry is judged per entry even when the walk short-circuits.)
TEST(OverhaulTests, cacheEvictDropsExpiredKeepsFresh) {
    auto src =
        "package test;\n"
        "import cajeta.collection.Cache;\n"
        "import cajeta.time.Duration;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cache<int32, int32> c = heap Cache<int32, int32>(8);\n"
        "        c.setMaxAge(Duration.ofNanos(1));\n"
        "        c.put(1, 100);\n"
        "        int64 spin = 0;\n"
        "        while (spin < 100000) { spin = spin + 1; }\n"  // let 1ns lapse
        "        c.evict();\n"
        "        int32 afterExpiry = c.count();\n"              // 0
        "        c.setMaxAge(Duration.ofSeconds(3600));\n"
        "        c.put(2, 200);\n"
        "        c.evict();\n"
        "        return afterExpiry * 100 + c.count();\n"        // 0*100 + 1
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
