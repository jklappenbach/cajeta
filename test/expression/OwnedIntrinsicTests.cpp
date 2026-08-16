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

// 5.1.1c — constructors read their own call's word.

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
