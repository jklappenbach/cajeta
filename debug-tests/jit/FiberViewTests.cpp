//
// CP6f-2b tests: the host-side live-fiber view. Drives a real spawn program
// through startDebugSession, parks at a breakpoint INSIDE a spawned fiber, and
// asserts JitDebugSession::liveFibers() enumerates that fiber from the JIT
// module's registry (with the stable dbg id assigned in CP6f-2a) and exposes a
// walkable frame chain. This is the host half of the DAP `threads` wiring.
//
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/dbg/DebugVars.h"
#include "../TempProgram.h"

using cajeta::jit::JitDebugSession;
using cajeta::jit::startDebugSession;
using cajeta::jit::JitRunOptions;
using cajeta::jit::Breakpoint;
using cajeta::debugtest::TempProgram;

namespace {
// main spawns one async worker and awaits it; the breakpoint is on the
// worker's first statement, so the program parks inside the spawned fiber while
// main is parked at the await.
const char* kSpawnProg =
    "package demo;\n"
    "public class Calc {\n"
    "    public static async int32 worker(int32 x) {\n"
    "        int32 y = x + 1;\n"     // line 4 — breakpoint (inside the fiber)
    "        return y;\n"            // line 5
    "    }\n"
    "    public static int32 main() {\n"
    "        int32 r = await spawn worker(41);\n"  // line 8
    "        return r;\n"            // line 9
    "    }\n"
    "}\n";

// Resume in a loop until the program finishes, then join. Mirrors the existing
// DebugSession test's drain so a mis-fire can't hang the suite.
int drainToExit(JitDebugSession* session) {
    int exitCode = 0;
    std::thread joiner([&] { exitCode = session->join(); });
    for (int i = 0; i < 200 && !session->isFinished(); ++i) {
        session->controller().resume();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    joiner.join();
    return exitCode;
}
} // namespace

TEST(FiberView, LiveFibersEnumeratesSpawnedFiberAtBreakpoint) {
    TempProgram p("demo", "Calc.cajeta", kSpawnProg);
    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.Calc.main";
    std::vector<Breakpoint> bps{ Breakpoint{"Calc.cajeta", 4} };
    std::string err;
    auto session = startDebugSession(opts, bps, &err);
    ASSERT_NE(session, nullptr) << err;

    cajeta::dbg::StopEvent ev;
    bool stopped = session->controller().waitForStop(ev, std::chrono::seconds(10));
    ASSERT_TRUE(stopped) << "breakpoint inside spawned fiber never parked";
    // Stopped inside a spawned fiber -> dbg id >= 1 (CP6f-2a assigns it).
    EXPECT_GE(ev.fiberId, 1);

    auto fibers = session->liveFibers();
    // The spawned worker is live (running, parked at the safepoint) and thus in
    // the registry. main runs on the entry thread (id 0), which is NOT a
    // registered carrier fiber, so the registry holds exactly the worker.
    ASSERT_GE(fibers.size(), 1u) << "liveFibers() saw no fibers";
    bool sawStopped = false;
    for (const auto& f : fibers) {
        if (f.id == static_cast<int>(ev.fiberId)) {
            sawStopped = true;
            // The stopped fiber's frame chain is walkable and shows `worker`
            // with its local x (=41) at the breakpoint (y not yet declared).
            auto frames = cajeta::dbg::walkFrames(f.frameTop);
            ASSERT_FALSE(frames.empty()) << "stopped fiber frame chain empty";
            bool sawX = false;
            for (const auto& v : frames.front().locals) {
                if (v.name == "x") {
                    sawX = true;
                    EXPECT_EQ(cajeta::dbg::formatValue(v.type, v.addr), "41");
                }
            }
            EXPECT_TRUE(sawX) << "param x not found in worker frame";
        }
    }
    EXPECT_TRUE(sawStopped) << "stopped fiber id not present in liveFibers()";

    EXPECT_EQ(drainToExit(session.get()), 42);  // 41 + 1
}
