// stdlib-ownership-convention 8.2.7 (spec §4.6) —
// `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`: a `#T` result must be received
// with `#=`.
//
// The CALLER half of §2.8's return-side redesign, and the half that carries
// the volume. 8.2.6 put the callee under contract and measured ZERO existing
// violations — a callee-side rule is checked where the promise is made, and
// almost nothing violates it. This one lands on every USE, and uses
// concentrate: 148 sites across three libraries, 38 of them `String.toBytes`.
//
// What it buys is legibility, which §2.8 treats as the point rather than a
// bonus:
//
//     int8[] w = s.toBytes();     // `#int8[]` — w is yours to free
//     int8[] w = s.trimView();    // plain     — the String still owns it
//
// Two lines a reader cannot tell apart, and opposite facts about who frees
// `w`. `#=` on the first is what tells them apart without opening the callee.
//
// It lands as a WARNING under spec §5.5 (`CAJETA_OWNED_BIND=warn`), because
// 148 is not a number an error can ship against — 3.3.3's lesson, paid for
// once already by Unit 3's gate.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"
#include "cajeta/ownership/OwnedBindCheck.h"

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "public final class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 v) { this.n = v; }\n"
    "}\n"
    "public final class Bank {\n"
    "    Cell c;\n"
    "    public Bank(#Cell v) { this.c #= v; }\n"
    "    public Cell lend() { Cell v = this.c; return v; }\n"   // plain: borrow
    "    public static #Cell mint(int32 v) { return heap Cell(v); }\n"
    "}\n";

// Restores the process-wide §5.5 switch however the test leaves. Without it a
// warn-mode test would silently disarm every rejection test in the file — the
// failure mode being tests that pass while checking nothing.
struct WarnMode {
    WarnMode() { cajeta::ownership::setOwnedBindWarns(true); }
    ~WarnMode() { cajeta::ownership::clearOwnedBindWarnsOverride(); }
};

void expectRejected(const std::string& src, const std::string& code) {
    try {
        CajetaJit::compile(src, "test.D");
        ADD_FAILURE() << "expected " << code;
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code) << "wrong diagnostic: " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

void expectOk(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.getErrorId()
                      << ": " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.what();
    }
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// The rule itself: `#`-returning callee, plain `=` at the lvalue.
TEST(OwnedResultTransferTests, plainBindOfOwnedResultRejected) {
    expectRejected(std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell c = Bank.mint(7);\n"
        "        return c.n;\n"
        "    }\n"
        "}\n", "CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER");
}

// The prescribed fix compiles and runs — a check whose fix does not work is
// worse than no check.
TEST(OwnedResultTransferTests, sharpAssignBindOfOwnedResultCompilesAndRuns) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell c #= Bank.mint(7);\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    expectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// THE CONTROL THAT MATTERS MOST. A plain-returning callee bound with plain `=`
// is the overwhelmingly common shape in the language and must stay untouched;
// if this breaks, everything breaks. It is also the line the rule exists to
// distinguish from the one above.
TEST(OwnedResultTransferTests, plainBindOfBorrowResultStillCompiles) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bank b #= heap Bank(#heap Cell(7));\n"
        "        Cell c = b.lend();\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    expectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// Spec §5.5's switch: warn mode admits the site and keeps compiling, so one
// build enumerates all 148 instead of one per rebuild.
TEST(OwnedResultTransferTests, warnModeAdmitsThePlainBind) {
    WarnMode warn;
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell c = Bank.mint(7);\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    expectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// ERROR IS THE DEFAULT, and stays so after a warn-mode test has run. The other
// half of the flip: 8.2.7 closes only when this check throws again, and a
// suite that could not tell the two apart could not certify it.
TEST(OwnedResultTransferTests, defaultModeStillThrows) {
    EXPECT_FALSE(cajeta::ownership::ownedBindWarns());
    expectRejected(std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell c = Bank.mint(7);\n"
        "        return c.n;\n"
        "    }\n"
        "}\n", "CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER");
}

// ---------------------------------------------------------------------------
// A MEASUREMENT, not a specification — and it decides how 148 diagnostics get
// worded.
//
// §4.6 is stated as a LEGIBILITY rule: the reader should see where title
// moves. That framing is only honest if a plain `=` bind is otherwise
// harmless. If it instead drops the title on the floor, the rule is a leak fix
// and the migration is urgent rather than cosmetic. Reading the codegen would
// not settle it — that is exactly how 8.2.5 was filed on a leak that did not
// exist — so measure it.
//
// `Cajeta.liveCount()` around a scope that mints and binds. Balanced (0) means
// legibility-only. POSITIVE means the plain bind leaks. NEGATIVE means it
// double-frees.
//
// Class-typed on purpose: liveCount is blind to arrays
// (OwnershipRuntimeProbeTests.instrumentIsBlindToArrays), so it cannot speak
// to `String.toBytes`, which is 38 of the 148. That gap is real and named
// rather than papered over.
//
// Returns leaked*100 + value; 7 == balanced.
TEST(OwnedResultTransferTests, whatAPlainBindOfAnOwnedResultActuallyDoes) {
    WarnMode warn;  // the shape under measurement is the one the rule rejects
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    static int32 work() {\n"
        "        Cell c = Bank.mint(7);\n"
        "        return c.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    int32_t got = runI32(src);
    EXPECT_EQ(got, 7) << "plain `=` bind of a `#` result: leaked="
                      << ((got - 7) / 100)
                      << " (0 balanced -> §4.6 is legibility only, "
                         ">0 leak -> it is a leak fix, <0 double free)";
}

// The `#=` control for the probe above: the prescribed fix must at least be no
// worse than what it replaces.
TEST(OwnedResultTransferTests, sharpAssignBindIsBalanced) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    static int32 work() {\n"
        "        Cell c #= Bank.mint(7);\n"
        "        return c.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = D.work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    int32_t got = runI32(src);
    EXPECT_EQ(got, 7) << "`#=` bind: leaked=" << ((got - 7) / 100);
}
