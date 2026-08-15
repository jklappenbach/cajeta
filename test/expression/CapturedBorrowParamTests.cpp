// stdlib-ownership-convention Unit 3 (spec 2.3, 2.4, 3.2, 4.2, 4.3, 7.2) —
// CAJETA_ERROR_CAPTURED_BORROW_PARAM.
//
// The other half of the convention. Unit 2 made `#x` on a borrow an error
// (the CALLER lying about what it holds); this makes a quietly-captured
// plain parameter an error (the CALLEE lying about what it keeps).
//
// Why it matters, from the spec's own origin: `setString(String)` stores
// its argument's buffer beyond the call while declaring a plain parameter,
// so every call site reads as a borrow and some are catastrophic. §2.4
// says a non-sink API that keeps a parameter spells it `#T`; this is the
// enforcement that makes a plain parameter MEAN "not kept" rather than
// merely suggest it.
//
// The sink exemption is the load-bearing subtlety. `ArrayList.add(T v)`
// with `this.slot #= v` is CORRECT and must keep compiling: `#=` records
// the source's mode per slot, so `add(v)` lends and `add(#v)` transfers
// and the developer chooses (§2.3). The opt-out is therefore keyed on the
// STORE FORM (`#=`), not on an annotation — which is what §7.3 closed.
//
// Measured context for the array cases: `#x` on a borrow does not
// transfer, and a receiver that outlives its lender reads reused memory
// (OwnershipArrayCanaryTests). A captured borrow is the same defect
// reached from the callee side.
//
// RED until 3.2.1.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"
#include "cajeta/type/Scope.h"

using cajeta_test::CajetaJit;

namespace {

// A payload with an observable value, plus a sink and a non-sink holder.
const char* kSrc =
    "package test;\n"
    "public final class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 v) { this.n = v; }\n"
    "}\n";

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode)
            << "wrong diagnostic: " << e.getMessage();
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

void compileExpectOk(const std::string& src) {
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

// 3.1.1 — storing a plain parameter into a FIELD is rejected, and the
// message names `#T` as the fix (§4.2) and both ends (§4.3).
TEST(CapturedBorrowParamTests, plainParamStoredIntoFieldRejected) {
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell held;\n"
        "    public Keeper() { this.held = null; }\n"
        "    public void keep(Cell c) { this.held = c; }\n"   // captured
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Keeper k = heap Keeper();\n"
        "        k.keep(heap Cell(7));\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    std::string msg = compileExpectError(src,
        "CAJETA_ERROR_CAPTURED_BORROW_PARAM");
    EXPECT_NE(msg.find("`c`"), std::string::npos)
        << "message should name the parameter: " << msg;
    // The fix is named with the CANONICAL type (`#test.Cell`), not the bare
    // spelling — assert the marker plus the type name rather than a literal
    // `#Cell`, which was this test's own bug on first run.
    EXPECT_NE(msg.find("`#test.Cell`"), std::string::npos)
        << "message should name `#T` as the fix: " << msg;
}

// 3.1.2 — storing into an ARRAY ELEMENT is the same capture.
TEST(CapturedBorrowParamTests, plainParamStoredIntoElementRejected) {
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell[] slots;\n"
        "    public Keeper() { this.slots = heap Cell[4]; }\n"
        "    public void put(int32 i, Cell c) { this.slots[i] = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_CAPTURED_BORROW_PARAM");
}

// 3.1.3 — THE SINK MODEL MUST SURVIVE (§2.3). A plain parameter stored
// with `#=` is the ArrayList contract: caller's choice, mode recorded per
// slot. This is the single most important non-rejection in the unit — if
// it breaks, every collection in the stdlib breaks with it.
TEST(CapturedBorrowParamTests, sinkStoringWithSharpAssignStillCompiles) {
    std::string src = std::string(kSrc) +
        "public final class Bag {\n"
        "    Cell held;\n"
        "    public Bag() { this.held = null; }\n"
        "    public void add(Cell c) { this.held #= c; }\n"   // sink
        "    public int32 value() { return this.held.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bag b = heap Bag();\n"
        "        b.add(#heap Cell(7));\n"
        "        return b.value();\n"
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// 3.1.4 — a `#T` parameter stored anywhere is correct and compiles: the
// signature already told the caller.
TEST(CapturedBorrowParamTests, sharpParamStoredStillCompiles) {
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell held;\n"
        "    public Keeper() { this.held = null; }\n"
        "    public void keep(#Cell c) { this.held #= c; }\n"
        "    public int32 value() { return this.held.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Keeper k = heap Keeper();\n"
        "        k.keep(#heap Cell(7));\n"
        "        return k.value();\n"
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// 3.1.5 — spec §7.2's straight-line case: the parameter reaches the field
// through an intermediate local. Same capture, one hop later.
//
// This is also what the two DISABLED OwnershipArrayCanaryTests need: they
// corrupt through exactly this shape (`T v = this.data; return v;`), and
// should be re-enabled when straight-line tracking lands.
TEST(CapturedBorrowParamTests, captureThroughStraightLineLocalRejected) {
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell held;\n"
        "    public Keeper() { this.held = null; }\n"
        "    public void keep(Cell c) {\n"
        "        Cell v = c;\n"          // one hop
        "        this.held = v;\n"       // ...still a capture
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_CAPTURED_BORROW_PARAM");
}

// 3.1.6 — THE CHECK MUST NEVER BLOCK VALID CODE (§7.2). What the analysis
// cannot prove is ALLOWED, not rejected. Here the stored value is chosen
// at run time between the parameter and a fresh allocation, so no
// straight-line chain proves a capture.
//
// Deliberately paired with 3.1.5: together they pin where the line sits,
// so a later tightening cannot quietly turn "unprovable" into "rejected".
TEST(CapturedBorrowParamTests, unprovableCaptureIsAllowed) {
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell held;\n"
        "    public Keeper() { this.held = null; }\n"
        "    public void keep(Cell c, boolean useIt) {\n"
        "        Cell v = heap Cell(1);\n"
        "        if (useIt) { v = c; }\n"
        "        this.held #= v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Keeper k = heap Keeper();\n"
        "        k.keep(heap Cell(7), false);\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// A plain parameter merely READ, never stored, is the common case and must
// stay silent — the check is about capture, not about parameters.
TEST(CapturedBorrowParamTests, plainParamReadOnlyStillCompiles) {
    std::string src = std::string(kSrc) +
        "public final class D {\n"
        "    static int32 peek(Cell c) { return c.n; }\n"
        "    public static int32 run() { return D.peek(heap Cell(7)); }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// ---------------------------------------------------------------------------
// 3.3.3 — the warning-first migration switch (spec §3.4).
//
// Unit 3 landed this check error-first and it cost the gate: 1354/76/7 against
// a 1426/0/11 baseline, where the static audit had classified 10 sites. A
// throw stops the build at the FIRST capture, so enumerating the rest is one
// ~90s compile per site with the total never visible. Warn mode reports and
// keeps going, so ONE build per library enumerates every site — which is what
// makes the migration a single pass instead of an unbounded serial hunt.
//
// The switch is a migration instrument, not a permanent escape hatch: 3.3.3
// closes by flipping back to error with the sites fixed, and these tests pin
// both positions so the flip is verifiable rather than asserted.
// ---------------------------------------------------------------------------

namespace {

// Restores the process-wide switch no matter how the test leaves. Without
// this, a warn-mode test leaking into the suite would silently disarm every
// rejection test above — the failure mode would be tests that pass while
// checking nothing.
struct WarnMode {
    WarnMode() { cajeta::Scope::setCapturedBorrowWarns(true); }
    ~WarnMode() { cajeta::Scope::clearCapturedBorrowWarnsOverride(); }
};

}  // namespace

// The 3.1.1 shape, unchanged, under warn mode: it must COMPILE AND RUN. The
// capture is still real — this is the demotion admitting a known-broken
// program through so the rest of the library can be enumerated behind it.
TEST(CapturedBorrowParamTests, warnModeAdmitsTheCaptureAndKeepsCompiling) {
    WarnMode warn;
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell held;\n"
        "    public Keeper() { this.held = null; }\n"
        "    public void keep(Cell c) { this.held = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Keeper k = heap Keeper();\n"
        "        Cell c = heap Cell(7);\n"
        "        k.keep(c);\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    compileExpectOk(src);
    EXPECT_EQ(runI32(src), 7);
}

// Warn mode must not make the check FORGET anything: the element-store site
// (3.1.2) goes through the same demotion, so both call sites are pinned.
TEST(CapturedBorrowParamTests, warnModeCoversTheElementStoreSiteToo) {
    WarnMode warn;
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell[] slots;\n"
        "    public Keeper() { this.slots = heap Cell[4]; }\n"
        "    public void put(int32 i, Cell c) { this.slots[i] = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    compileExpectOk(src);
}

// ERROR IS THE DEFAULT, and stays so after a warn-mode test has run. This is
// the other half of the flip: 3.3.3 is only closed when the check throws
// again, and a suite that could not tell the two apart could not certify it.
TEST(CapturedBorrowParamTests, defaultModeStillThrows) {
    EXPECT_FALSE(cajeta::Scope::capturedBorrowWarns());
    std::string src = std::string(kSrc) +
        "public final class Keeper {\n"
        "    Cell held;\n"
        "    public Keeper() { this.held = null; }\n"
        "    public void keep(Cell c) { this.held = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_CAPTURED_BORROW_PARAM");
}
