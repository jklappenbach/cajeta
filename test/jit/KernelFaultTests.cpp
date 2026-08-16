//
// jupyter-kernel U4 (spec 3.4→shutdown, 4.4; plan 4.1) — failure containment.
//
// A notebook you cannot make a mistake in is not a notebook. Until this unit
// an uncaught throw from a cell reached `__cajeta_throw` with no exception
// frame installed and called `exit(1)` — the KERNEL died, taking every
// binding and every earlier cell with it, for a typo. What these pin is that
// a cell can fail and the session carries on.
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <csignal>
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

// 4.1.1 / spec 4.4, 7.1 — the cell throws, the session lives. The error is
// structured (type, message, a traceback naming the cell), and the NEXT cell
// runs against bindings the throw did not disturb.

// A cell's own locals are dropped on the way out (the throw unwinds to the
// guard's watermark), while SESSION bindings are not — they belong to the
// session registry, not to the entry's drop frame. A later cell rebinding the
// name must therefore still work.

// 4.1.2 — a throw from inside `scope { spawn … }` surfaces as the cell's
// error, and the carrier pool survives it: the next cell can still spawn.
TEST(KernelFaultTests, recoverableThrowInSpawnedWork) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult bad = s->execute(
        "async int32 boom() { throw heap Exception(\"in-task\"); return 0; }\n"
        "scope {\n"
        "    spawn boom();\n"
        "}\n");
    EXPECT_FALSE(bad.ok) << "a throwing task did not surface as a cell error";

    // Carriers survive: spawning again works and the session still computes.
    CellResult after = s->execute(
        "async int32 seven() { return 7; }\n"
        "int32 total = 0;\n"
        "scope {\n"
        "    total = await spawn seven();\n"
        "}\n"
        "total;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("7", after.result);
}

// A throwing cell must not leave the compiler or the JIT wedged: the very
// next cell compiles and runs normally, and so does the one after it.

// 4.1.3 / spec 3.3-3.4 — shutdown drops the session's bindings and joins the
// carriers, in that order and each exactly once. Dropping AFTER the task
// shutdown would run drop code on a pool that had already been torn down.

// 4.3.1 / spec 4 — UNRECOVERABLE is a panic, not a cell error. The session
// guard is a catch-all, so without an explicit check it would swallow one and
// carry on over a world the runtime has already reported as broken. Documented
// behaviour: the kernel dies loudly, having said why.
//
// A death test, because "the process ends" IS the contract.
//
// It failed for a while after the abort branch landed, and the cause was NOT
// what the failure looked like: the on-disk object cache was serving objects
// built BEFORE the branch existed, so the child ran the old guard and
// swallowed the panic. The tell was the child's wall time — 5s (cache-served,
// failed) versus ~49s (cold, passed) — not anything about descriptors. Stable
// over four consecutive runs across both harness shapes once the cache turned
// over.
TEST(KernelFaultTests, unrecoverableThrowKillsTheKernelLoudly) {
    // The predicate accepts any death EXCEPT the swallow marker below, and
    // the regex is the "loudly" half — the runtime must have said what broke
    // before it went. Asserting SIGABRT specifically would be asserting the
    // signal handler's chaining, not this contract.
    // THREADSAFE, not the default fast style. Fast forks this process, and
    // this process already hosts an LLJIT: the child gets one thread and none
    // of ORC's pools, so the session it builds is not the session under test
    // — it died before reaching the throw, with nothing on stderr. Threadsafe
    // re-execs, paying a cold prime for a truthful run.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // testing::ExitedWithCode, not WIFEXITED/WEXITSTATUS — those live in
    // <sys/wait.h>, which mingw does not ship, and the raw macros also encode
    // the POSIX wait-status layout, which is not what gtest hands the
    // predicate on Windows. ExitedWithCode is gtest's portable spelling of the
    // same test and is what makes this file compile on the Windows runners.
    auto notSwallowed = [](int status) {
        return !::testing::ExitedWithCode(99)(status);
    };
    ASSERT_EXIT({
        auto s = freshSession();
        if (!s) _exit(98);
        CellResult r = s->execute(
            "throw heap UnrecoverableException(\"invariant\");\n");
        _exit(r.ok ? 92 : (r.threw ? 90 : 91));
    }, notSwallowed, "unrecoverable exception: invariant");
}

// 2.3.2 — a would-be-UB TRAP must stop the CELL, not the kernel.
//
// Divide-by-zero is undefined behaviour in Cajeta and `--ub-traps` (on by
// default in Debug) lowers it to `llvm.trap`. That is right for a program:
// the trap fires before the optimizer wrong-codes around the UB. It is fatal
// for a notebook, where `4 / 0` is one of the most ordinary things a person
// types by accident and a `ud2` takes the kernel, every binding and every
// earlier cell with it — with nothing printed, because a trap says nothing.
//
// Found by 2.3.1, which originally used this as its throw shape and killed
// the test process (SIGILL, exit 132).

// The same containment for the rest of the family — one trap site serves
// divide, remainder, the three shifts and signed overflow, so a fix that
// only covered division would be a coincidence rather than a mechanism.
TEST(KernelFaultTests, oversizedShiftAndRemainderAlsoFailTheCellOnly) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult rem = s->execute("int32 zero = 0;\nint32 r = 7 % zero;\n");
    EXPECT_FALSE(rem.ok) << "remainder by zero reported success";
    EXPECT_EQ("ArithmeticError", rem.exceptionType);
    EXPECT_NE(std::string::npos, rem.message.find("remainder by zero"))
        << rem.message;

    CellResult shift = s->execute("int32 wide = 64;\nint32 v = 1 << wide;\n");
    EXPECT_FALSE(shift.ok) << "an oversized shift reported success";
    EXPECT_EQ("ArithmeticError", shift.exceptionType);
    EXPECT_NE(std::string::npos, shift.message.find("bit width")) << shift.message;

    CellResult after = s->execute("return 5;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ(5, after.value);
}
