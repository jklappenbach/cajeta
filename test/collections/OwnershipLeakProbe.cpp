// Verifies the String ownership/drop model behind the hashmap-string benchmark:
// owned (mode 0) Strings — concat results, allocating-method copies — are freed
// when their owner drops; view-mode literals and borrowed aliases are not.
// Cajeta.liveCount() reports the live-object population; we diff it across a unit
// of work. (docs/specification/lang/String.md § Memory model.)

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {
std::unique_ptr<CajetaJit> jitOf(const char* body) {
    std::string src =
        std::string("package test;\n") +
        "import cajeta.collection.HashMap;\n" +
        "public final class D {\n" + body + "}\n";
    return CajetaJit::compile(src, "test.D");
}

// uniform-transfer-semantics 2.1.7 — the lend-into-a-container shape is now a
// COMPILE error, so those contracts are asserted on the compiler.
std::string compileExpectError(const std::string& src,
                               const std::string& expectCode,
                               const char* entryClass = "test.F") {
    try {
        CajetaJit::compile(src, entryClass);
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected a cajeta::Exception, got " << e.what();
        return e.what();
    }
    ADD_FAILURE() << "expected " << expectCode << ", got a clean compile";
    return "";
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
// (`ArrayList<String>`) drops its elements when the list drops — `#`-marked
// storage joins the field-drop walk, bounded by the @ElementCount field (the
// array header word is capacity, not live count). Pre-fix baseline: +1006.
TEST(OwnershipLeakProbe, arrayListOwnedElementsDropped) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class E {\n"
        "    public static void fill(int32 n) {\n"
        "        ArrayList<String> a = heap ArrayList<String>();\n"
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

// title-tracking §8.1 (plan 7.1.2) — the borrow→owning-slot MATERIALIZE tests
// were RETIRED with owning instantiations. uniform-transfer-semantics 2.1.7
// finished the job: there is no borrow-store to pin any more, because lending
// into a container is a compile error (see arrayListStringLendIsRejected
// below and ContainerTitleTests.indexedLendIsRejected).

// Balance check at scale: N owned adds into a list, drop everything →
// liveCount returns to baseline. Each String is surrendered to the list, which
// reclaims it at teardown; nothing is dropped twice and nothing is stranded.
TEST(OwnershipLeakProbe, ownedElementsBalanceAtScale) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class H {\n"
        "    public static void fill(int32 n) {\n"
        "        ArrayList<String> a = heap ArrayList<String>();\n"
        "        int32 i = 0;\n"
        "        while (i < n) { String s = \"elem\" + i; a.add(#s); i = i + 1; }\n"
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
    EXPECT_LT(delta, 20) << "owned container elements leaked: +" << delta;
}

// title-tracking §8.1 (plan 7.1.2) — viewIntoOwningSlotPromotesToShared
// RETIRED with the materialize family (no owning slots to promote into);
// the String view/share machinery itself stays pinned by the String suites.


// uniform-transfer-semantics 2.1.7 — was `arrayListBorrowElementsUntouched`.
//
// Its premise was that a list BORROWS its elements and must not touch them:
// `a.add(keep); a.add(keep);` twice with the same String, then read `keep`
// after the list's scope. Spec 2.3 abolishes borrowing elements, and phase 1
// made String a fully owned class, so BOTH halves of that fixture are now
// diagnosed. This pins the two diagnostics it turns into — the add-the-same-
// element-twice hazard the original was quietly relying on is now named.
// A LEND of a String into an ArrayList. Collections do not own by default, so
// the plain spelling compiles and the list stores a BORROW: `keep` keeps title
// and is still readable after the add, and the list must not free it.
TEST(OwnershipLeakProbe, arrayListStringLendStoresABorrow) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class F {\n"
        "    public static int64 run() {\n"
        "        String keep = \"keep\" + 7;\n"
        "        ArrayList<String> a = heap ArrayList<String>();\n"
        "        a.add(keep);\n"
        "        return (int64) keep.count();\n"
        "    }\n"
        "}\n";
    // "keep7".count() == 5 — the borrow is intact after the list took it.
    auto jit = CajetaJit::compile(src, "test.F");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

// The same binding cannot be surrendered twice: the first `#keep` demotes it
// to a borrow, and transferring from a borrow is the one error that shape
// raises. The old fixture added `keep` twice and got away with it only
// because the list did not take a title either time.
TEST(OwnershipLeakProbe, arrayListStringAddedTwiceIsRejected) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class F {\n"
        "    public static int64 run() {\n"
        "        String keep = \"keep\" + 7;\n"
        "        ArrayList<String> a = heap ArrayList<String>();\n"
        "        a.add(#keep);\n"
        "        a.add(#keep);\n"
        "        return (int64) a.get(0).count();\n"
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_MOVE_OF_BORROW");
}

// 2.1.4 — the three spellings, all in one place. `add(s)` is rejected;
// `add(#s)` surrenders the one String; a fresh copy (`substring` returns
// `#String`) gives the list its own element and leaves `s` an owner. The
// plan wrote this third form as `s.clone()`; String has no clone — a
// full-width `substring` is the copy idiom it does have.
TEST(OwnershipLeakProbe, stringElementTransferSpellings) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class G {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 t = 0;\n"
        "        {\n"
        "            String s = \"keep\" + 7;\n"
        "            ArrayList<String> a = heap ArrayList<String>();\n"
        "            a.add(s.substring(0, s.count()));\n"   // fresh copy
        "            a.add(#s);\n"                          // surrender
        "            t = (int64) (a.get(0).count() + a.get(1).count());\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return leaked * 100 + t;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 10);
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
