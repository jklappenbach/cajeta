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

// 1.1.2 / spec 2.1 — a class static mutated by cell 1 is READ by cell 2 at
// its mutated value. Statics are session-lived: each cell's dylib runs its
// own ctors exactly once at initialize(), and the stdlib's run once at
// bootstrap — not once per cell.

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

// 1.1.4 / spec 2.4 — the same stdlib specialization used in two cells is
// ONE definition for the session. Without weak_odr demotion this is a hard
// ORC duplicate-definition failure on the second cell, so `ok` alone is a
// real assertion; the shared address proves it wasn't merely re-emitted.

// 1.1.5 / spec 2.5 — runtime singletons. Task-spawning cells share ONE
// carrier pool and one __cajeta_task_shutdown definition; shutdown runs
// exactly once, at session end, not per cell (a per-cell shutdown would
// tear the pool out from under later cells).

// Link-order regression (see the file header): a `throw` raised in a later
// cell and caught in that same cell must find the JIT's exception TLS, not
// the native runtime's. This fails with a plain addToLinkOrder(stdlibJD) —
// the process-symbol dylib wins the lookup and the throw is never caught.

// script-units 5.5 / spec 2.2 — a cell that fails to compile leaves the
// session exactly as it was: no dylib, no partial definitions, and the next
// cell still sees everything the good cells defined.

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
