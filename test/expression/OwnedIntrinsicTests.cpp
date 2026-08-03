// title-stores Unit 5 (spec §4): `Cajeta.owned(formal)` — the
// conditional-logic escape hatch — and the `Cajeta.moveMask()`
// retirement. RED until 5.2.x lands: owned() doesn't resolve yet and
// moveMask still compiles.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 5.1.1a — class formal: owned() answers per CALL, reorder-safe (read
// AFTER an intervening call — the word is SSA, not a clobberable TLS).
TEST(OwnedIntrinsicTests, classFormalPerCallReorderSafe) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 noise() { Cell j = heap Cell(1); return j.n; }\n"
        "    public static int32 probe(Cell c) {\n"
        "        int32 nz = D.noise();\n"          // intervening call
        "        if (Cajeta.owned(c)) {\n"
        "            Cajeta.dropValue(c);\n"
        "            return 10 + nz;\n"
        "        }\n"
        "        return 20 + nz;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cell mine = heap Cell(5);\n"
        "        int32 a = D.probe(mine);\n"        // lent → 21
        "        int32 b = D.probe(#heap Cell(6));\n"  // surrendered → 11
        "        if (mine.n != 5) { return -1; }\n"
        "        return a * 100 + b;\n"             // 2111
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2111);
}

// 5.1.1b — String formal: no drop entry exists, the word bit still
// answers (the 6.2.1 entry-less-formal rule, now user-visible).
TEST(OwnedIntrinsicTests, stringFormalSupported) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 probe(String s) {\n"
        "        if (Cajeta.owned(s)) { return 1; }\n"
        "        return 2;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        String keep = \"kept\";\n"
        "        int32 a = D.probe(keep);\n"        // lent → 2
        "        int32 b = D.probe(#\"gone\");\n"   // surrendered → 1
        "        return a * 10 + b;\n"              // 21
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

// 5.1.1c — constructors read their own call's word.
TEST(OwnedIntrinsicTests, worksInConstructors) {
    std::string src = std::string(kCellSrc) +
        "public class Adopter {\n"
        "    public int32 mode;\n"
        "    public Cell held;\n"
        "    public Adopter(Cell c) {\n"
        "        if (Cajeta.owned(c)) {\n"
        "            this.held #= c;\n"
        "            this.mode = 1;\n"
        "        } else {\n"
        "            this.held = c;\n"
        "            this.mode = 2;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell mine = heap Cell(3);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Adopter lent = heap Adopter(mine);\n"
        "            Adopter took = heap Adopter(#heap Cell(4));\n"
        "            t = lent.mode * 10 + took.mode;\n"   // 21
        "        }\n"                                       // took drops its Cell
        "        if (mine.n != 3) { return -1; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // mine
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

// 5.1.1d — non-formal argument is a compile error: owned() answers "did
// THIS call surrender v" and only formals have a word bit.
TEST(OwnedIntrinsicTests, nonFormalArgumentRejected) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell local = heap Cell(1);\n"
        "        if (Cajeta.owned(local)) { return 1; }\n"
        "        return 2;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW({
        auto jit = CajetaJit::compile(src, "test.D");
        (void) jit;
    });
}

// 5.1.2 — the interning-pool use case (spec 4.4.1) end-to-end: adopt an
// owned argument, copy a lent one; the pool reclaims exactly what it
// adopted.
TEST(OwnedIntrinsicTests, internPoolAdoptOrCopy) {
    std::string src = std::string(kCellSrc) +
        "public class Pool {\n"
        "    public Cell[] data;\n"
        "    public int32 size;\n"
        "    public Pool(int32 cap) {\n"
        "        this.data = heap Cell[cap];\n"
        "        this.size = 0;\n"
        "    }\n"
        "    public void intern(Cell c) {\n"
        "        if (Cajeta.owned(c)) {\n"
        "            this.data[this.size] #= c;\n"     // adopt
        "        } else {\n"
        "            this.data[this.size] #= heap Cell(c.n);\n"  // copy
        "        }\n"
        "        this.size = this.size + 1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell mine = heap Cell(7);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Pool p = heap Pool(4);\n"
        "            p.intern(mine);\n"               // copy
        "            p.intern(#heap Cell(9));\n"      // adopt
        "            t = p.data[0].n * 10 + p.data[1].n;\n"   // 79
        "        }\n"                                  // pool drops copy + adoptee
        "        if (mine.n != 7) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // mine
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 79);
}

// 5.1.3 — Cajeta.moveMask() is RETIRED: using it is a compile error that
// names the successors (#=, slot bits, Cajeta.owned).
TEST(OwnedIntrinsicTests, moveMaskRetired) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 probe(Cell c) {\n"
        "        int64 mvm = Cajeta.moveMask();\n"
        "        return (int32) mvm;\n"
        "    }\n"
        "    public static int32 run() { return D.probe(heap Cell(1)); }\n"
        "}\n";
    EXPECT_ANY_THROW({
        auto jit = CajetaJit::compile(src, "test.D");
        (void) jit;
    });
}
