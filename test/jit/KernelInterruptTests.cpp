//
// jupyter-kernel U6 (spec 5.1-5.2; plan 6.1) — interrupt.
//
// A notebook you cannot stop is a notebook you have to kill. Unit 4 made a
// cell's FAILURE survivable; this makes a cell's RUNAWAY survivable, which is
// the more common accident: a `while` with a typo'd exit condition should
// cost the user a red cell, not their whole session.
//
// THREADING. These tests do all session work — create, execute, shutdown — on
// ONE worker thread, and interrupt from the test thread. That is not
// incidental: it is the class's contract (KernelSession.h) and the kernel's
// own shape, where the execution thread owns the session and the control
// channel interrupts it. Creating the session on the test thread and running
// cells on another looks harmless and is not — StdlibReuseCore's baselines
// are thread_local, so the executing thread finds none, takes the cold path,
// and compiles the stdlib from source. My first draft did exactly that and
// the cell died with `unknown field type 'bfloat16'` from a stdlib class that
// only that path ever reaches.
//
// The cell announces when it has entered its loop and the test waits for that
// announcement rather than sleeping a guessed interval. A sleep long enough
// to be safe on a slow machine is mostly waste on a fast one, and a sleep
// that lands during the cell's COMPILE interrupts nothing: the request is
// cleared at the start of every cell, by design (spec 5.2).
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using cajeta::kernel::CellResult;
using cajeta::kernel::KernelSession;

namespace {

// Spins until `pred` holds or the deadline passes. Returns whether it held —
// so a caller asserts on the outcome instead of on having waited.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::seconds limit) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// The cell that will not stop on its own. It announces itself first, so the
// test can interrupt when the loop is genuinely running rather than while the
// cell is still compiling.
const char* kRunawayCell =
    "System.stdout.println(\"entered-loop\");\n"
    "int32 spin = 0;\n"
    "while (spin < 2147483647) {\n"
    "    spin = spin + 1;\n"
    "}\n"
    "spin;\n";

}  // namespace

// The control for everything below: the same loop shape, bounded, no
// interrupt anywhere near it. If this fails, the interrupt tests are failing
// for a reason that has nothing to do with interrupts.

// THE MINIMAL REPRO (plan 6.1.3). One cell, three statements, no loop, no
// second thread, no timing: the interrupt is armed to trip at a KNOWN
// safepoint, so this asks one question and only one — does a longjmp out of
// ANY safepoint work?
//
// If this passes, safepoint unwinding is sound and whatever ails the runaway
// case is specific to loops. If it faults, the mechanism is wrong everywhere
// and the loop was never the variable. Either answer kills a whole family of
// hypotheses, which the threaded test could not do: it can only aim at a cell
// that runs long enough to be aimed at, and that means a loop is always in
// the picture.

// The repro above, moved INSIDE a loop and still single-threaded. Together
// the two isolate the runaway case's only remaining variables: this one adds
// "the safepoint is in a loop body" and holds the thread fixed; the threaded
// test adds "the request comes from another thread". Whichever of the two
// fails is the one carrying the defect.

// 6.1.1 / spec 5.1 — the runaway loop. The cell ends within a bound, the
// session survives with its bindings, and the next cell runs.
TEST(KernelInterruptTests, tightLoopInterrupts) {
    std::atomic<KernelSession*> live{nullptr};
    std::atomic<bool> looping{false};
    std::atomic<bool> ranaway{false};
    std::atomic<bool> done{false};
    std::string createError;
    bool boundOk = false;
    CellResult runaway;
    CellResult after;

    std::thread worker([&] {
        auto s = KernelSession::create(&createError);
        if (!s) { done.store(true); return; }
        s->setStreamHandler([&looping](const std::string& chunk) {
            if (chunk.find("entered-loop") != std::string::npos) {
                looping.store(true);
            }
        });
        live.store(s.get());
        // A binding made BEFORE the interrupt and read AFTER it: spec 5.1
        // promises the next cell runs against unchanged session state, which
        // is not observable without something to observe.
        boundOk = s->execute("int32 keepsafe = 17;\n").ok;
        runaway = s->execute(kRunawayCell);
        ranaway.store(true);
        after = s->execute("keepsafe + 1;\n");
        s->shutdown();
        done.store(true);
    });

    // Wait for the cell to be RUNNING, not merely submitted.
    bool started = waitFor([&] { return looping.load() || done.load(); },
                           std::chrono::seconds(300));
    ASSERT_TRUE(started) << "the cell never announced itself; create error: "
                         << createError;
    ASSERT_FALSE(done.load()) << "the session died before the loop: "
                              << createError;

    KernelSession* session = live.load();
    ASSERT_NE(nullptr, session);
    session->requestInterrupt();
    EXPECT_TRUE(waitFor([&] { return ranaway.load(); }, std::chrono::seconds(120)))
        << "the cell did not stop — a safepoint is the only thing that can "
           "end that loop, so either none were emitted or none were reached";
    ASSERT_TRUE(waitFor([&] { return done.load(); }, std::chrono::seconds(300)));
    worker.join();

    ASSERT_TRUE(boundOk) << "the setup cell failed";
    EXPECT_FALSE(runaway.ok) << "an interrupted cell reported success";
    EXPECT_TRUE(runaway.threw)
        << "not reported as a throw; errorId=" << runaway.errorId
        << " message=" << runaway.message;
    EXPECT_EQ("KeyboardInterrupt", runaway.exceptionType);
    EXPECT_FALSE(runaway.traceback.empty()) << "no traceback on the interrupt";

    // The session is intact: the earlier binding still reads.
    EXPECT_TRUE(after.ok) << after.errorId << ": " << after.message;
    EXPECT_EQ("18", after.result);
}

// 6.1.2 / spec 5.2 — an interrupt with nothing running is a no-op. The hazard
// is a LEFTOVER request: a flag set while idle and never cleared kills the
// next cell, and the user watches a cell they never interrupted die.

// One request interrupts ONE cell. The flag is taken (read-and-clear) at the
// safepoint that acts on it, so a single Ctrl-C cannot cascade into the cells
// that follow — which, from the user's side, would look like the kernel
// having died rather than a cell having stopped.
