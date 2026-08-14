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
#include <sys/wait.h>
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
TEST(KernelFaultTests, uncaughtThrowKeepsSessionAlive) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("int32 keep = 41;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    CellResult bad = s->execute("throw heap Exception(\"boom\");\n");
    EXPECT_FALSE(bad.ok) << "a throwing cell reported success";
    EXPECT_TRUE(bad.threw) << "not reported as a throw";
    EXPECT_NE(std::string::npos, bad.exceptionType.find("Exception"))
        << "exception type was: " << bad.exceptionType;
    EXPECT_EQ("boom", bad.message);

    // Spec 4.4: frames name CELLS, not the synthesized class the cell
    // compiles into.
    ASSERT_FALSE(bad.traceback.empty()) << "no traceback";
    EXPECT_EQ("In[2], line 1", bad.traceback[0].text)
        << "top frame was: " << bad.traceback[0].text;

    // The session is intact: same bindings, still executing.
    CellResult after = s->execute("keep + 1;\n");
    ASSERT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("42", after.result);
}

// A cell's own locals are dropped on the way out (the throw unwinds to the
// guard's watermark), while SESSION bindings are not — they belong to the
// session registry, not to the entry's drop frame. A later cell rebinding the
// name must therefore still work.
TEST(KernelFaultTests, throwLeavesSessionBindingsRebindable) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("String tag = \"first\";\n").ok);
    CellResult bad = s->execute(
        "String local = \"scratch\";\n"
        "throw heap Exception(\"mid-cell\");\n");
    EXPECT_FALSE(bad.ok);

    ASSERT_TRUE(s->execute("String tag = \"second\";\n").ok);
    CellResult read = s->execute("tag;\n");
    ASSERT_TRUE(read.ok) << read.errorId << ": " << read.message;
    EXPECT_EQ("second", read.result);
}

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
TEST(KernelFaultTests, sessionRunsManyCellsAcrossFailures) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    for (int i = 0; i < 3; ++i) {
        CellResult bad = s->execute("throw heap Exception(\"again\");\n");
        EXPECT_FALSE(bad.ok) << "iteration " << i;
        CellResult ok = s->execute("1 + 1;\n");
        ASSERT_TRUE(ok.ok) << "iteration " << i << ": " << ok.errorId << ": "
                           << ok.message;
        EXPECT_EQ("2", ok.result);
    }
}

// 4.1.3 / spec 3.3-3.4 — shutdown drops the session's bindings and joins the
// carriers, in that order and each exactly once. Dropping AFTER the task
// shutdown would run drop code on a pool that had already been torn down.
TEST(KernelFaultTests, shutdownDropsBindingsAndJoins) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    ASSERT_TRUE(s->execute("String held = \"live\";\n").ok);
    CellResult work = s->execute(
        "async int32 worker() { System.stdout.println(\"worker\"); return 1; }\n"
        "scope {\n"
        "    spawn worker();\n"
        "}\n");
    ASSERT_TRUE(work.ok) << work.errorId << ": " << work.message;

    s->shutdown();
    EXPECT_EQ(1, s->stats().taskShutdownCalls);
    EXPECT_EQ(1, s->stats().sessionDropAllCalls);
    EXPECT_EQ(0, s->stats().liveSessionBindings)
        << "session bindings survived shutdown";

    // Idempotent — the destructor calls it too.
    s->shutdown();
    EXPECT_EQ(1, s->stats().taskShutdownCalls);
    EXPECT_EQ(1, s->stats().sessionDropAllCalls);
}

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
    auto notSwallowed = [](int status) {
        return !(WIFEXITED(status) && WEXITSTATUS(status) == 99);
    };
    ASSERT_EXIT({
        auto s = freshSession();
        if (!s) _exit(98);
        CellResult r = s->execute(
            "throw heap UnrecoverableException(\"invariant\");\n");
        _exit(r.ok ? 92 : (r.threw ? 90 : 91));
    }, notSwallowed, "unrecoverable exception: invariant");
}
