// field-store-ownership Unit 1 (plan 1.1.1, spec 8): stores that are correct
// today and stay legal under spec 1.2. Each payload's owner dies before the
// read, churn recycles freed blocks, and the value is read back.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "public class Cell {\n"
    "    public int64 n;\n"
    "    public Cell(int64 v) { this.n = v; }\n"
    "}\n"
    "public final class Churn {\n"
    "    public static int64 cells() {\n"
    "        int64 s = 0L;\n"
    "        for (int32 i = 0; i < 2000; i++) {\n"
    "            Cell x = heap Cell(99L);\n"
    "            s = s + x.n;\n"
    "        }\n"
    "        return s;\n"
    "    }\n"
    "    public static int64 strings() {\n"
    "        int64 s = 0L;\n"
    "        String a = \"99\";\n"
    "        for (int32 i = 0; i < 2000; i++) {\n"
    "            String x = a + \"99\";\n"
    "            s = s + (int64) x.charAt(0);\n"
    "        }\n"
    "        return s;\n"
    "    }\n"
    "    public static #String fresh() {\n"
    "        String a = \"81\";\n"
    "        return a + \"00\";\n"
    "    }\n"
    "    public static #Cell freshCell() { return heap Cell(8100L); }\n"
    "    public static Cell plainMake() { return Churn.freshCell(); }\n"
    "}\n";

int64_t runI64(const std::string& body) {
    auto jit = CajetaJit::compile(std::string(kPrelude) + body, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

}  // namespace

// spec 8: `#Cell p; this.c #= p`, caller `#a`.
TEST(FieldStoreOwnershipTests, sharpFormalSharpStoreTakesTitle) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public Cell c;\n"
        "    public Keep() { }\n"
        "    public void keep(#Cell p) { this.c #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    static void fill(Keep k) { Cell a = heap Cell(8100L); k.keep(#a); }\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Keep k = heap Keep();\n"
        "            D.fill(k);\n"
        "            Churn.cells();\n"
        "            got = k.c.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        81000);
}

// spec 3.2.3: a plain formal stored with `#=` takes the title on `#a`.
TEST(FieldStoreOwnershipTests, plainFormalSharpStoreTakesTransfer) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public Cell c;\n"
        "    public Keep() { }\n"
        "    public void keep(Cell p) { this.c #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    static void fill(Keep k) { Cell a = heap Cell(8100L); k.keep(#a); }\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Keep k = heap Keep();\n"
        "            D.fill(k);\n"
        "            Churn.cells();\n"
        "            got = k.c.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        81000);
}

// spec 3.2.3: the same formal records a borrow on a lend, and the lender frees.
TEST(FieldStoreOwnershipTests, plainFormalSharpStoreRecordsLend) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public Cell c;\n"
        "    public Keep() { }\n"
        "    public void keep(Cell p) { this.c #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Cell a = heap Cell(8100L);\n"
        "            Keep k = heap Keep();\n"
        "            k.keep(a);\n"
        "            Churn.cells();\n"
        "            got = k.c.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        81000);
}

// spec 3.2.5: String formals follow the same table.
TEST(FieldStoreOwnershipTests, stringFormalSharpStoreTakesTransfer) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public String v;\n"
        "    public Keep() { }\n"
        "    public void keep(String p) { this.v #= p; }\n"
        "    public void keepSharp(#String p) { this.v #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    static void fill(Keep k) { String s #= Churn.fresh(); k.keep(#s); }\n"
        "    static void fillSharp(Keep k) { String s #= Churn.fresh(); k.keepSharp(#s); }\n"
        "    public static int64 run() {\n"
        "        Keep k = heap Keep();\n"
        "        Keep j = heap Keep();\n"
        "        D.fill(k);\n"
        "        D.fillSharp(j);\n"
        "        Churn.strings();\n"
        "        int64 r = 0L;\n"
        "        if (k.v.equals(\"8100\")) { r = r + 1L; }\n"
        "        if (j.v.equals(\"8100\")) { r = r + 2L; }\n"
        "        return r;\n"
        "    }\n"
        "}\n"),
        3);
}

// spec 3.2.5: a String lend through `#=` reads back while the lender lives.
TEST(FieldStoreOwnershipTests, stringFormalSharpStoreRecordsLend) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public String v;\n"
        "    public Keep() { }\n"
        "    public void keep(String p) { this.v #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        String s #= Churn.fresh();\n"
        "        Keep k = heap Keep();\n"
        "        k.keep(s);\n"
        "        Churn.strings();\n"
        "        if (k.v.equals(\"8100\") && s.equals(\"8100\")) { return 1L; }\n"
        "        return 0L;\n"
        "    }\n"
        "}\n"),
        1);
}

// spec 4.2.2: an owned local stored with `#=` survives the method.
TEST(FieldStoreOwnershipTests, ownedLocalSharpStoreSurvives) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public Cell c;\n"
        "    public Cell d;\n"
        "    public Keep() { }\n"
        "    public void fromHeap() { Cell l = heap Cell(8100L); this.c #= l; }\n"
        "    public void fromCall() { Cell l = Churn.plainMake(); this.d #= l; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Keep k = heap Keep();\n"
        "            k.fromHeap();\n"
        "            k.fromCall();\n"
        "            Churn.cells();\n"
        "            got = k.c.n + k.d.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        162000);
}

// spec 1.2 rule 6: a `heap` or a call written in place may be stored with `=`.
TEST(FieldStoreOwnershipTests, inPlaceProducerPlainStoreSurvives) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public Cell c;\n"
        "    public Cell d;\n"
        "    public Keep() { }\n"
        "    public void fill() {\n"
        "        this.c = heap Cell(8100L);\n"
        "        this.d = Churn.plainMake();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 got = 0L;\n"
        "        {\n"
        "            Keep k = heap Keep();\n"
        "            k.fill();\n"
        "            Churn.cells();\n"
        "            got = k.c.n + k.d.n;\n"
        "        }\n"
        "        return got * 10L + (Cajeta.liveCount() - base);\n"
        "    }\n"
        "}\n"),
        162000);
}

// spec 8: `{ Cell sp = heap ..; out #= sp; }` keeps the title past the block.
TEST(FieldStoreOwnershipTests, blockLocalSharpBindSurvivesBlock) {
    EXPECT_EQ(runI64(
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Cell out = null;\n"
        "        int64[] arr = null;\n"
        "        {\n"
        "            Cell sp = heap Cell(8100L);\n"
        "            out #= sp;\n"
        "            int64[] sa = heap int64[2];\n"
        "            sa[1] = 47L;\n"
        "            arr #= sa;\n"
        "        }\n"
        "        Churn.cells();\n"
        "        int64[] c1 = heap int64[2];\n"
        "        c1[1] = 99L;\n"
        "        return out.n + arr[1];\n"
        "    }\n"
        "}\n"),
        8147);
}

// spec 3.1: a slot store follows the formal table.
TEST(FieldStoreOwnershipTests, slotSharpStoreTakesTransfer) {
    EXPECT_EQ(runI64(
        "public final class Keep {\n"
        "    public Cell[] items;\n"
        "    public Keep() { this.items = heap Cell[2]; }\n"
        "    public void put(int32 i, Cell p) { this.items[i] #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    static void fill(Keep k) { Cell a = heap Cell(8100L); k.put(1, #a); }\n"
        "    public static int64 run() {\n"
        "        Keep k = heap Keep();\n"
        "        D.fill(k);\n"
        "        Churn.cells();\n"
        "        return k.items[1].n;\n"
        "    }\n"
        "}\n"),
        8100);
}

// Instrument check, spec 1.3: a lend whose owner dies first reads back wrong,
// so the churn above does recycle a freed block.
TEST(FieldStoreOwnershipTests, churnExposesALendThatOutlivesItsOwner) {
    EXPECT_NE(runI64(
        "public final class Keep {\n"
        "    public Cell c;\n"
        "    public Keep() { }\n"
        "    public void keep(Cell p) { this.c #= p; }\n"
        "}\n"
        "public final class D {\n"
        "    static void fill(Keep k) { Cell a = heap Cell(8100L); k.keep(a); }\n"
        "    public static int64 run() {\n"
        "        Keep k = heap Keep();\n"
        "        D.fill(k);\n"
        "        Churn.cells();\n"
        "        return k.c.n;\n"
        "    }\n"
        "}\n"),
        8100);
}
