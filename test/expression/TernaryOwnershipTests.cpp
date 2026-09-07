//
// Ownership of a local bound from a ternary (`T x = c ? a : b`).
//
// Measured 2026-09-07 (cajeta-llm prefill-routes 2.1.1, tmp/probe-emit):
// `String nm = r.name != null ? r.name : "-";` gave the local an ARMED
// drop entry that freed the field's wrapper at scope end, while
// `String nm = r.name;` got none. LocalVariableDeclaration classified an
// initializer as a borrow only for the shapes it recognised (literal,
// identifier, field read, element read, plain-return call); a ternary
// matched none and defaulted to owned. Every diagnostic record under
// cajeta-llm's trace/debug logging double-freed a String through that line,
// and the freed block's reuse showed up as GPU page faults at host
// addresses — a "device race" that was this host miscompile.
//
// The rule these pin: the local owns exactly what the TAKEN arm produced.
// Two borrow arms -> no drop. A heap/concat/`#x` arm -> dropped when taken,
// never when the borrow arm was taken. The verdicts are read back through
// the runtime: a wrongly freed wrapper is poisoned, so the field's bytes no
// longer compare equal; a leaked temporary moves Cajeta.liveCount().
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const char* src, const char* cls) {
    auto jit = CajetaJit::compile(src, cls);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Rec {\n"
    "        public String name;\n"
    "        public Rec() { return; }\n"
    "    }\n"
    "    static class Cell {\n"
    "        public int32 v;\n"
    "        public Cell(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell a;\n"
    "        public Cell b;\n"
    "        public Holder() {\n"
    "            this.a #= heap Cell(1);\n"
    "            this.b #= heap Cell(2);\n"
    "            return;\n"
    "        }\n"
    "    }\n";

} // namespace

// A String local from a ternary whose BOTH arms are borrows (a field read
// and a literal) must not free the field. This is the measured bug.
TEST(TernaryOwnershipTests, stringLocalFromBorrowArmsDoesNotFreeTheField) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        String nm = c ? r.name : \"-\";\n"
        "        return nm.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            A.probe(r, true);\n"
        "            A.probe(r, false);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!r.name.equals(\"hello1\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "1 = the field's bytes no longer read back: the local dropped a borrow";
}

// Mixed arms: the concat is dropped exactly when it was taken (no leak),
// and the field is never freed when the borrow arm was taken.
TEST(TernaryOwnershipTests, stringLocalFromMixedArmsDropsOnlyWhatItOwns) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        String s = c ? (\"x\" + 7) : r.name;\n"
        "        return s.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { A.probe(r, true); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0) { return 2; }\n"
        "        i = 0;\n"
        "        while (i < 64) { A.probe(r, false); i = i + 1; }\n"
        "        if (!r.name.equals(\"hello1\")) { return 3; }\n"
        "        if (Cajeta.liveCount() != l0) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "2 = the taken concat leaked; 3 = the borrow arm freed the field; "
           "4 = live count moved on the borrow arm";
}

// A class-typed local from two field-read arms must not free either object.
TEST(TernaryOwnershipTests, classLocalFromBorrowArmsDoesNotFreeEither) {
    std::string src = std::string(PRE) +
        "    static int32 pick(Holder h, boolean c) {\n"
        "        Cell k = c ? h.a : h.b;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            A.pick(h, true);\n"
        "            A.pick(h, false);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 5; }\n"
        "        if (h.b.v != 2) { return 6; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "5/6 = a field object no longer reads back: the local dropped a borrow";
}

// Mixed class arms: the heap object is dropped when taken, the field never.
TEST(TernaryOwnershipTests, classLocalFromMixedArmsDropsOnlyWhatItOwns) {
    std::string src = std::string(PRE) +
        "    static int32 pick(Holder h, boolean c) {\n"
        "        Cell k = c ? heap Cell(9) : h.a;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { A.pick(h, true); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0) { return 7; }\n"
        "        i = 0;\n"
        "        while (i < 64) { A.pick(h, false); i = i + 1; }\n"
        "        if (h.a.v != 1) { return 8; }\n"
        "        if (Cajeta.liveCount() != l0) { return 9; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "7 = the taken heap object leaked; 8 = the borrow arm freed the field; "
           "9 = live count moved on the borrow arm";
}

// ---------------------------------------------------------------------------
// The OTHER consumers (2026-09-07, probe tmp/probe-emit/src/probe/W.cajeta).
//
// Each position that takes an expression's value has its own shape
// classifier, and the conditional was missing from every one of them: the
// `#=` store treated the merge phi as a static transfer (a borrowed wrapper
// stored raw with the own-bit set, freed twice), a `#` return stored a
// constant 1 over a borrow arm (a forged title), a call argument never
// reclaimed a fresh String arm nor put a fresh class arm's title in the
// transfer word (leaks), and an assignment's re-arm matched neither
// spelling. These pin each position to the rule the local-binding fix set:
// the consumer does for the conditional exactly what it does for the TAKEN
// arm written alone.
// ---------------------------------------------------------------------------

#include "cajeta/error/Exception.h"

namespace {

const char* PRE2 =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Rec {\n"
    "        public String name;\n"
    "        public String other;\n"
    "        public Rec() { return; }\n"
    "    }\n"
    "    static class Cell {\n"
    "        public int32 v;\n"
    "        public Cell(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell a;\n"
    "        public Cell b;\n"
    "        public Cell c;\n"
    "        public Holder() {\n"
    "            this.a #= heap Cell(1);\n"
    "            this.b #= heap Cell(2);\n"
    "            return;\n"
    "        }\n"
    "    }\n";

void expectRejected(const std::string& src, const std::string& code) {
    try {
        CajetaJit::compile(src.c_str(), "test.A");
        ADD_FAILURE() << "expected " << code;
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code)
            << "wrong diagnostic: " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

} // namespace

// `r.other #= c ? r.name : "-"`: both arms are borrows, so the field must
// RESOLVE its own wrapper (a copy / static alias), never adopt r.name's. The
// overwrite that follows frees whatever the field holds — with the bug that
// was r.name's wrapper.
TEST(TernaryOwnershipTests, sharpStoreOfBorrowArmsResolvesTheString) {
    std::string src = std::string(PRE2) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        r.other #= c ? r.name : \"-\";\n"
        "        r.other #= \"fresh\" + 1;\n"
        "        return r.other.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            A.probe(r, true);\n"
        "            A.probe(r, false);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!r.name.equals(\"hello1\")) { return 1; }\n"
        "        if (Cajeta.liveCount() != l0 + 1) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "1 = the field's wrapper was freed through the alias; "
           "2 = live count off (leak or double free of the stored wrapper)";
}

// Mixed arms into a `#=` field store: the concat transfers as-is when
// taken, the field read resolves a copy when taken; r.name survives both.
TEST(TernaryOwnershipTests, sharpStoreOfMixedArmsTakesOnlyTheFreshString) {
    std::string src = std::string(PRE2) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        r.other #= c ? (\"x\" + 7) : r.name;\n"
        "        return r.other.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { A.probe(r, true); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0 + 1) { return 3; }\n"
        "        i = 0;\n"
        "        while (i < 64) { A.probe(r, false); i = i + 1; }\n"
        "        if (!r.name.equals(\"hello1\")) { return 4; }\n"
        "        if (Cajeta.liveCount() != l0 + 1) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "3 = the displaced concat leaked; 4 = the borrow arm was adopted "
           "and freed; 5 = live count off after the borrow arm";
}

// A class field `#=` from two borrow arms records a BORROW (own-bit clear):
// when the holder drops, `a` and `b` are freed once each and `c` not at all.
TEST(TernaryOwnershipTests, sharpStoreOfBorrowArmsClassRecordsABorrow) {
    std::string src = std::string(PRE2) +
        "    static int32 build(boolean c) {\n"
        "        Holder h = heap Holder();\n"
        "        h.c #= c ? h.a : h.b;\n"
        "        return h.c.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        if (A.build(true) != 1) { return 6; }\n"
        "        if (Cajeta.liveCount() != l0) { return 7; }\n"
        "        if (A.build(false) != 2) { return 8; }\n"
        "        if (Cajeta.liveCount() != l0) { return 9; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "6/8 = wrong arm stored; 7/9 = live count moved after the holder "
           "dropped (the aliased cell was freed twice, or something leaked)";
}

// Mixed class arms: the heap cell is owned (and released when displaced or
// when the holder drops); the field-read arm is a borrow.
TEST(TernaryOwnershipTests, sharpStoreOfMixedArmsClassOwnsOnlyTheFreshCell) {
    std::string src = std::string(PRE2) +
        "    static int32 build(boolean c) {\n"
        "        Holder h = heap Holder();\n"
        "        h.c #= c ? heap Cell(3) : h.a;\n"
        "        h.c #= c ? heap Cell(3) : h.a;\n"
        "        return h.c.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        if (A.build(true) != 3) { return 10; }\n"
        "        if (Cajeta.liveCount() != l0) { return 11; }\n"
        "        if (A.build(false) != 1) { return 12; }\n"
        "        if (Cajeta.liveCount() != l0) { return 13; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "10/12 = wrong arm stored; 11 = a heap cell leaked (displaced or "
           "at drop); 13 = the borrowed cell was freed";
}

// `String s #= c ? ("x" + 7) : r.name` — the `#=` declaration is the same
// mode-carrying wrapper as the field store; it owns only the fresh arm.
TEST(TernaryOwnershipTests, sharpDeclarationFromMixedArmsOwnsOnlyTheFreshString) {
    std::string src = std::string(PRE2) +
        "    static int32 probe(Rec r, boolean c) {\n"
        "        String s #= c ? (\"x\" + 7) : r.name;\n"
        "        return s.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { A.probe(r, true); i = i + 1; }\n"
        "        if (Cajeta.liveCount() != l0) { return 14; }\n"
        "        i = 0;\n"
        "        while (i < 64) { A.probe(r, false); i = i + 1; }\n"
        "        if (!r.name.equals(\"hello1\")) { return 15; }\n"
        "        if (Cajeta.liveCount() != l0) { return 16; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "14 = the concat leaked; 15 = the borrow arm freed the field; "
           "16 = live count moved on the borrow arm";
}

// A `#` return promises a title on EVERY arm: a field-read arm is rejected
// at compile time, as a bare `return this.f` under `#` is.
TEST(TernaryOwnershipTests, ownedReturnOfFieldReadArmIsRejected) {
    std::string src = std::string(PRE2) +
        "    static #String pick(Rec r, boolean c) {\n"
        "        return c ? r.name : \"-\";\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

// A bare local arm holds no title either (the frame's drop runs before the
// `ret`): rejected, with `#s` as the prescribed spelling.
TEST(TernaryOwnershipTests, ownedReturnOfBareLocalArmIsRejected) {
    std::string src = std::string(PRE2) +
        "    static #Cell pick(boolean c) {\n"
        "        Cell s = heap Cell(1);\n"
        "        Cell t = heap Cell(2);\n"
        "        return c ? s : t;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_OWNED_RETURN_OF_BORROW");
}

// Owned arms under a `#` return: the fresh cell rides out when taken; the
// surrendered local rides out when taken and is dropped by the frame when
// not. Balanced either way.
TEST(TernaryOwnershipTests, ownedReturnOfOwnedArmsCarriesTheTakenTitle) {
    std::string src = std::string(PRE2) +
        "    static #Cell pick(boolean c) {\n"
        "        Cell k = heap Cell(6);\n"
        "        return c ? heap Cell(5) : #k;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            Cell x #= A.pick(true);\n"
        "            if (x.v != 5) { return 17; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 18; }\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            Cell y #= A.pick(false);\n"
        "            if (y.v != 6) { return 19; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 20; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "17/19 = wrong arm returned; 18 = the un-taken local leaked or the "
           "fresh cell was not received; 20 = live count off after `#k`";
}

// A PLAIN return holds every arm to the fresh-return rule: a `heap` arm
// would leak (the caller registers no drop), so it is rejected.
TEST(TernaryOwnershipTests, plainReturnOfFreshArmIsRejected) {
    std::string src = std::string(PRE2) +
        "    static Cell pick(Holder h) {\n"
        "        return h.a.v > 0 ? heap Cell(1) : h.a;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER");
}

// A plain return of (call, field read) rides the callee's flag through the
// taken arm — the tail-call rule (ownership §2.1) applied per arm. The
// receiving local arms its drop from that flag: the `#Cell` result is
// freed each iteration, the borrowed field never.
TEST(TernaryOwnershipTests, plainReturnOfCallArmRidesTheCalleeFlag) {
    std::string src = std::string(PRE2) +
        "    static #Cell fresh() { return heap Cell(9); }\n"
        "    static Cell pick(Holder h) {\n"
        "        return h.a.v == 1 ? A.fresh() : h.b;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            Cell x = A.pick(h);\n"
        "            if (x.v != 9) { return 21; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 22; }\n"
        "        h.a.v = 0;\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            Cell y = A.pick(h);\n"
        "            if (y.v != 2) { return 23; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.b.v != 2) { return 24; }\n"
        "        if (Cajeta.liveCount() != l0) { return 25; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "21/23 = wrong arm; 22 = the fresh cells leaked (flag not ridden); "
           "24 = the borrowed field was freed; 25 = live count off";
}

// A String argument from mixed arms: the concat is reclaimed after the call
// exactly when it was the taken arm; the field read is never touched.
TEST(TernaryOwnershipTests, stringArgFromMixedArmsIsReclaimedOnlyWhenOwned) {
    std::string src = std::string(PRE2) +
        "    static int32 sink(String s) { return s.byteLength(); }\n"
        "    static int32 probe(Rec r, boolean c) {\n"
        "        return A.sink(c ? (\"x\" + 7) : r.name);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Rec r = heap Rec();\n"
        "        r.name #= \"hello\" + 1;\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(r, true) != 2) { return 26; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 27; }\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(r, false) != 6) { return 28; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!r.name.equals(\"hello1\")) { return 29; }\n"
        "        if (Cajeta.liveCount() != l0) { return 30; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "26/28 = wrong arm passed; 27 = the concat leaked; 29 = the field "
           "was reclaimed; 30 = live count off on the borrow arm";
}

// A class argument from mixed arms: the fresh cell's title rides the
// transfer word to the callee's formal (freed there); the field read lends.
TEST(TernaryOwnershipTests, classArgFromMixedArmsHandsTheTitleThrough) {
    std::string src = std::string(PRE2) +
        "    static int32 sinkCell(Cell c) { return c.v; }\n"
        "    static int32 probe(Holder h, boolean c) {\n"
        "        return A.sinkCell(c ? heap Cell(8) : h.a);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h, true) != 8) { return 31; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 32; }\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h, false) != 1) { return 33; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 34; }\n"
        "        if (Cajeta.liveCount() != l0) { return 35; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "31/33 = wrong arm passed; 32 = the fresh cells leaked (title not "
           "in the word); 34 = the lent field was freed; 35 = live count off";
}

// A `#T` formal is a promise the argument must keep on every arm: a
// field-read arm is rejected as a bare field-read argument is.
TEST(TernaryOwnershipTests, sharpFormalRejectsABorrowArm) {
    std::string src = std::string(PRE2) +
        "    static void take(#Cell c) { return; }\n"
        "    static void probe(Holder h, boolean c) {\n"
        "        A.take(c ? heap Cell(1) : h.a);\n"
        "        return;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectRejected(src, "CAJETA_ERROR_TRANSFER_REQUIRED");
}

// Re-assigning a moved-out binding from mixed arms re-arms its entry from
// the taken arm: the fresh cell is dropped at scope exit, the lent field
// never. (A never-moved binding keeps its entry on the displaced value —
// the documented reassign family — unchanged here.)
TEST(TernaryOwnershipTests, reassignFromMixedArmsRearmsTheMovedBinding) {
    std::string src = std::string(PRE2) +
        "    static void take(#Cell c) { return; }\n"
        "    static int32 probe(Holder h, boolean c) {\n"
        "        Cell k = heap Cell(1);\n"
        "        A.take(#k);\n"
        "        k = c ? heap Cell(6) : h.a;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h, true) != 6) { return 36; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 37; }\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h, false) != 1) { return 38; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 39; }\n"
        "        if (Cajeta.liveCount() != l0) { return 40; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "36/38 = wrong arm bound; 37 = the fresh cell leaked (entry not "
           "re-armed); 39 = the lent field was freed; 40 = live count off";
}
