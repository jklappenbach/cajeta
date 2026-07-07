// Verifies the String ownership/drop model behind the hashmap-string benchmark:
// owned (mode 0) Strings — concat results, allocating-method copies — are freed
// when their owner drops; view-mode literals and borrowed aliases are not.
// Cajeta.liveCount() reports the live-object population; we diff it across a unit
// of work. (docs/specification/lang/String.md § Memory model.)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
std::unique_ptr<CajetaJit> jitOf(const char* body) {
    std::string src =
        std::string("package test;\n") +
        "import cajeta.collection.HashMap;\n" +
        "public final class D {\n" + body + "}\n";
    return CajetaJit::compile(src, "test.D");
}
}

// Loop of owned concat Strings (no map). Each must drop at its loop-body scope.
TEST(OwnershipLeakProbe, loopConcatFreed) {
    auto jit = jitOf(
        "    public static int64 spin(int32 n) {\n"
        "        int64 s = 0; int32 j = 0;\n"
        "        while (j < n) { String q = \"key\" + j; s = s + (int64) q.count(); j = j + 1; }\n"
        "        return s;\n"
        "    }\n"
        "    public static int64 run(int32 n) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        spin(n);\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("run");
    EXPECT_LT(fn(4000), 50) << "owned concat Strings leaked at loop-scope exit";
}

// A borrowed alias must NOT be dropped (the source still owns it) — no double-free.
TEST(OwnershipLeakProbe, borrowAliasNotDoubleFreed) {
    auto jit = jitOf(
        "    public static int64 run() {\n"
        "        int64 acc = 0; int32 j = 0;\n"
        "        while (j < 1000) {\n"
        "            String a = \"key\" + j;\n"
        "            String b = a;\n"            // borrow alias — b must not drop a's buffer
        "            acc = acc + (int64) b.count();\n"
        "            j = j + 1;\n"
        "        }\n"
        "        return acc;\n"                  // clean exit (no UAF/double-free abort) is the assertion
        "    }\n");
    auto fn = jit->lookup<int64_t (*)()>("run");
    // Byte-length sum of "key0".."key999": 10*4 + 90*5 + 900*6 = 5890. A correct
    // borrow (no double-free crash, no clobbered buffer) returns exactly this.
    EXPECT_EQ(fn(), 5890);
}

// 15.13 regression (element-ownership 3.1.2): an OWNING instantiation
// (`ArrayList<#String>`) drops its elements when the list drops — `#`-marked
// storage joins the field-drop walk, bounded by the @ElementCount field (the
// array header word is capacity, not live count). Pre-fix baseline: +1006.
TEST(OwnershipLeakProbe, arrayListOwnedElementsDropped) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class E {\n"
        "    public static void fill(int32 n) {\n"
        "        ArrayList<#String> a = heap ArrayList<#String>();\n"
        "        int32 i = 0;\n"
        "        while (i < n) { String s = \"elem\" + i; a.add(#s); i = i + 1; }\n"
        "    }\n"
        "    public static int64 run(int32 n) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        fill(n);\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.E");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("run");
    int64_t delta = fn(1000);
    EXPECT_GE(delta, 0);
    EXPECT_LT(delta, 20) << "owned ArrayList elements leaked: +" << delta;
}

// Borrow→owning materialize (element-ownership 3.1.4): storing a BORROW
// source into an owning container's slot must materialize (fresh wrapper the
// slot owns) — storing the borrowed wrapper would dangle when the source
// scope exits, and 3B's teardown loop would then double-free it. The probe
// reads the stored value AFTER the source scope closed.
TEST(OwnershipLeakProbe, borrowIntoOwningSlotMaterializes) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class G {\n"
        "    public static int64 run() {\n"
        "        ArrayList<#String> a = heap ArrayList<#String>();\n"
        "        if (true) {\n"
        "            String s = \"born\" + 42;\n"
        "            a.add(s);\n"                     // borrow source, owning slot
        "        }\n"
        // Churn the allocator so a freed source wrapper gets reused —
        // a raw borrowed pointer in the slot then reads clobbered bytes.
        "        int64 churn = 0; int32 c = 0;\n"
        "        while (c < 64) { String j = \"clobber\" + c; churn = churn + (int64) j.count(); c = c + 1; }\n"
        "        if (churn < 0) { return churn; }\n"
        "        String e = a.get(0);\n"
        "        return (int64) e.count();\n"          // "born42" -> 6
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 6) << "borrow stored raw into owning slot dangled";
}

// Balance check for the materialize path (element-ownership 3.1.5 slice):
// borrow-add ×N into an owning list, drop everything → liveCount returns to
// baseline (materialized wrappers dropped by the 3B walk, sources by scope).
TEST(OwnershipLeakProbe, borrowIntoOwningSlotBalances) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class H {\n"
        "    public static void fill(int32 n) {\n"
        "        ArrayList<#String> a = heap ArrayList<#String>();\n"
        "        int32 i = 0;\n"
        "        while (i < n) { String s = \"elem\" + i; a.add(s); i = i + 1; }\n"
        "    }\n"
        "    public static int64 run(int32 n) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        fill(n);\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.H");
    auto fn = jit->lookup<int64_t (*)(int32_t)>("run");
    int64_t delta = fn(1000);
    EXPECT_GE(delta, 0);
    EXPECT_LT(delta, 20) << "borrow->owning materialize leaked: +" << delta;
}

// Borrow→owning materialize, heap-root view arm (element-ownership 3.1.3 +
// the 3.1.5 balance): storing a >256 B heap-backed VIEW into an owning slot
// promotes-to-shared (stake on the root, no byte copy) — sharedPopulation
// rises while the container lives and returns to baseline when the 3B
// teardown walk releases the slot's stake. Returns the observed rise
// (>= 1), or -1 on a dangled read, -2 on a stake imbalance after drop.
TEST(OwnershipLeakProbe, viewIntoOwningSlotPromotesToShared) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class I {\n"
        "    public static int64 run() {\n"
        "        int64 sharedBase = Cajeta.sharedPopulation();\n"
        "        int64 rise = 0;\n"
        "        if (true) {\n"
        // StringBuilder.toString() -> owned HEAP buffer (a concat-built local
        // would be frame-arena-backed, and arena sources copy at any size).
        "            StringBuilder sb = heap StringBuilder();\n"
        "            int32 i = 0;\n"
        "            while (i < 30) { sb.append(\"0123456789\"); i = i + 1; }\n"
        "            String big = sb.toString();\n"
        "            ArrayList<#String> a = heap ArrayList<#String>();\n"
        "            if (true) {\n"
        "                String v = big.substring(10, 280);\n"  // mode-2 heap-root view
        "                a.add(v);\n"                            // borrow view -> owning slot
        "            }\n"
        "            rise = Cajeta.sharedPopulation() - sharedBase;\n"
        "            String e = a.get(0);\n"
        "            if ((int64) e.count() != 270) { return (int64) -1; }\n"
        "        }\n"
        "        if (Cajeta.sharedPopulation() != sharedBase) { return (int64) -2; }\n"
        "        return rise;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.I");
    auto fn = jit->lookup<int64_t (*)()>("run");
    int64_t rise = fn();
    EXPECT_GE(rise, 1) << (rise == -1 ? "stored view dangled"
                          : rise == -2 ? "stake imbalance after drop"
                          : "no stake taken — view was byte-copied or stored raw");
}

// Borrow-instantiation control: `ArrayList<String>` (no `#`) must NOT drop
// elements — they belong to the enclosing scope; a premature free would
// poison `keep` before the trailing read (element-ownership §7.1.4 gate).
TEST(OwnershipLeakProbe, arrayListBorrowElementsUntouched) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class F {\n"
        "    public static int64 run() {\n"
        "        String keep = \"keep\" + 7;\n"
        "        if (true) {\n"
        "            ArrayList<String> a = heap ArrayList<String>();\n"
        "            a.add(keep);\n"
        "            a.add(keep);\n"
        "        }\n"
        "        return (int64) keep.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.F");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 5) << "borrow-instantiated list touched elements it does not own";
}

// Bench-faithful: build a #-keyed HashMap<String,int32> AND do n lookups (each a
// throwaway borrowed "key"+j), repeated over many iterations. The live-set must
// stay BOUNDED — owned keys reclaimed on map drop, lookup temps on loop exit.
TEST(OwnershipLeakProbe, benchScaleStaysBounded) {
    auto jit = jitOf(
        "    public static int64 iter(int32 n) {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>(65536);\n"
        "        int32 i = 0;\n"
        "        while (i < n) { String k = \"key\" + i; m.put(#k, i); i = i + 1; }\n"
        "        int64 h = 0; int32 j = 0;\n"
        "        while (j < n) { String q = \"key\" + j; int32 v = m.get(q); if (v == j) { h = h + 1; } j = j + 1; }\n"
        "        return h;\n"
        "    }\n"
        "    public static int64 run(int32 n, int32 iters) {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0; int64 acc = 0;\n"
        "        while (t < iters) { acc = acc + iter(n); t = t + 1; }\n"
        "        if (acc != (int64) n * (int64) iters) { return (int64) -1; }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n");
    auto fn = jit->lookup<int64_t (*)(int32_t, int32_t)>("run");
    int64_t delta = fn(4000, 8);
    EXPECT_GE(delta, 0);
    EXPECT_LT(delta, 200) << "live-set grew by " << delta
                          << " over 8 iterations — per-iteration leak remains";
}
