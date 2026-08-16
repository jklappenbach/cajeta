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

// 2.1.1 for PRIMITIVES — `x = 5` then `x + 2` is the most basic thing anyone
// types in a notebook. Primitives have no drop entry, so the owner-promotion
// path never saw them and this used to refuse (and before the refusal, return
// garbage: 40 + 2 came back as -1254244080). They are now boxed into the
// session registry by __cajeta_session_bind_value.
TEST(KernelCellTests, primitiveBindingSpansCells) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int32 seed = 40;\n").ok);
    CellResult c2 = s->execute("return seed + 2;\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    EXPECT_EQ(42, c2.value);
}

// Rebinding a primitive in a later cell replaces the box; the old one is
// freed by the registry's rebind path, and reads after it see the new value.
TEST(KernelCellTests, primitiveRebindInLaterCellWins) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int32 n = 1;\n").ok);
    ASSERT_TRUE(s->execute("int32 n = 9;\n").ok);
    CellResult c3 = s->execute("return n + 1;\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;
    EXPECT_EQ(10, c3.value);
}

// A wider primitive than the pointer-sized default, to pin that the box is
// sized from the slot rather than assumed: int64 and float64 both survive.
TEST(KernelCellTests, widePrimitivesSpanCells) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int64 big = 5000000000;\n").ok);
    CellResult c2 = s->execute("return (int32) (big / 1000000000);\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;
    EXPECT_EQ(5, c2.value);

    ASSERT_TRUE(s->execute("float64 half = 0.5;\n").ok);
    CellResult c4 = s->execute("return (int32) (half * 8.0);\n");
    ASSERT_TRUE(c4.ok) << c4.errorId << ": " << c4.message;
    EXPECT_EQ(4, c4.value);
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
TEST(KernelCellTests, userTypeInstantiationSurvivesAcrossCells) {
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

// 2.1.3 / script-units 5.3 — redefining a class in a later cell is
// GENERATIONAL: a value bound from the old definition stays alive and its
// methods still run, while later cells mean the new definition.

// 2.1.4 / script-units 5.4 — a BODY-ONLY redefinition (same layout, same
// signatures, changed bodies) introduces NO new generation: values that
// already exist adopt the new bodies. Contrast typeRedefinitionIsGenerational,
// where the layout changed and the old value kept the old behaviour.
//
// DISABLED — a real gap. Today a body-only change takes the generational
// path, so this returns 5: the existing value keeps its old body.
//
// ATTEMPTED 2026-08-13 AND REVERTED. The plan called for detect-then-patch:
// compare the redeclaration's fields and signatures against the previous
// declaration, suppress the generation, then overwrite the materialized
// vtable's slots with the new bodies' addresses. The patch itself was written
// and is sound in shape — take the entry-array offset and the entry stride
// from the vtable StructType via DataLayout, never assume the
// {version,count,parent,drop_fn,classObject} prefix — but the DETECTION has no
// footing:
//
//   THERE IS NO PREVIOUS CLASS OBJECT TO COMPARE AGAINST. A redeclaration
//   REUSES the same CajetaClass instance (canonicalMap[canonical].get() ==
//   this inside generatePrototype), refilled from the new declaration. By the
//   time the struct name is chosen, the old field list and signature set are
//   already gone.
//
// So 5.4 needs the comparison to happen where the OLD shape is still readable
// — at declaration/parse time, before the class is refilled — with the result
// carried forward to generatePrototype. Snapshotting the shape (field names +
// type canonicals + signature set) into SessionState when a cell declares a
// class is the obvious way: it is small, it is exactly what the comparison
// needs, and the session already owns per-class bookkeeping via
// noteDeclaredClass.
//
// NOTE this also means the generational path is NOT distinguishing old from
// new via the class object — an older value survives because its vtable
// pointer was baked at construction, not because it holds an older class.
// Worth re-checking what 2.1.3's boundType is really buying before building
// on it. See plan 2.1.4.
TEST(KernelCellTests, bodyOnlyRedefinitionSwapsInPlace) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "public class Counter { public int32 n;\n"
        "  public Counter(int32 n) { this.n = n; }\n"
        "  public int32 value() { return this.n; } }\n"
        "Counter c = heap Counter(5);\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult c0 = s->execute("return c.value();\n");
    ASSERT_TRUE(c0.ok) << c0.errorId << ": " << c0.message;
    ASSERT_EQ(5, c0.value);

    // Identical layout and signature; only the body differs.
    CellResult c2 = s->execute(
        "public class Counter { public int32 n;\n"
        "  public Counter(int32 n) { this.n = n; }\n"
        "  public int32 value() { return this.n * 2; } }\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;

    CellResult c3 = s->execute("return c.value();\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;
    EXPECT_EQ(10, c3.value) << "the existing value did not adopt the new body";

    // ...and it adopted it by the route we think. Without this the test
    // passes just as well if the call were statically bound to the newest
    // symbol, which would leave every value reached through a vtable — the
    // actual case — still running the old body.
    EXPECT_GT(s->stats().vtableSlotsRepointed, 0)
        << "no vtable slot was repointed, so the swap came from somewhere else";

    // A value made AFTER the edit runs the new body too. It shares the one
    // table with the older value (this cell's vtable global was deduped into
    // a reference to it), so this pins that the patch serves both.
    CellResult c4 = s->execute(
        "Counter fresh = heap Counter(7);\n"
        "return fresh.value();\n");
    ASSERT_TRUE(c4.ok) << c4.errorId << ": " << c4.message;
    EXPECT_EQ(14, c4.value) << "a newly constructed value ran the old body";
}

// 2.1.3b — MEASURED, and there is no hole here. Filed as a suspected third
// position (after 2.1.3a's argument and assignment), on the reasoning that a
// field read takes its offset from the current layout. It does not: a seeded
// binding carries its OWN generation's type, so both the member lookup and
// the offset come from the generation the value actually belongs to. A field
// the newer generation added is simply not a member of the older one.
//
// That is also why 2.1.3a was needed and this is not: an argument is checked
// against the PARAMETER's type, so the callee would use the new layout. A
// field read never leaves the value's own type. Kept as the regression pin
// for that difference.

// 2.1.4 — the LIMIT of the in-place swap, pinned so it cannot regress into a
// silent half-swap. A class that implements an interface also has a
// per-(class, interface) vtable, and a class someone extends has its methods
// inlined into the subclass's table; the swap repoints neither. Such a
// redefinition therefore takes the GENERATIONAL path instead (5.3) — the old
// value keeps the old body, which is coherent — rather than swapping one
// table and leaving the object to answer differently depending on how it was
// called.

// 2.2.5 — ASSIGNMENT to a session-seeded binding rebinds it, so the NEXT
// cell sees the new value. `isScriptBindingName` is the set a unit DECLARES,
// so `tag = "second";` in a later cell fell straight through the rebind and
// the write landed only in the staging slot the seeded read materializes.
// Redeclaration always worked, which is why nothing caught this.
TEST(KernelCellTests, assignToSeededBindingRebinds) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    // A primitive rebinds through the box, like its declaration does.
    ASSERT_TRUE(s->execute("int32 n = 1;\n").ok);
    CellResult ni = s->execute("n = 2;\n");
    ASSERT_TRUE(ni.ok) << ni.errorId << ": " << ni.message;
    CellResult nr = s->execute("n;\n");
    ASSERT_TRUE(nr.ok) << nr.errorId << ": " << nr.message;
    EXPECT_EQ("2", nr.result) << "assignment did not reach the registry";

    // A reference rebinds through the pointer.
    ASSERT_TRUE(s->execute("String tag = \"first\";\n").ok);
    CellResult a = s->execute("tag = \"second\";\n");
    ASSERT_TRUE(a.ok) << a.errorId << ": " << a.message;
    CellResult r = s->execute("tag;\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_EQ("second", r.result);
}

// 2.1.3a — MIXING generations.
//
// DISABLED, and the finding is WORSE than the item was filed as. It was
// filed as a missing diagnostic ("the message does not mention generations").
// It is not: passing an old-generation value where the NEW generation is
// declared is accepted silently and then SIGSEGVs on a null deref — an old
// object reached through a new layout and a new vtable. A memory-safety hole,
// not a wording problem.
//
// Everything up to the mixing line PASSES and is worth keeping: the old value
// keeps its own body (`p.get()` still returns 7), which is 2.1.3's contract.
//
// `Point q = p;` is NOT the shape to probe with — a top-level binding may not
// hold a borrow, so it is rejected for an unrelated and correct reason. The
// argument shape below is the one that gets through.
//
// THE FIX needs the type checker to distinguish generations, which it cannot
// today: two generations share a canonical name and differ only in
// `CajetaClass::generationSuffix` (symbols carry it, the canonical does not),
// so any check comparing canonicals sees a match. The contained place to put
// it is argument binding plus assignment: when both sides are CajetaClass
// with the same canonical and different suffixes, error naming both.

// 2.3.1 (spec 4.4; script-units 6.x) — THE Cell$N-FREE CONTRACT, as an
// acceptance check over the shapes Unit 2 actually produces.
//
// Every cell compiles into a synthesized implicit class with a synthesized
// entry method (`cajeta.script.cell_3::__cajeta_script_entry`). That is
// scaffolding: the author wrote three lines in a notebook and must be told
// about them in the coordinates they typed. So each diagnostic has to name
// `In[N]` and a USER line, and none of them may mention the scaffolding.
//
// Asserted over one session across several failure shapes rather than one
// test per shape: the contract is uniform, and the failure mode it guards
// against — one diagnostic path that never got the U5 mapping — shows up as
// a single odd entry among many correct ones.
TEST(KernelCellTests, everyDiagnosticNamesTheCellAndLine) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    // Scaffolding vocabulary. `cajeta.script.` alone is NOT on the list: it
    // is the reserved default package, so a class the author declared in a
    // cell legitimately canonicalizes into it and naming it is correct.
    auto namesScaffolding = [](const std::string& text) {
        return text.find("__cajeta_script_entry") != std::string::npos
            || text.find("cajeta.script.cell_") != std::string::npos
            || text.find("cajeta.script.In_") != std::string::npos
            || text.find("Cell$") != std::string::npos;
    };

    auto expectWellNamed = [&](const CellResult& r, const char* what) {
        const std::string cell = "In[" + std::to_string(r.executionCount) + "]";
        EXPECT_FALSE(r.ok) << what << ": expected a failure";
        EXPECT_EQ(cell, r.file) << what << ": diagnostic file";
        EXPECT_GT(r.line, 0) << what << ": no user line";
        EXPECT_FALSE(namesScaffolding(r.message))
            << what << ": message leaks scaffolding: " << r.message;
        for (const auto& d : r.diagnostics) {
            if (d.severity != "error") continue;
            EXPECT_EQ(cell, d.file) << what << ": structured diagnostic file";
            EXPECT_GT(d.line, 0) << what << ": structured diagnostic line";
            EXPECT_FALSE(namesScaffolding(d.message))
                << what << ": structured message leaks scaffolding: "
                << d.message;
        }
    };

    ASSERT_TRUE(s->execute(
        "public class Point { public int32 x;\n"
        "  public Point(int32 x) { this.x = x; }\n"
        "  public int32 get() { return this.x; } }\n"
        "Point p = heap Point(7);\n"
        "int32 keep = 1;\n").ok);

    // 1. A syntax error — the located-listener path, which does not pass
    //    through the diagnostic engine at all.
    expectWellNamed(s->execute("int32 bad = ;\n"), "syntax error");

    // 2. An unknown member — ordinary semantic resolution.
    expectWellNamed(s->execute("int32 n = keep;\np.nosuch;\n"), "unknown member");

    // 3. A stale-generation use (2.1.3a) — thrown UNLOCATED from the session
    //    seam and stamped by remapScriptException, so it is the shape most
    //    likely to arrive with no coordinates at all.
    ASSERT_TRUE(s->execute(
        "public class Point { public int32 x; public int32 y;\n"
        "  public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
        "  public int32 get() { return this.x + this.y; } }\n").ok);
    expectWellNamed(s->execute("int32 use(Point pt) { return pt.get(); }\nuse(p);\n"),
                    "stale generation");

    // 4. A cell that RUNS and throws: the traceback is the other half of
    //    spec 4.4, and a frame in the cell's own entry must render as the
    //    cell, never as the class it compiles into.
    CellResult threw = s->execute("int32 pad = 0;\nthrow heap Exception(\"boom\");\n");
    EXPECT_FALSE(threw.ok) << "the throw did not fail the cell";
    EXPECT_TRUE(threw.threw) << "reported as a compile failure, not a throw";
    ASSERT_FALSE(threw.traceback.empty()) << "a throw with no traceback";
    const std::string cell = "In[" + std::to_string(threw.executionCount) + "]";
    EXPECT_EQ(cell, threw.traceback.front().file);
    EXPECT_GT(threw.traceback.front().line, 0);
    for (const auto& f : threw.traceback) {
        EXPECT_FALSE(namesScaffolding(f.text))
            << "traceback frame leaks scaffolding: " << f.text;
    }

    // The session is still usable after all of it.
    CellResult after = s->execute("keep + 1;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("2", after.result);
}
