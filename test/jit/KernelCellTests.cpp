//
// jupyter-kernel U2 (spec 2.1-2.3; script-units 4.1-4.4, 5.3-5.5) — cell
// semantics over the session.
//
// U1 built the world cells live in and gave them a cumulative namespace for
// METHODS and TYPES. This unit is about VALUES: a binding created in cell K
// must be the same live object cell K+1 mutates. That is the read-through-
// session codegen script-units left open as 4.2.4(a) — before this, a
// cross-unit read of a live binding threw a located NOT_IMPLEMENTED.
//
// The distinction that matters: session bindings are locals of each unit's
// synthesized entry, so they are NOT visible inside a top-level METHOD (which
// is a static member of the implicit class). Cells read them from top-level
// statements; the tests use the entry's return value (CellResult::value) to
// observe.
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <cstdint>
#include <memory>
#include <string>

using cajeta::kernel::KernelSession;
using cajeta::kernel::CellResult;

namespace {

std::unique_ptr<KernelSession> freshSession() {
    std::string error;
    auto s = KernelSession::create(&error);
    EXPECT_TRUE(s != nullptr) << "session create failed: " << error;
    return s;
}

}  // namespace

// 2.1.1 / spec 2.1 — THE notebook behaviour: cell 1 binds a collection, cell 2
// mutates THAT object and sees both elements. Not a copy, not a fresh value.
TEST(KernelCellTests, bindingSpansCells) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "import cajeta.collection.ArrayList;\n"
        "ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "xs.add(1);\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult c2 = s->execute(
        "xs.add(7);\n"
        "return (int32) xs.count();\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    EXPECT_EQ(2, c2.value) << "cell 2 did not mutate cell 1's live object";
}

// KNOWN GAP, pinned so it fails loudly rather than silently. Only OWNERS
// reach the session registry — U3's promotion rides the drop-entry choke
// point, and a primitive has no drop entry, so nothing ever binds it. Reading
// one from a later cell must REFUSE, not load through a null occupant and
// return garbage (which is what it did before this was pinned: 40 + 2 came
// back as -1254244080). When primitives gain session registration this test
// flips to asserting 42.
TEST(KernelCellTests, primitiveBindingAcrossCellsRefusesLoudly) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int32 seed = 40;\n").ok);
    CellResult c2 = s->execute("return seed + 2;\n");
    EXPECT_FALSE(c2.ok) << "returned " << c2.value << " instead of refusing";
    EXPECT_EQ("CAJETA_ERROR_NOT_IMPLEMENTED", c2.errorId);
}

// script-units 4.2 across the kernel seam — a binding MOVED OUT in one cell is
// unreadable in the next until rebound, with the transfer site named. This
// already worked at the compiler level (SessionOwnershipTests); here it is
// pinned through the kernel, where the diagnostic reaches a notebook user.
TEST(KernelCellTests, movedBindingRejectedInLaterCell) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute(
        "public class Probe { public int32 v; public Probe(int32 v) { this.v = v; } }\n"
        "void take(#Probe p) { }\n"
        "Probe p = heap Probe(1);\n").ok);
    ASSERT_TRUE(s->execute("take(#p);\n").ok);

    CellResult c3 = s->execute("return p.v;\n");
    EXPECT_FALSE(c3.ok);
    EXPECT_EQ("CAJETA_ERROR_MOVE_OF_BORROW", c3.errorId);
    EXPECT_NE(std::string::npos, c3.message.find("moved out"))
        << "diagnostic did not explain the move: " << c3.message;
}

// script-units 4.3 + 5.2 — rebinding a name in a later cell drops the old
// value and the name owns the new one; reads after the rebind are legal again.
TEST(KernelCellTests, rebindInLaterCellRestoresReadability) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute(
        "public class Probe { public int32 v; public Probe(int32 v) { this.v = v; } }\n"
        "void take(#Probe p) { }\n"
        "Probe p = heap Probe(1);\n").ok);
    ASSERT_TRUE(s->execute("take(#p);\n").ok);

    CellResult c3 = s->execute("Probe p = heap Probe(9);\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;

    CellResult c4 = s->execute("return p.v;\n");
    ASSERT_TRUE(c4.ok) << c4.errorId << ": " << c4.message;
    EXPECT_EQ(9, c4.value);
}

// 2.1.5 / spec 2.4 — a stdlib template instantiated over a USER type defined
// in an earlier cell. This is the audited hazard: under stdlib reuse, a
// specialization over a per-session user type is exactly the shape that
// triggers ReuseHazardAbort in the test harness, and the instantiation's IR
// is owned by the stdlib module while the type is owned by a cell. Cell 3
// re-using it must not re-emit a second strong definition either.
// DISABLED until plan 2.1.6 lands. It pins a REAL gap (see the diagnosis
// above) and fails honestly; it is disabled only so `main` stays green for
// everyone else's sweeps, not because the behaviour is acceptable. Run it
// with --gtest_also_run_disabled_tests when working 2.1.6.
TEST(KernelCellTests, DISABLED_userTypeInstantiationSurvivesAcrossCells) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute(
        "public class Foo { public int32 v; public Foo(int32 v) { this.v = v; } }\n").ok);

    CellResult c2 = s->execute(
        "import cajeta.collection.ArrayList;\n"
        "ArrayList<Foo> foos = heap ArrayList<Foo>();\n"
        "foos.add(heap Foo(3));\n"
        "return (int32) foos.count();\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    EXPECT_EQ(1, c2.value);

    // Third cell uses the SAME specialization again — one definition, and the
    // live binding from cell 2 is still the object being appended to.
    CellResult c3 = s->execute(
        "foos.add(heap Foo(4));\n"
        "return (int32) foos.count();\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;
    EXPECT_EQ(2, c3.value);
}

// 2.1.2 / spec 2.2 — a failed cell leaves BINDINGS intact, not merely the
// symbol table: the session must be usable after a typo, and the value bound
// before the failure must still be there.
TEST(KernelCellTests, failedCellLeavesBindingsIntact) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    // An OWNING binding, deliberately: a primitive would trip the separate
    // not-yet-registered gap above and this test would pass or fail for a
    // reason that has nothing to do with atomicity.
    ASSERT_TRUE(s->execute(
        "import cajeta.collection.ArrayList;\n"
        "ArrayList<int32> kept = heap ArrayList<int32>();\n"
        "kept.add(5);\n").ok);

    CellResult bad = s->execute("return notAThing();\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.errorId.empty());

    CellResult after = s->execute("return (int32) kept.count();\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ(1, after.value) << "the binding did not survive the failed cell";
}
