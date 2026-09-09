// Unit 6 (spec 5.5–5.11) — the return statement on the title classifier: the
// `#T` static checks, the plain-return checks, and the return-flag
// composition, plus pins on what must not move.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        if (!jit) return -1;
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (!fn) return -2;
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
        return -3;
    }
}

void expectRejected(const std::string& src, const std::string& code) {
    try {
        CajetaJit::compile(src.c_str(), "test.A");
        ADD_FAILURE() << "expected " << code;
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code) << "wrong diagnostic: " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "    }\n"
    "    static class Bank {\n"                       // non-final: get() dispatches virtually
    "        public Cell c;\n"
    "        public Bank(#Cell v) { this.c #= v; }\n"
    "        public Cell get() { return this.c; }\n"
    "    }\n"
    "    static #Cell mk(int32 n) { return heap Cell(n); }\n"
    "    static Cell viaPlain(int32 n) { return A.mk(n); }\n"
    "    static Cell lendVia(Bank b) { return b.get(); }\n";   // rides get()'s flag: 0

// A balanced-liveCount loop around `probe()`; `probe` must return `want`.
std::string loop(const std::string& members, const std::string& probeBody,
                 const std::string& want, int failBase) {
    return std::string(PRE) + members
        + "    static int32 probe(int32 i) {\n" + probeBody + "    }\n"
        + "    public static int32 run() {\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 1;\n"
        + "        while (i < 65) {\n"
        + "            if (A.probe(i) != " + want + ") { return " + std::to_string(failBase) + "; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
}

std::string rejectSrc(const std::string& members) {
    return std::string(PRE) + members
        + "    public static int32 run() { return 0; }\n"
        + "}\n";
}

} // namespace

// ── 6.1.1 — spec 5.5: casts peeled in the return position ─────────────────

TEST(ReturnOwnershipTests, castPeeledBorrowLocalUnderOwnedReturnRejected) {
    expectRejected(rejectSrc(
        "    static #Cell f(Cell seed) {\n"
        "        Cell acc = seed;\n"                 // a lend of the plain formal
        "        return (Cell) acc;\n"               // the cast must not hide it
        "    }\n"), "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

TEST(ReturnOwnershipTests, castPeeledStackLocalUnderOwnedReturnRejected) {
    expectRejected(rejectSrc(
        "    static #Cell f() {\n"
        "        Cell c = stack Cell(1);\n"
        "        return (Cell) c;\n"
        "    }\n"), "CAJETA_ERROR_STACK_RETURN_ESCAPES");
}

TEST(ReturnOwnershipTests, castPeeledBorrowArmUnderOwnedReturnRejected) {
    expectRejected(rejectSrc(
        "    static #Cell f(Bank b, boolean c) {\n"
        "        Cell x = b.get();\n"
        "        return c ? (Cell) x : A.mk(1);\n"   // the cast arm is a borrow
        "    }\n"), "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

TEST(ReturnOwnershipTests, castPeeledCallRideUnderPlainReturnCarriesTitle) {
    std::string src = loop(
        "    static Cell f(int32 i) { return (Cell) A.mk(i); }\n",   // the `#` callee's flag rides
        "        Cell got = A.f(i);\n"
        "        return got.n;\n", "i", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = the ride was lost behind the cast (leak)";
}

// ── 6.1.2 — a runtime-conditional local under `#T` forwards its entry ─────

TEST(ReturnOwnershipTests, runtimeBorrowLocalUnderOwnedReturnRidesItsEntry) {
    std::string src = loop(
        "    static #Cell fwd(Bank b) {\n"
        "        Cell x = A.lendVia(b);\n"           // flagged entry, 0 at run time
        "        return x;\n"                        // must ride that 0, not assert 1
        "    }\n",
        "        Bank b = heap Bank(A.mk(i));\n"
        "        Cell got #= A.fwd(b);\n"             // a forged title here double-frees b.c
        "        return got.n;\n", "i", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = the Bank's cell was freed twice or leaked";
}

// ── 6.1.3 — a concatenation under a plain return carries its title ────────

TEST(ReturnOwnershipTests, plainReturnOfConcatCarriesItsTitle) {
    std::string src = loop(
        "    static String f(int32 i) { return \"v\" + i; }\n",
        "        String s = A.f(i);\n"
        "        String w = \"v\" + i;\n"
        "        if (!s.equals(w)) { return 1; }\n"
        "        return 2;\n", "2", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value; 31 = the concatenation leaked or was freed twice";
}

// ── 6.1.4 — spec 5.11: `stack` under a plain class return is BY VALUE ─────
// A plain `T` return of `stack X(...)` is by value (sret): accepted, no title.

TEST(ReturnOwnershipTests, stackConstructionUnderPlainClassReturnLandsByValue) {
    std::string src = loop(
        "    static Cell f(int32 i) { return stack Cell(i); }\n",
        "        Cell got = A.f(i);\n"
        "        return got.n;\n", "i", 35);
    EXPECT_EQ(runVerdict(src), 0) << "35 = wrong value (the frame's copy was clobbered); 36 = live count moved";
}

// ── 6.1.5 — an entry-less, origin-less local under `#T` is a borrow ───────

TEST(ReturnOwnershipTests, literalBoundLocalUnderOwnedReturnRejected) {
    expectRejected(rejectSrc(
        "    static #String f() {\n"
        "        String s = \"lit\";\n"              // no entry: a borrow of static storage
        "        return s;\n"
        "    }\n"), "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

// ── 6.1.6–6.1.9 — pins ────────────────────────────────────────────────────

TEST(ReturnOwnershipTests, ownedLocalUnderOwnedReturnIsAccepted) {
    std::string src = loop(
        "    static #Cell f(int32 i) { Cell c = heap Cell(i); return c; }\n",
        "        Cell got #= A.f(i);\n"
        "        return got.n;\n", "i", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = the owned local leaked or double-freed";
}

TEST(ReturnOwnershipTests, transferredFormalUnderOwnedReturnIsAccepted) {
    std::string src = loop(
        "    static #Cell f(#Cell p) { return p; }\n",
        "        Cell got #= A.f(A.mk(i));\n"
        "        return got.n;\n", "i", 50);
    EXPECT_EQ(runVerdict(src), 0) << "50 = wrong value; 51 = the `#` formal's title was lost or doubled";
}

TEST(ReturnOwnershipTests, moveOfThisUnderOwnedReturnRejected) {
    expectRejected(rejectSrc(
        "    static class Box {\n"
        "        public int32 n;\n"
        "        public Box(int32 n) { this.n = n; }\n"
        "        public #Box same() { return #this; }\n"   // no move conjures a title for the receiver
        "    }\n"), "CAJETA_ERROR_OWNED_RETURN_OF_BORROWED_THIS");
}

TEST(ReturnOwnershipTests, heapArrayLiteralArmUnderOwnedReturnAccepted) {
    std::string src = loop(
        "    static #Cell[] f(boolean c, int32 i) {\n"
        // spec 5.6: a fresh arm, owned (elements are constructions, not calls).
        "        return c ? [heap Cell(i)] : [heap Cell(i + 100)];\n"
        "    }\n",
        "        Cell[] r #= A.f(i % 2 == 0, i);\n"
        "        if (i % 2 == 0) { return r[0].n; }\n"
        "        return r[0].n - 100;\n", "i", 60);
    EXPECT_EQ(runVerdict(src), 0) << "60 = wrong value; 61 = the array-literal arm leaked";
}
