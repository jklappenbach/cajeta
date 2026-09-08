//
// ownership-title-classifier Unit 7 — call arguments on the classifier
// (MethodCallExpression: the argument stash, the `#x` flags, the `#T` formal
// check, the transfer-word composition, the after-call reclaim, the receiver
// temp; ClassCreatorRest: the same for constructor arguments). Written RED,
// before the sites migrate; the pins guard what must not move.
//
//   7.1.2  spec 5.8 — a bare local holding a proven borrow (no entry) passed
//          to a `#T` formal is rejected; an owned local surrendered with `#k`
//          and an inactive-entry local (a later assignment may arm it) pass
//   7.1.3  spec 5.11 — a `stack` value transferred into a `#T` formal (a
//          method's and a constructor's) is rejected
//   7.1.4  a `#R` callee that carries a BORROW out (`return #= x`) passed to
//          a plain formal must hand the callee the runtime bit, not the
//          constant 1 the fresh-call rule assumes (double free today)
//   7.1.5  pins — an owned call result to a plain formal moves (balanced), a
//          concatenation and an owned String call to a plain String formal
//          are reclaimed after the call, a field read to a `#T` formal is
//          rejected, and the constructor twins of each
//

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
    "    static class Keeper {\n"
    "        public Cell c;\n"
    "        public int32 len;\n"
    "        public Keeper() { }\n"
    "        public Keeper(#Cell v) { this.c #= v; }\n"           // a `#T` constructor formal
    "        public void take(#Cell v) { this.c #= v; }\n"        // a `#T` method formal
    "        public void lend(Cell v) { this.c #= v; }\n"         // a plain formal: the sink model, the caller chooses
    "        public void keepS(String t) { this.len = t.byteLength(); }\n"   // plain String formal: reads only
    "    }\n"
    "    static #Cell mk(int32 n) { return heap Cell(n); }\n"
    "    static #String mkS(int32 n) { return \"v\" + n; }\n"
    "    static Cell viaPlain(int32 n) { return A.mk(n); }\n"
    "    static #Cell relay(Bank b) { Cell x = b.get(); return #= x; }\n"   // `#R`, carries a BORROW out
    "    static int32 use(Cell c) { return c.n; }\n"                         // plain formal, reads only
    "    static int32 useS(String s) { return s.byteLength(); }\n";

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

// ── 7.1.2 — spec 5.8: a proven-borrow local to a `#T` formal ──────────────

TEST(CallArgOwnershipTests, entrylessBorrowLocalToOwnedFormalRejected) {
    expectRejected(rejectSrc(
        "    static void f(Cell p) {\n"
        "        Cell k = p;\n"                          // a lend: no entry, no title
        "        Keeper h = heap Keeper();\n"
        "        h.take(k);\n"                           // `#T` formal: the callee would store a borrow
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

TEST(CallArgOwnershipTests, entrylessBorrowLocalToOwnedCtorFormalRejected) {
    expectRejected(rejectSrc(
        "    static void f(Cell p) {\n"
        "        Cell k = p;\n"
        "        Keeper h = heap Keeper(k);\n"          // the constructor twin
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

TEST(CallArgOwnershipTests, ownedLocalSurrenderedToOwnedFormalPasses) {
    std::string src = loop("",
        "        Cell k = heap Cell(i);\n"
        "        Keeper h = heap Keeper();\n"
        "        h.take(#k);\n"                          // the title moves to the keeper
        "        return h.c.n;\n", "i", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = leaked or freed twice";
}

TEST(CallArgOwnershipTests, inactiveEntryLocalSurrenderedForwardsItsFlag) {
    // Spec 5.8's does-not-fire: a borrow-initialised local gets an INACTIVE
    // entry so a later assignment can arm it; `#k` forwards whatever the
    // entry says on each path (1 after `k = heap`, 0 on the borrow path).
    std::string src = loop("",
        "        Cell p = heap Cell(i + 100);\n"
        "        Cell k = p;\n"                          // a borrow: inactive entry
        "        if (i % 2 == 0) { k = heap Cell(i); }\n"   // armed on even i
        "        Keeper h = heap Keeper();\n"
        "        h.take(#k);\n"                          // forwards the entry's flag
        "        if (i % 2 == 0) { return h.c.n; }\n"
        "        return h.c.n - 100;\n", "i", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = the even path leaked or the odd path double-freed";
}

// FOUND writing the test above (MEASURED 2026-09-08, verdict 21 — live count
// moved): an OWNED local re-assigned to a borrow keeps its entry (the
// displaced value lives to scope exit, so lends of it stay valid), but the
// NAME now holds a borrow — and `#k` forwards the entry's title for the
// displaced value, handing the keeper a title on `p`'s cell. Demoting the
// name at the re-assign was tried and reverted: the scope's move marking is
// flow-insensitive, so a borrow re-assign in one `if` arm made every later
// `#=` of the name a rejection, which §7.2 forbids (what the analysis
// cannot prove is ALLOWED — CapturedBorrowParamTests.unprovableCapture-
// IsAllowed pins that line). The precise fix is flow-sensitive name state;
// filed in the plan's 8.2.2. DISABLED: it fails by design until then.
TEST(CallArgOwnershipTests, DISABLED_ownerReassignedToBorrowThenSurrenderedWitness) {
    std::string src = loop("",
        "        Cell p = heap Cell(i + 100);\n"
        "        Cell k = A.viaPlain(i);\n"              // flagged entry, 1 at run time
        "        if (i % 2 == 0) { k = p; }\n"           // k now names a borrow on even i
        "        Keeper h = heap Keeper();\n"
        "        h.take(#k);\n"                          // forwards the entry's title: p's cell on even i
        "        if (i % 2 == 0) { return h.c.n - 100; }\n"
        "        return h.c.n;\n", "i", 25);
    EXPECT_EQ(runVerdict(src), 0) << "25 = wrong value; 26 = p's cell was freed twice or the displaced cell leaked";
}

// ── 7.1.3 — spec 5.11: a `stack` value into a `#T` formal ─────────────────

TEST(CallArgOwnershipTests, stackLocalToOwnedFormalRejected) {
    expectRejected(rejectSrc(
        "    static void f() {\n"
        "        Cell s = stack Cell(1);\n"
        "        Keeper h = heap Keeper();\n"
        "        h.take(#s);\n"
        "    }\n"), "CAJETA_ERROR_STACK_TRANSFER");
}

TEST(CallArgOwnershipTests, stackLocalToOwnedCtorFormalRejected) {
    expectRejected(rejectSrc(
        "    static #Keeper f() {\n"
        "        Cell s = stack Cell(5);\n"
        "        Keeper h = heap Keeper(#s);\n"         // TransferOfBorrowTests' witness, the ctor twin
        "        return #h;\n"
        "    }\n"), "CAJETA_ERROR_STACK_TRANSFER");
}

// ── 7.1.4 — a `#R` callee carrying a borrow out, into a plain formal ──────

TEST(CallArgOwnershipTests, modeCarryingOwnedCalleeIntoPlainFormalRidesTheBit) {
    std::string src = loop("",
        "        Bank b = heap Bank(A.mk(i));\n"
        "        Keeper h = heap Keeper();\n"
        "        h.lend(A.relay(b));\n"                  // relay hands back b's cell with flag 0
        "        return h.c.n;\n", "i", 30);              // the keeper must NOT own it
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value; 31 = the bank's cell was freed twice (constant word bit)";
}

// ── 7.1.5 — pins ──────────────────────────────────────────────────────────

TEST(CallArgOwnershipTests, ownedCallResultToPlainFormalMoves) {
    std::string src = loop("",
        "        Keeper h = heap Keeper();\n"
        "        h.lend(A.mk(i));\n"                     // the fresh cell's title moves to the plain formal
        "        return h.c.n;\n", "i", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = the fresh cell leaked or double-freed";
}

TEST(CallArgOwnershipTests, concatToPlainStringFormalIsReclaimed) {
    std::string src = loop("",
        "        Keeper h = heap Keeper();\n"
        "        h.keepS(\"v\" + i);\n"                  // the callee reads; the temp is dropped after
        "        int32 d = i < 10 ? 2 : 3;\n"            // byte length of \"v\" + i
        "        return A.useS(\"w\" + i) + h.len - 2 * d;\n", "0", 50);
    EXPECT_EQ(runVerdict(src), 0) << "50 = wrong value; 51 = a concatenation temp leaked";
}

TEST(CallArgOwnershipTests, ownedStringCallToPlainStringFormalIsReclaimed) {
    std::string src = loop("",
        "        Keeper h = heap Keeper();\n"
        "        h.keepS(A.mkS(i));\n"
        "        int32 d = i < 10 ? 2 : 3;\n"            // byte length of mkS(i)
        "        return A.useS(A.mkS(i)) + h.len - 2 * d;\n", "0", 60);
    EXPECT_EQ(runVerdict(src), 0) << "60 = wrong value; 61 = an owned String temp leaked";
}

TEST(CallArgOwnershipTests, fieldReadToOwnedFormalRejected) {
    expectRejected(rejectSrc(
        "    static void f(Bank b) {\n"
        "        Keeper h = heap Keeper();\n"
        "        h.take(b.c);\n"
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

TEST(CallArgOwnershipTests, fieldReadToOwnedCtorFormalRejected) {
    expectRejected(rejectSrc(
        "    static void f(Bank b) {\n"
        "        Keeper h = heap Keeper(b.c);\n"
        "    }\n"), "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// The two stdlib idioms the `#T`-formal row keeps (measured 2026-09-08 on
// the stdlib compile): a String literal into a `#String` constructor formal
// (`heap SomeException("...")` — the literal's static wrapper is adopted, its
// drop a no-op) and a stage adopting its receiver (`heap FilterStream<T>(this,
// pred)` inside `filter()`: the `#Stream source` formal adopts `this`; the
// language has no `#this` spelling to say so).
TEST(CallArgOwnershipTests, stringLiteralToOwnedCtorFormalIsAdopted) {
    std::string src = loop(
        "    static class Msg {\n"
        "        public String m;\n"
        "        public Msg(#String m) { this.m #= m; }\n"
        "    }\n",
        "        Msg g = heap Msg(\"literal\");\n"
        "        return g.m.byteLength();\n", "7", 80);
    EXPECT_EQ(runVerdict(src), 0) << "80 = wrong value; 81 = the literal's wrapper was freed or leaked";
}

TEST(CallArgOwnershipTests, receiverIntoOwnedCtorFormalIsTheConsumedReceiverIdiom) {
    std::string src = loop(
        "    static class Stage {\n"
        "        public Stage src;\n"
        "        public int32 n;\n"
        "        public Stage(int32 n) { this.n = n; }\n"
        "        public Stage(#Stage s, int32 n) { this.src #= s; this.n = n; }\n"
        "        public #Stage then(int32 n) { return heap Stage(this, n); }\n"   // adopts `this`
        "        public int32 depth() { if (this.src == null) { return 1; } return 1 + this.src.depth(); }\n"
        "    }\n",
        "        Stage head = heap Stage(i);\n"
        "        Stage tail #= head.then(i + 1);\n"      // tail owns head now; head's own drop is a no-op
        "        return tail.depth();\n", "2", 90);
    EXPECT_EQ(runVerdict(src), 0) << "90 = wrong value; 91 = the chain leaked or was freed twice";
}

TEST(CallArgOwnershipTests, ownedCallResultToOwnedCtorFormalMoves) {
    std::string src = loop("",
        "        Keeper h = heap Keeper(A.mk(i));\n"    // the constructor twin of the move
        "        return h.c.n;\n", "i", 70);
    EXPECT_EQ(runVerdict(src), 0) << "70 = wrong value; 71 = the fresh cell leaked or double-freed";
}
