//
// jupyter-kernel U1 (spec 2.1, 2.4; script-units 5.1-5.2) —
// CajetaKernelSession: the accumulating-dylib world.
//
// A kernel session is ONE LLJIT holding a bootstrap dylib (runtime + resident
// stdlib, initialized once) plus one JITDylib PER CELL, each linked against
// the stdlib and its predecessors. Cells accumulate: cell N sees every symbol
// cells 1..N-1 defined, without any cross-cell `Linker::linkModules` merge —
// resolution is by name at materialization, the model
// SharedStdlibDylibSpikeTests proved.
//
// These tests drive the session DIRECTLY: no ZeroMQ, no protocol, no cell
// numbering ceremony. Transport is U5's problem; this is about whether the
// world holds together as cells pile up.
//
// NOTE on link order (the hazard that makes this non-obvious): a fresh
// JITDylib seeds its link order with the PROCESS-SYMBOL main dylib first, so
// a plain addToLinkOrder(stdlibJD) leaves user code resolving runtime symbols
// to the NATIVE copy while the stdlib uses the JIT copy — two different
// __cajeta_main_exc_top TLS slots, and a `throw` crossing that boundary is
// never caught (JitTestHelper.cpp:1137-1150 documents the same bug in the
// per-test dylib path). The session must setLinkOrder explicitly with the
// stdlib and prior cells AHEAD of the process dylib; `throwCrossesCells`
// below is the regression that pins it.
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
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

// 1.1.1 / spec 2.1 — cell 1 defines a top-level method; cell 2 defines another
// that CALLS it. Cross-cell symbol resolution happens by name across dylibs;
// no cell's module is ever merged into another's.
TEST(KernelSessionTests, crossCellSymbolResolves) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("int32 foo() { return 40; }\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult c2 = s->execute("int32 bar() { return foo() + 2; }\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;

    auto bar = s->lookup<int32_t (*)()>("bar");
    ASSERT_NE(nullptr, bar);
    EXPECT_EQ(42, bar());

    // The accumulating-dylib contract: one dylib per cell, and no merge.
    EXPECT_EQ(2, s->stats().cellsCompiled);
    EXPECT_EQ(2, s->stats().cellDylibsCreated);
    EXPECT_EQ(0, s->stats().crossCellModuleMerges);
}

// 1.1.2 / spec 2.1 — a class static mutated by cell 1 is READ by cell 2 at
// its mutated value. Statics are session-lived: each cell's dylib runs its
// own ctors exactly once at initialize(), and the stdlib's run once at
// bootstrap — not once per cell.
TEST(KernelSessionTests, staticsPersistAcrossCells) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "public class Counter { public static int32 total; }\n"
        "Counter.total = 41;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult c2 = s->execute(
        "int32 readTotal() { return Counter.total + 1; }\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;

    auto readTotal = s->lookup<int32_t (*)()>("readTotal");
    ASSERT_NE(nullptr, readTotal);
    EXPECT_EQ(42, readTotal());   // 41 survived from cell 1
}

// Boundary pin for staticsPersistAcrossCells: does a static survive at all
// WITHIN its defining cell? If this passes and the cross-cell version fails,
// the fault is the session seam (each cell resolving its own copy of the
// global); if this fails too, the assignment or read is broken before the
// seam is ever involved, and the cross-cell test is blaming the wrong thing.
TEST(KernelSessionTests, staticVisibleWithinDefiningCell) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "public class Counter { public static int32 total; }\n"
        "Counter.total = 41;\n"
        "int32 readHere() { return Counter.total; }\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    auto readHere = s->lookup<int32_t (*)()>("readHere");
    ASSERT_NE(nullptr, readHere);
    EXPECT_EQ(41, readHere());
}

// 1.1.3 / script-units 5.2 — redefinition is last-write-wins. Cell 2
// redefines `value()`; cell 3's call and a direct lookup BOTH get the new
// body. Requires per-cell resource-tracker shadowing: two strong definitions
// of one name across dylibs must not be a duplicate-definition error.
TEST(KernelSessionTests, redefinitionLastWriteWins) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int32 value() { return 1; }\n").ok);
    ASSERT_TRUE(s->execute("int32 value() { return 2; }\n").ok);

    CellResult c3 = s->execute("int32 useValue() { return value(); }\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;

    auto useValue = s->lookup<int32_t (*)()>("useValue");
    ASSERT_NE(nullptr, useValue);
    EXPECT_EQ(2, useValue());          // the call sees the new body

    auto value = s->lookup<int32_t (*)()>("value");
    ASSERT_NE(nullptr, value);
    EXPECT_EQ(2, value());             // and so does a direct lookup
}

// 1.1.4 / spec 2.4 — the same stdlib specialization used in two cells is
// ONE definition for the session. Without weak_odr demotion this is a hard
// ORC duplicate-definition failure on the second cell, so `ok` alone is a
// real assertion; the shared address proves it wasn't merely re-emitted.
TEST(KernelSessionTests, instantiationDefinedOnce) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "import cajeta.collection.ArrayList;\n"
        "int32 sizeOne() {\n"
        "    ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "    xs.add(1);\n"
        "    return (int32) xs.count();\n"
        "}\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    // An unrelated cell in between, so the two uses are not adjacent.
    ASSERT_TRUE(s->execute("int32 filler() { return 0; }\n").ok);

    CellResult c3 = s->execute(
        "import cajeta.collection.ArrayList;\n"
        "int32 sizeTwo() {\n"
        "    ArrayList<int32> ys = heap ArrayList<int32>();\n"
        "    ys.add(1);\n"
        "    ys.add(2);\n"
        "    return (int32) ys.count();\n"
        "}\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;

    auto sizeOne = s->lookup<int32_t (*)()>("sizeOne");
    auto sizeTwo = s->lookup<int32_t (*)()>("sizeTwo");
    ASSERT_NE(nullptr, sizeOne);
    ASSERT_NE(nullptr, sizeTwo);
    EXPECT_EQ(1, sizeOne());
    EXPECT_EQ(2, sizeTwo());

    // The demotion actually ran — without it the second cell's re-emitted
    // specialization is a hard ORC duplicate-definition failure, so this
    // pins the mechanism rather than just the happy outcome above.
    EXPECT_GT(s->stats().weakDemotedInstantiations, 0);
}

// 1.1.5 / spec 2.5 — runtime singletons. Task-spawning cells share ONE
// carrier pool and one __cajeta_task_shutdown definition; shutdown runs
// exactly once, at session end, not per cell (a per-cell shutdown would
// tear the pool out from under later cells).
TEST(KernelSessionTests, carrierAndRuntimeSingletonsSharedAcrossCells) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int32 one() { return 1; }\n").ok);
    void* shutdownAfterCell1 = s->lookupSymbol("__cajeta_task_shutdown");

    ASSERT_TRUE(s->execute("int32 two() { return 2; }\n").ok);
    void* shutdownAfterCell2 = s->lookupSymbol("__cajeta_task_shutdown");

    ASSERT_NE(nullptr, shutdownAfterCell1);
    EXPECT_EQ(shutdownAfterCell1, shutdownAfterCell2)
        << "each cell resolved its own runtime copy — the carrier pool and "
           "TLS state would be split per cell";

    EXPECT_EQ(0, s->stats().taskShutdownCalls);
    s->shutdown();
    EXPECT_EQ(1, s->stats().taskShutdownCalls);
    s->shutdown();                                   // idempotent
    EXPECT_EQ(1, s->stats().taskShutdownCalls);
}

// Link-order regression (see the file header): a `throw` raised in a later
// cell and caught in that same cell must find the JIT's exception TLS, not
// the native runtime's. This fails with a plain addToLinkOrder(stdlibJD) —
// the process-symbol dylib wins the lookup and the throw is never caught.
TEST(KernelSessionTests, throwCrossesCellsAndIsCaught) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute(
        "import cajeta.error.Exception;\n"
        "void raise() { throw heap Exception(\"boom\"); }\n").ok);

    CellResult c2 = s->execute(
        "import cajeta.error.Exception;\n"
        "int32 catches() {\n"
        "    try { raise(); return 0; }\n"
        "    catch (Exception e) { return 7; }\n"
        "}\n");
    ASSERT_TRUE(c2.ok) << c2.errorId << ": " << c2.message;

    auto catches = s->lookup<int32_t (*)()>("catches");
    ASSERT_NE(nullptr, catches);
    EXPECT_EQ(7, catches());
}

// script-units 5.5 / spec 2.2 — a cell that fails to compile leaves the
// session exactly as it was: no dylib, no partial definitions, and the next
// cell still sees everything the good cells defined.
TEST(KernelSessionTests, failedCellLeavesSessionUnchanged) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("int32 good() { return 5; }\n").ok);
    int dylibsBefore = s->stats().cellDylibsCreated;

    CellResult bad = s->execute("int32 broken() { return notAThing(); }\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.errorId.empty());
    EXPECT_EQ(dylibsBefore, s->stats().cellDylibsCreated)
        << "a failed cell must not leave a dylib behind";

    CellResult after = s->execute("int32 stillWorks() { return good() + 2; }\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    auto stillWorks = s->lookup<int32_t (*)()>("stillWorks");
    ASSERT_NE(nullptr, stillWorks);
    EXPECT_EQ(7, stillWorks());
}

// 1.3.1 — the SCALE acceptance: a twenty-cell session of the shape a
// notebook actually accumulates (definitions, uses of earlier cells' values,
// spawned work), then a clean teardown.
//
// It ASSERTS structure and MEASURES time. Per-cell timings are printed for
// the record and reviewed rather than thresholded — a wall-clock bound
// compiled into a test fails on a loaded machine and says nothing about the
// code (see the project's perf-acceptance rule). What IS asserted is the
// thing a threshold was standing in for: one dylib per cell and no merging,
// so the world cannot be growing quadratically behind a fast wall clock.
//
// Run under MALLOC_PERTURB_ for the leak half; the numbers from that run go
// in the commit message.
TEST(KernelSessionTests, twentyCellSessionStaysBoundedAndTearsDownClean) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    constexpr int kCells = 20;
    std::vector<double> millis;
    millis.reserve(kCells);

    for (int i = 1; i <= kCells; ++i) {
        const std::string n = std::to_string(i);
        std::string source;
        switch (i % 4) {
            case 1:   // define a type and bind one
                source = "public class N" + n + " { public int32 v;\n"
                         "  public N" + n + "(int32 v) { this.v = v; }\n"
                         "  public int32 get() { return this.v; } }\n"
                         "N" + n + " obj" + n + " = heap N" + n + "(" + n + ");\n";
                break;
            case 2:   // use an EARLIER cell's binding and add one of your own
                source = "int32 acc" + n + " = obj" + std::to_string(i - 1)
                       + ".get() + " + n + ";\n";
                break;
            case 3:   // a stdlib generic over a cell-defined type
                source = "import cajeta.collection.ArrayList;\n"
                         "ArrayList<N" + std::to_string(i - 2) + "> list" + n
                       + " = heap ArrayList<N" + std::to_string(i - 2) + ">();\n"
                         "list" + n + ".add(heap N" + std::to_string(i - 2)
                       + "(" + n + "));\n";
                break;
            default:  // spawned work, joined in the same cell
                source = "int32 spawned" + n + " = 0;\n"
                         "scope {\n"
                         "  spawned" + n + " = " + n + ";\n"
                         "}\n";
                break;
        }
        const auto start = std::chrono::steady_clock::now();
        CellResult r = s->execute(source);
        const auto end = std::chrono::steady_clock::now();
        millis.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
        ASSERT_TRUE(r.ok) << "cell " << i << ": " << r.errorId << ": "
                          << r.message << "\n--- source ---\n" << source;
        EXPECT_EQ(i, r.executionCount);
    }

    // One dylib per cell, and no cell was ever merged into another. This is
    // the structural form of "compile time does not grow with N": the JIT
    // world grows by exactly one unit per cell.
    EXPECT_EQ(kCells, s->stats().cellsCompiled);
    EXPECT_EQ(kCells, s->stats().cellDylibsCreated);
    EXPECT_EQ(0, s->stats().crossCellModuleMerges);

    // For the record, not for a threshold — and compared LIKE FOR LIKE. The
    // four cell kinds cost wildly different amounts (a stdlib generic over a
    // cell-defined type is ~50x a plain read), and cell 1 also pays for the
    // session's first compile, so a first-half/second-half mean says nothing
    // about growth. What answers "does cell N cost more than cell N-4" is the
    // first and last occurrence of each KIND.
    static const char* kindName[4] = {"spawn", "define", "use", "generic"};
    std::cerr << "[1.3.1] per-cell ms:";
    for (double m : millis) std::cerr << " " << (long) m;
    std::cerr << "\n";
    for (int kind = 0; kind < 4; ++kind) {
        double first = -1, last = -1;
        int firstAt = 0, lastAt = 0;
        for (int i = 1; i <= kCells; ++i) {
            if (i % 4 != kind) continue;
            if (first < 0) { first = millis[i - 1]; firstAt = i; }
            last = millis[i - 1];
            lastAt = i;
        }
        if (first < 0 || firstAt == lastAt) continue;
        std::cerr << "[1.3.1] " << kindName[kind] << ": cell " << firstAt
                  << " = " << (long) first << " ms, cell " << lastAt << " = "
                  << (long) last << " ms\n";
    }

    // Teardown: bindings dropped while the runtime that their drop code
    // reaches into is still up, then the task shutdown — each exactly once.
    s->shutdown();
    EXPECT_EQ(1, s->stats().sessionDropAllCalls);
    EXPECT_EQ(1, s->stats().taskShutdownCalls);
    EXPECT_EQ(0, s->stats().liveSessionBindings)
        << "bindings survived shutdown";
    EXPECT_GT(s->stats().sessionBindingsAtShutdown, 0)
        << "twenty cells bound nothing at all — the session never saw them";

    // Idempotent: a second shutdown is a no-op, not a double drop.
    s->shutdown();
    EXPECT_EQ(1, s->stats().sessionDropAllCalls);
    EXPECT_EQ(1, s->stats().taskShutdownCalls);
}

// 7.2.7 — A SESSION'S TYPES MUST NOT OUTLIVE IT.
//
// llvm struct types are CONTEXT-owned, and on the resident path the context
// outlives the session. So a class a session declared is still registered in
// the context's NamedStructTypes when the NEXT session declares one by the
// same name, and the second session gets the first session's LAYOUT: declaring
// `Point {x, y}` after a dead session left a one-field `Point` behind failed
// with `Invalid indices for GEP pointer type` on the GEP for `y`.
//
// This is written as ONE test with two sessions on purpose. It was found as a
// cross-test order dependency — KernelIoTests failing only when it happened to
// follow KernelCellTests — where the test that FAILS is not the test at fault,
// and each passes alone. Pinning it as an ordering rule between two suites
// would pin nothing; a session's teardown either gives the name back or it
// does not.
//
// The REDEFINITION in session 1 is load-bearing. A redefined class mints a new
// generation (`Point$g2`) and takes over the registry key, so generation 1's
// struct is unreachable from `canonicalMap` — which is exactly why the
// existing `CajetaType::releaseThrownTransientStructNames()` walk does not
// cover it, and why the release is by delivered MODULE instead.
TEST(KernelSessionTests, aSessionsStructNamesDoNotLeakIntoTheNext) {
    {
        auto first = KernelSession::create();
        ASSERT_NE(nullptr, first.get());
        ASSERT_TRUE(first->execute(
            "public class Point { public int32 x;\n"
            "  public Point(int32 x) { this.x = x; }\n"
            "  public int32 get() { return this.x; } }\n"
            "Point p = heap Point(3);\n").ok);
        // Supersede it: `Point` now maps to generation 2, and generation 1's
        // one-field struct is the thing that leaks.
        ASSERT_TRUE(first->execute(
            "public class Point { public int32 x; public int32 y;\n"
            "  public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
            "  public int32 get() { return this.x + this.y; } }\n"
            "Point q = heap Point(1, 2);\n").ok);
        first->shutdown();
    }

    // A NEW session, same class name, two fields. Its `y` is at index 2 and
    // must land in a struct that has an index 2.
    auto second = KernelSession::create();
    ASSERT_NE(nullptr, second.get());
    CellResult r = second->execute(
        "public class Point { public int32 x; public int32 y;\n"
        "  public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
        "  public int32 sum() { return this.x + this.y; } }\n"
        "Point p = heap Point(4, 5);\n"
        "p.sum();\n");
    ASSERT_TRUE(r.ok) << r.errorId << ": " << r.message;
    EXPECT_TRUE(r.hasResult);
    EXPECT_EQ("9", r.result);
}
