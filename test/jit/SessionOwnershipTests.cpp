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
TEST(SessionOwnershipTests, moveOutDisarmsSessionSlot) {
    auto jit = compileUnit(
        "Probe xs = heap Probe(7);\n"
        "take(#xs);\n"
        "return 0;\n",
        "cajeta.script.own2", "own2.cajeta", nullptr);
    ASSERT_NE(nullptr, jit.get());
    auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
    auto dropCount = jit->lookup<int32_t (*)()>("dropCount");
    auto dropAll = reinterpret_cast<void (*)()>(
        jit->lookupRawSymbol("__cajeta_session_drop_all"));
    ASSERT_TRUE(entry && dropCount && dropAll);
    entry();
    int32_t afterEntry = dropCount();
    EXPECT_EQ(1, afterEntry);          // `take` owned and dropped it
    dropAll();
    EXPECT_EQ(afterEntry, dropCount());  // the disarmed slot stays quiet
}

// spec 4.2 runtime seam, `#=` shape — `b #= a` between two top-level names:
// b's slot owns the object, a's slot is disarmed at the transfer. One drop
// total, at session end.
TEST(SessionOwnershipTests, transferInitDisarmsSourceSlot) {
    auto jit = compileUnit(
        "Probe a = heap Probe(1);\n"
        "Probe b #= a;\n"
        "return 0;\n",
        "cajeta.script.own3", "own3.cajeta", nullptr);
    ASSERT_NE(nullptr, jit.get());
    auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
    auto dropCount = jit->lookup<int32_t (*)()>("dropCount");
    auto dropAll = reinterpret_cast<void (*)()>(
        jit->lookupRawSymbol("__cajeta_session_drop_all"));
    ASSERT_TRUE(entry && dropCount && dropAll);
    entry();
    EXPECT_EQ(0, dropCount());   // both names alive-shaped; one real owner
    dropAll();
    EXPECT_EQ(1, dropCount());   // b's slot drops once; a's slot is disarmed
}

// 4.1.3 / spec 4.6 — a top-level binding cannot HOLD a borrow: the binding
// outlives the unit's frame, but the borrowed owner may drop or rebind in a
// later unit. Rejected at the declaration with a directive diagnostic.
TEST(SessionOwnershipTests, borrowCannotEscapeUnit) {
    CompileError err;
    auto jit = compileUnit(
        "Probe p = heap Probe(1);\n"
        "Probe q = p;\n"
        "return 0;\n",
        "cajeta.script.own4", "own4.cajeta", nullptr, &err);
    EXPECT_EQ(nullptr, jit.get());
    EXPECT_EQ("CAJETA_ERROR_SESSION_BORROW_ESCAPE", err.errorId);
}

// spec 4.5 boundary — the borrow-escape rule is about the SESSION scope
// only: the same alias inside a block is an ordinary frame-local borrow.
TEST(SessionOwnershipTests, blockLocalBorrowStaysLegal) {
    auto jit = compileUnit(
        "Probe p = heap Probe(1);\n"
        "{\n"
        "    Probe q = p;\n"
        "    use(q);\n"
        "}\n"
        "return 0;\n",
        "cajeta.script.own5", "own5.cajeta", nullptr);
    EXPECT_NE(nullptr, jit.get());
}

// White-box anchor for the U4 API — a unit's top-level bindings land in the
// SessionState with their canonical types, unmoved.
TEST(SessionOwnershipTests, sessionStateRecordsBindings) {
    cajeta::SessionState session;
    auto jit = compileUnit(
        "Probe xs = heap Probe(1);\n"
        "Probe ys = heap Probe(2);\n"
        "return 0;\n",
        "cajeta.script.own6", "own6.cajeta", &session);
    ASSERT_NE(nullptr, jit.get());
    auto* xs = session.find("xs");
    auto* ys = session.find("ys");
    ASSERT_NE(nullptr, xs);
    ASSERT_NE(nullptr, ys);
    EXPECT_EQ("cajeta.script.Probe", xs->typeCanonical);
    EXPECT_FALSE(xs->moved);
    EXPECT_FALSE(ys->moved);
}

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
TEST(SessionOwnershipTests, rebindClearsMoveState) {
    cajeta::SessionState session;
    auto k = compileUnit(
        "Probe xs = heap Probe(1);\n"
        "take(#xs);\n"
        "return 0;\n",
        "cajeta.script.cellthree", "cell-3", &session);
    ASSERT_NE(nullptr, k.get());
    ASSERT_TRUE(session.find("xs") && session.find("xs")->moved);

    auto k1 = compileUnit(
        "Probe xs = heap Probe(9);\n"
        "return xs.id;\n",
        "cajeta.script.cellfour", "cell-4", &session);
    ASSERT_NE(nullptr, k1.get());
    auto entry = k1->lookup<int32_t (*)()>("__cajeta_script_entry");
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(9, entry());
    auto* fact = session.find("xs");
    ASSERT_NE(nullptr, fact);
    EXPECT_FALSE(fact->moved);       // write-back cleared the move state
}
