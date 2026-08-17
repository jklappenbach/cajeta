//
// jupyter-kernel U7 (spec 6.1; plan 7.1.1) — the session classpath contract.
//
// This file USED to put `dev.cajeta.ml` — an Olla library developed in the
// sibling clone `cajeta-ml` — on a session classpath and drive a notebook
// against it. That was a DEPENDENCY INVERSION: dev.cajeta.ml depends on
// cajeta, not the other way around, so cajeta's own gate must never wait on a
// downstream package's build output. It bit exactly as you would expect —
// landing spec §4.6 (`#=` at a `#T` receipt) turned both tests red against a
// prebuilt archive from 2026-08-07, on someone else's release cadence, with
// nothing wrong in this repo.
//
// What remains is the half of 7.2.4's contract this repo can own: a session
// with NO classpath is a stdlib session. The positive path — a session that
// loads a real archive off the classpath — wants a fixture archive BUILT IN
// THIS TREE, the way `DependencyTests` and `HttpRepositoryV2Tests` already
// build their own `.cja` files. Filed rather than faked: a test that skips
// when a sibling clone is missing reports green on most machines while
// covering nothing.
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <memory>
#include <string>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;

// The half of 7.2.4's contract this repo owns: a session with NO classpath is still
// a stdlib session, and asking for a class that is not on it fails as a
// compile error rather than by some other route. Guards against a future
// "just put everything on the classpath" shortcut.
TEST(KernelDataScienceTests, noClasspathMeansStdlibOnly) {
    std::string error;
    auto s = KernelSession::create(&error);
    ASSERT_NE(nullptr, s.get()) << error;

    CellResult r = s->execute(
        "import acme.notreal.Absent;\n"
        "Absent a = heap Absent();\n");
    EXPECT_FALSE(r.ok)
        << "a class from an un-classpathed archive resolved anyway";

    // The session survives the failure, as any failed cell must.
    CellResult after = s->execute("1 + 1;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("2", after.result);
}
