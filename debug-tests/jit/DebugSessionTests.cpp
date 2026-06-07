//
// End-to-end CP3 test: a line breakpoint, set through the real JIT pipeline,
// parks the running program at the right source line and resumes on continue.
// This exercises the whole stack — codegen-emitted safepoints (CP2), the
// runtime handler + fiber id, the DebugController rendezvous, and the JIT debug
// session running the entry on a background thread.
//
#include <gtest/gtest.h>

#include <chrono>

#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/DebugVars.h"
#include "../TempProgram.h"

using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::startDebugSession;
using cajeta::dbg::StopEvent;
using cajeta::debugtest::TempProgram;

namespace {
// Lines:           1               2                3
const char* kProg =
    "package demo;\n"                              // 1
    "public class Calc {\n"                        // 2
    "    public static int32 main() {\n"           // 3
    "        int32 a = 6;\n"                        // 4
    "        int32 b = 7;\n"                        // 5
    "        return a * b;\n"                       // 6
    "    }\n"                                       // 7
    "}\n";                                          // 8
} // namespace

// A breakpoint on line 5 parks the program there; the parked loc maps back to
// line 5; resume() lets it finish and the int32 entry returns 42.
TEST(DebugSession, LineBreakpointParksThenResumes) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.Calc.main";

    std::vector<Breakpoint> bps{ Breakpoint{"Calc.cajeta", 5} };
    std::string err;
    auto session = startDebugSession(opts, bps, &err);
    ASSERT_NE(session, nullptr) << err;

    StopEvent ev;
    ASSERT_TRUE(session->controller().waitForStop(ev, std::chrono::seconds(30)))
        << "program never hit the breakpoint";

    const auto& loc = cajeta::dbg::globalDbgLocTable().at(ev.locId);
    EXPECT_EQ(loc.line, 5);
    // The entry runs on the session's program thread, not a fiber -> id 0.
    EXPECT_EQ(ev.fiberId, 0);
    EXPECT_FALSE(session->isFinished());

    session->controller().resume();
    int rc = session->join();
    EXPECT_EQ(rc, 42);
    EXPECT_TRUE(session->isFinished());
}

// With no breakpoints armed, the program runs straight through to completion.
TEST(DebugSession, NoBreakpointsRunsToCompletion) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.Calc.main";

    std::string err;
    auto session = startDebugSession(opts, /*breakpoints=*/{}, &err);
    ASSERT_NE(session, nullptr) << err;

    // Nothing should park.
    StopEvent ev;
    EXPECT_FALSE(session->controller().waitForStop(ev, std::chrono::milliseconds(500)));

    int rc = session->join();
    EXPECT_EQ(rc, 42);
}

// CP5 end-to-end: at the breakpoint the dbg frame chain exposes the in-scope
// local `a` (declared on line 4) with its value 6. The safepoint on line 6
// fires after both declarations have executed, so `a` and `b` are registered.
TEST(DebugSession, FrameChainExposesLocals) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    JitRunOptions opts;
    opts.sourceRoot = p.sourceRoot();
    opts.entryMethod = "demo.Calc.main";

    std::vector<Breakpoint> bps{ Breakpoint{"Calc.cajeta", 6} };
    std::string err;
    auto session = startDebugSession(opts, bps, &err);
    ASSERT_NE(session, nullptr) << err;

    StopEvent ev;
    ASSERT_TRUE(session->controller().waitForStop(ev, std::chrono::seconds(30)))
        << "program never hit the breakpoint";
    ASSERT_NE(ev.frameTop, nullptr) << "no frame chain at stop";

    auto frames = cajeta::dbg::walkFrames(ev.frameTop);
    ASSERT_FALSE(frames.empty());
    const auto& top = frames[0];

    // Find locals a and b.
    std::string aVal, bVal;
    for (const auto& v : top.locals) {
        std::string rendered = cajeta::dbg::formatValue(v.type, v.addr);
        if (v.name == "a") aVal = rendered;
        if (v.name == "b") bVal = rendered;
    }
    EXPECT_EQ(aVal, "6");
    EXPECT_EQ(bVal, "7");

    session->controller().resume();
    int rc = session->join();
    EXPECT_EQ(rc, 42);
}
