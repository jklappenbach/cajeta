// Unit 5 (spec 5.1–5.11) — the store sites on the title classifier: field
// own-bit, tail slot, field-array element, local re-arm, `operator[]=`, and
// array-literal element stores, plus the 5.10 / 5.11 rejections.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const std::string& src) {
    auto jit = CajetaJit::compile(src.c_str(), "test.A");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
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

void expectAccepted(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        EXPECT_TRUE(jit != nullptr);
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
    }
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.collection.HashMap;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "    }\n"
    "    static class Pt { public int32 x; public int32 y; }\n"
    "    static class Holder {\n"
    "        public Cell c;\n"
    "        public Pt p;\n"
    "        public String s;\n"
    "        public Holder() { }\n"
    "    }\n"
    "    static #Cell mk(int32 n) { return heap Cell(n); }\n"
    "    static #String mkS(int32 i) { return \"v\" + i; }\n"
    "    static int32 digits(int32 v) { return v < 10 ? 1 : 2; }\n"
    "    static Cell viaPlain(int32 n) { return A.mk(n); }\n";

// A balanced-liveCount loop around `probe()`; `probe` must return `want`.
std::string loop(const std::string& probeBody, const std::string& want, int failBase) {
    return std::string(PRE)
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

} // namespace

// ── 5.1.1 — aggregate `heap` initialiser is owned at every store site ──────

TEST(StoreOwnershipTests, heapAggregateIntoFieldIsOwned) {
    std::string src = loop(
        "        Holder h = heap Holder();\n"
        "        h.p = heap Pt { x: i, y: 2 };\n"          // owned by the field: freed with h
        "        return h.p.x;\n", "i", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = the aggregate leaked (field recorded a borrow)";
}

TEST(StoreOwnershipTests, heapAggregateIntoTailSlotIsOwned) {
    std::string src = loop(
        "        Pt[] ps = heap Pt[2];\n"
        "        ps[1] = heap Pt { x: i, y: 3 };\n"        // owned by the slot: freed with ps
        "        return ps[1].x;\n", "i", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = the aggregate leaked (slot recorded a borrow)";
}

TEST(StoreOwnershipTests, heapAggregateIntoRearmedLocalIsOwned) {
    std::string src = loop(
        "        Pt p = heap Pt { x: 1, y: 1 };\n"
        "        p = heap Pt { x: i, y: 4 };\n"            // displaced one released, new one owned
        "        return p.x;\n", "i", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value; 31 = one of the two aggregates leaked";
}

// ── 5.1.2 — closure-call and plain-callee results ride the flag ──────────

TEST(StoreOwnershipTests, closureCallResultIntoFieldRidesTheFlag) {
    std::string src = loop(
        "        (int32) -> #Cell maker = (int32 n) -> heap Cell(n);\n"
        "        Holder h = heap Holder();\n"
        "        h.c = maker(i);\n"                        // the lambda handed out a title: h owns it
        "        return h.c.n;\n", "i", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = the closure's fresh Cell leaked";
}

TEST(StoreOwnershipTests, plainCalleeResultIntoFieldRidesTheFlag) {
    std::string src = loop(
        "        Holder h = heap Holder();\n"
        "        h.c = A.viaPlain(i);\n"                   // the ride-through title: h owns it
        "        return h.c.n;\n", "i", 50);
    EXPECT_EQ(runVerdict(src), 0) << "50 = wrong value; 51 = the ride-through Cell leaked";
}

TEST(StoreOwnershipTests, plainCalleeResultIntoRearmedLocalRidesTheFlag) {
    std::string src = loop(
        "        Cell c = heap Cell(1);\n"
        "        c = A.viaPlain(i);\n"                     // displaced released; ride-through owned
        "        return c.n;\n", "i", 60);
    EXPECT_EQ(runVerdict(src), 0) << "60 = wrong value; 61 = a Cell leaked";
}

TEST(StoreOwnershipTests, closureCallResultIntoTailSlotRidesTheFlag) {
    std::string src = loop(
        "        (int32) -> #Cell maker = (int32 n) -> heap Cell(n);\n"
        "        Cell[] cs = heap Cell[2];\n"
        "        cs[0] = maker(i);\n"                      // slot owns the fresh Cell
        "        return cs[0].n;\n", "i", 70);
    EXPECT_EQ(runVerdict(src), 0) << "70 = wrong value; 71 = the closure's fresh Cell leaked";
}

// ── 5.1.3 — `operator[]=` tenders a fresh value's title ──────────────────

TEST(StoreOwnershipTests, indexAssignOfFreshValueTendersTheTitle) {
    std::string src = loop(
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[i] = heap Cell(i);\n"                   // word bit 1: the map owns it
        "        return m.get(i).n;\n", "i", 80);
    EXPECT_EQ(runVerdict(src), 0) << "80 = wrong value; 81 = the map's Cell leaked (word bit was 0)";
}

// ── 5.1.4 — array literal and element store: lend a bare name, take `#x` ─

TEST(StoreOwnershipTests, arrayLiteralLendsABareOwnedLocal) {
    std::string src = loop(
        "        String out #= A.mkS(i);\n"
        "        int64 before = Cajeta.liveCount();\n"
        "        int32 n = 0;\n"
        "        {\n"
        "            String[] argv = [\"ls\", out];\n"      // slot 1 borrows; out keeps its title
        "            n = argv[1].byteLength() + argv[0].byteLength();\n"
        "        }\n"                                         // argv dropped: a lent slot frees nothing
        "        if (Cajeta.liveCount() != before) { return -90; }\n"   // out still alive
        "        return n + out.byteLength();\n", "(2 * (1 + A.digits(i)) + 2)", 90);
    EXPECT_EQ(runVerdict(src), 0)
        << "90 = wrong value (a freed String read back); 91 = out double-freed or leaked";
}

TEST(StoreOwnershipTests, arrayLiteralTakesOnlyAMovedLocal) {
    std::string src = loop(
        "        String out #= A.mkS(i);\n"
        "        int64 before = Cajeta.liveCount();\n"
        "        int32 n = 0;\n"
        "        {\n"
        "            String[] argv = [\"ls\", #out];\n"     // slot 1 owns; out no longer does
        "            n = argv[1].byteLength();\n"
        "        }\n"                                         // argv dropped: the taken String goes with it
        "        if (Cajeta.liveCount() != before - 1) { return -100; }\n"
        "        return n;\n", "(1 + A.digits(i))", 100);
    EXPECT_EQ(runVerdict(src), 0) << "100 = wrong value; 101 = the moved String was freed twice or leaked";
}

TEST(StoreOwnershipTests, elementStoreLendsABareOwnedLocalAndTakesAMove) {
    std::string src = loop(
        "        Cell a = heap Cell(i);\n"
        "        Cell b = heap Cell(i + 1);\n"
        "        int64 before = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell[] cs = heap Cell[2];\n"
        "            cs[0] = a;\n"                         // borrow: a still owns
        "            cs[1] = #b;\n"                        // transfer: the slot owns
        "            t = cs[0].n + cs[1].n;\n"
        "        }\n"                                         // cs dropped: b freed with it, a untouched
        "        if (Cajeta.liveCount() != before - 1) { return -110; }\n"
        "        return t + a.n;\n", "(3 * i + 1)", 110);
    EXPECT_EQ(runVerdict(src), 0) << "110 = wrong value; 111 = a Cell was freed twice or leaked";
}

TEST(StoreOwnershipTests, arrayLiteralLiteralAndFreshElements) {
    std::string src = loop(
        "        String[] xs = [\"lit\", A.mkS(i)];\n"   // borrow of static; slot-owned fresh
        "        return xs[0].byteLength() + xs[1].byteLength();\n", "(4 + A.digits(i))", 120);
    EXPECT_EQ(runVerdict(src), 0) << "120 = wrong value; 121 = the fresh element leaked or the literal was freed";
}

// ── 5.1.5 — an escaping array with borrowed-local slots is rejected ──────

TEST(StoreOwnershipTests, escapingArrayWithBorrowedLocalSlotsIsRejected) {
    std::string src = std::string(PRE) +
        "    static #Cell[] parts() {\n"
        "        Cell a = heap Cell(1);\n"
        "        Cell b = heap Cell(2);\n"
        "        Cell[] r = [a, b];\n"
        "        return #r;\n"                             // r leaves; its slots borrow a and b
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL");
}

TEST(StoreOwnershipTests, escapingArrayAsOwnedArgumentIsRejected) {
    std::string src = std::string(PRE) +
        "    static void keep(#Cell[] xs) { }\n"
        "    static void probe() {\n"
        "        Cell a = heap Cell(1);\n"
        "        Cell[] r = [a];\n"
        "        A.keep(#r);\n"                            // the callee may retain r past this frame
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL");
}

TEST(StoreOwnershipTests, escapingArrayOfMovedOrFreshElementsIsAccepted) {
    std::string src = std::string(PRE) +
        "    static #Cell[] parts(Cell given) {\n"
        "        Cell a = heap Cell(1);\n"
        "        Cell b = A.viaPlain(2);\n"                           // a runtime owner: the callee's lend is the assertion
        "        Cell[] r = [#a, heap Cell(3), given, b];\n"          // moved, fresh, the caller's, a runtime owner
        "        return #r;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectAccepted(src);
}

// ── 5.1.6 — a `stack` value transferred into a retaining position ────────

TEST(StoreOwnershipTests, stackValueIntoFieldIsRejected) {
    std::string src = std::string(PRE) +
        "    static void probe() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell c = stack Cell(3);\n"
        "        h.c #= c;\n"                              // h outlives the frame; c does not
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_STACK_TRANSFER");
}

TEST(StoreOwnershipTests, stackValueIntoSlotIsRejected) {
    std::string src = std::string(PRE) +
        "    static void probe() {\n"
        "        Cell[] cs = heap Cell[1];\n"
        "        Cell c = stack Cell(3);\n"
        "        cs[0] #= c;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_STACK_TRANSFER");
}

TEST(StoreOwnershipTests, stackValueSameFrameBindAndByValueReturnAreAccepted) {
    std::string src = std::string(PRE) +
        "    static Cell byValue() { return stack Cell(5); }\n"   // sret: lands in the caller's frame
        "    static int32 probe() {\n"
        "        Cell c = stack Cell(3);\n"
        "        Cell d #= c;\n"                            // same frame: not an escape
        "        Cell e = A.byValue();\n"
        "        return d.n + e.n;\n"
        "    }\n"
        "    public static int32 run() { return A.probe() == 8 ? 0 : 1; }\n"
        "}\n";
    expectAccepted(src);
    EXPECT_EQ(runVerdict(src), 0);
}

// 5.1.4: a static owner and a formal placed in a literal by bare name are
// both lends — readable after the array is gone.
TEST(StoreOwnershipTests, arrayLiteralLendsAStaticOwnerAndAFormal) {
    std::string src = loop(
        "        int8[] bs = heap int8[3];\n"
        "        bs[0] = 97; bs[1] = 98; bs[2] = 99;\n"
        "        String own = heap String(#bs, 3);\n"          // static owner (no `#=` after heap)
        "        return A.inner(own, i) + own.byteLength();\n", "(i + 6)", 130);
    src.replace(src.find("    static int32 probe("), 0,
        "    static int32 inner(String given, int32 i) {\n"
        "        int64 before = Cajeta.liveCount();\n"
        "        int32 n = 0;\n"
        "        {\n"
        "            String[] argv = [given, \"x\"];\n"          // the formal, by bare name: a lend
        "            n = argv[0].byteLength();\n"
        "        }\n"
        "        if (Cajeta.liveCount() != before) { return -130; }\n"
        "        return n + i;\n"
        "    }\n");
    EXPECT_EQ(runVerdict(src), 0)
        << "130 = wrong value (a lent String was freed with the array); 131 = a leak or double free";
}
