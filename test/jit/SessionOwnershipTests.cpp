//
// script-units U4 (spec 4.2, 4.6) — ownership analysis across the session
// seam.
//
// Within one unit the memory model applies unchanged: a `#` transfer demotes
// its top-level source to a borrow, a SECOND transfer is the standard
// CAJETA_ERROR_MOVE_OF_BORROW. Across units the rule is stricter (spec 4.2):
// a binding moved out in unit K is unreadable in unit K+1 until rebound —
// the borrow's validity cannot be seen across the seam. The compiler-side
// carrier is SessionState: each unit's entry codegen seeds its root scope
// from the table and writes ownership facts back (carry-facts-in-the-slot,
// the DebugTypeTable precedent).
//
// Runtime side: moving a session binding away must DISARM its registry slot
// (the new owner drops it; __cajeta_session_drop_all must not double-drop).
// Borrow-shaped top-level bindings are rejected outright (spec 4.6): the
// binding outlives the unit's frame, but the borrowed owner may drop or
// rebind in any later unit.
//

#include "gtest/gtest.h"
#include "JitTestHelper.h"
#include "cajeta/compile/SessionState.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Drop probe + a title-taking sink and a borrowing reader, all top-level.
const char* kPrelude =
    "public class Probe {\n"
    "    public static int32 drops;\n"
    "    public int32 id;\n"
    "    public Probe(int32 id) { this.id = id; }\n"
    "    public ~Probe() { Probe.drops = Probe.drops + 1; }\n"
    "}\n"
    "void take(#Probe p) { }\n"
    "void use(Probe p) { }\n"
    "int32 dropCount() { return Probe.drops; }\n";

struct CompileError {
    std::string errorId;
    std::string file;
    int line = -1;
};

// Compile a script unit into `session` under host name `hostName`; returns
// the jit (null plus a filled `err` when the compile failed).
std::unique_ptr<CajetaJit> compileUnit(const std::string& scriptTail,
                                       const std::string& fqClass,
                                       const std::string& hostName,
                                       cajeta::SessionState* session,
                                       CompileError* err = nullptr) {
    CajetaJit::Options opts;
    opts.session = session;
    opts.sessionHostName = hostName;
    try {
        return CajetaJit::compile(std::string(kPrelude) + scriptTail, fqClass,
                                  opts);
    } catch (cajeta::Exception& e) {
        if (err) {
            err->errorId = e.getErrorId();
            err->file = e.getFile();
            err->line = e.getLine();
        }
        return nullptr;
    }
}

}  // namespace

// 4.1.1 / spec 4.2 (single-unit form) — top-level bindings obey the standard
// linearity rules inside their own unit: transferring twice is the ordinary
// transfer-from-a-borrow error, exactly as for a method local.
TEST(SessionOwnershipTests, moveOutThenSecondTransferErrors) {
    CompileError err;
    auto jit = compileUnit(
        "Probe xs = heap Probe(1);\n"
        "take(#xs);\n"
        "take(#xs);\n"
        "return 0;\n",
        "cajeta.script.own1", "own1.cajeta", nullptr, &err);
    EXPECT_EQ(nullptr, jit.get());
    EXPECT_EQ("CAJETA_ERROR_MOVE_OF_BORROW", err.errorId);
}

// spec 4.2 runtime seam — moving a session binding away disarms its registry
// slot: the taker owns (and drops) the value; session end must not re-drop.

// spec 4.2 runtime seam, `#=` shape — `b #= a` between two top-level names:
// b's slot owns the object, a's slot is disarmed at the transfer. One drop
// total, at session end.

// 4.1.3 / spec 4.6 — a top-level binding cannot HOLD a borrow: the binding
// outlives the unit's frame, but the borrowed owner may drop or rebind in a
// later unit. Rejected at the declaration with a directive diagnostic.

// spec 4.5 boundary — the borrow-escape rule is about the SESSION scope
// only: the same alias inside a block is an ordinary frame-local borrow.

// White-box anchor for the U4 API — a unit's top-level bindings land in the
// SessionState with their canonical types, unmoved.

// 4.1.2 / spec 4.2 — move state spans units. Unit K moves `xs` out; unit
// K+1's read is rejected with the standard diagnostic, located at K+1's
// host name (4.3.1 / spec 6.1 groundwork).
TEST(SessionOwnershipTests, moveStateSpansUnits) {
    cajeta::SessionState session;
    auto k = compileUnit(
        "Probe xs = heap Probe(1);\n"
        "take(#xs);\n"
        "return 0;\n",
        "cajeta.script.cellone", "cell-1", &session);
    ASSERT_NE(nullptr, k.get());
    auto* fact = session.find("xs");
    ASSERT_NE(nullptr, fact);
    EXPECT_TRUE(fact->moved);

    CompileError err;
    auto k1 = compileUnit(
        "use(xs);\n"
        "return 0;\n",
        "cajeta.script.celltwo", "cell-2", &session, &err);
    EXPECT_EQ(nullptr, k1.get());
    EXPECT_EQ("CAJETA_ERROR_MOVE_OF_BORROW", err.errorId);
    EXPECT_EQ("cell-2", err.file);   // the LATER unit's host name
    EXPECT_GT(err.line, 0);
}

// 4.1.4 / spec 4.2 + 4.3 — rebinding a moved-out name clears its move state:
// the later unit redeclares `xs` and reads it freely.
