//
// resident-debug-server Unit 1 — multi-session DAP lifecycle (plan 1.1.x,
// spec §2/§4). One DapServer serves sequential launch→…→disconnect cycles:
// `disconnect` ends the SESSION (and keeps the request loop alive); nothing
// observable — breakpoints, conditions, exception arming, env overlays,
// debuggee outcome — leaks from session N into session N+1.
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "../TempProgram.h"
#include "../../test/PortableEnv.h"   // setenv/unsetenv — absent from the MinGW CRT

using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::debugtest::TempProgram;

namespace {

const char* kCalcProg =
    "package demo;\n"
    "public class Calc {\n"
    "    public static int32 main() {\n"
    "        int32 a = 6;\n"      // line 4
    "        int32 b = 7;\n"      // line 5
    "        return a * b;\n"     // line 6
    "    }\n"
    "}\n";

bool drive(DapServer& srv, const Json& req, std::vector<Json>& log) {
    return srv.handle(req, [&log](const Json& m) { log.push_back(m); });
}

Json req(int seq, const std::string& command, Json args) {
    Json r = Json::object();
    r["seq"] = seq;
    r["type"] = "request";
    r["command"] = command;
    r["arguments"] = std::move(args);
    return r;
}

int countEvent(const std::vector<Json>& log, const std::string& ev) {
    int n = 0;
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == ev) n++;
    return n;
}

int exitCodeOf(const std::vector<Json>& log) {
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            return m.at("body").at("exitCode").asInt();
    return -1;
}

Json launchArgs(const TempProgram& p, const char* entry) {
    Json a = Json::object();
    a["entry-method"] = entry;
    a["sourceRoot"] = p.sourceRoot();
    return a;
}

} // namespace

// 1.1.1 — two full sessions through ONE server behave identically, and
// `disconnect` keeps the request loop alive (returns true).
TEST(DapMultiSession, TwoFullSessionsOneServer) {
    TempProgram p("demo", "Calc.cajeta", kCalcProg);
    DapServer srv;

    for (int session = 1; session <= 2; ++session) {
        std::vector<Json> log;
        drive(srv, req(1, "initialize", Json::object()), log);
        EXPECT_EQ(countEvent(log, "initialized"), 1) << "session " << session;
        drive(srv, req(2, "launch", launchArgs(p, "demo.Calc.main")), log);
        drive(srv, req(3, "configurationDone", Json::object()), log);
        EXPECT_EQ(exitCodeOf(log), 42) << "session " << session;
        EXPECT_EQ(countEvent(log, "exited"), 1) << "session " << session;
        EXPECT_EQ(countEvent(log, "terminated"), 1) << "session " << session;
        EXPECT_EQ(countEvent(log, "stopped"), 0) << "session " << session;

        std::vector<Json> tail;
        bool keepGoing = drive(srv, req(4, "disconnect", Json::object()), tail);
        EXPECT_TRUE(keepGoing)
            << "disconnect must end the session, not the process";
    }
}

// 1.1.2 — breakpoints, conditions, and exception arming from session 1 never
// fire in session 2 (spec 4.1: per-session state resets on disconnect).
TEST(DapMultiSession, BreakpointStateDoesNotLeak) {
    TempProgram p("demo", "Calc.cajeta", kCalcProg);
    DapServer srv;

    // Session 1: breakpoint on line 5 + armed exceptions; park, resume, exit.
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    drive(srv, req(2, "launch", launchArgs(p, "demo.Calc.main")), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Calc.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 5;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    Json exArgs = Json::object();
    Json filters = Json::array();
    filters.push_back(Json(std::string("all")));
    exArgs["filters"] = filters;
    drive(srv, req(4, "setExceptionBreakpoints", exArgs), log);
    drive(srv, req(5, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    log.clear();
    drive(srv, req(6, "continue", Json::object()), log);
    ASSERT_EQ(exitCodeOf(log), 42);
    drive(srv, req(7, "disconnect", Json::object()), log);

    // Session 2: NO breakpoints, NO exception filters — must run straight
    // through.
    log.clear();
    drive(srv, req(8, "initialize", Json::object()), log);
    drive(srv, req(9, "launch", launchArgs(p, "demo.Calc.main")), log);
    drive(srv, req(10, "configurationDone", Json::object()), log);
    EXPECT_EQ(countEvent(log, "stopped"), 0)
        << "session 1 breakpoint/exception arming leaked into session 2";
    EXPECT_EQ(exitCodeOf(log), 42);
    drive(srv, req(11, "disconnect", Json::object()), log);
}

// 1.1.3 — spec 4.2.2: session 1's env overlay is invisible to session 2.
TEST(DapMultiSession, EnvOverlayDoesNotLeak) {
    static const char* kEnvProg =
        "package demo;\n"
        "public class Env {\n"
        "    public static int32 main() {\n"
        "        String v = System.env.get(\"CAJETA_MULTISESSION_VAR\");\n"
        "        if (v == null) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    ::unsetenv("CAJETA_MULTISESSION_VAR");
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    DapServer srv;

    // Session 1: overlay sets the var; debuggee sees it (exit 1).
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = launchArgs(p, "demo.Env.main");
    Json env = Json::object();
    env["CAJETA_MULTISESSION_VAR"] = "yes";
    args["env"] = env;
    drive(srv, req(2, "launch", args), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    ASSERT_EQ(exitCodeOf(log), 1);
    drive(srv, req(4, "disconnect", Json::object()), log);

    // Session 2: no env in the launch — the overlay must be gone (exit 0).
    log.clear();
    drive(srv, req(5, "initialize", Json::object()), log);
    drive(srv, req(6, "launch", launchArgs(p, "demo.Env.main")), log);
    drive(srv, req(7, "configurationDone", Json::object()), log);
    EXPECT_EQ(exitCodeOf(log), 0)
        << "session 1's env overlay leaked into session 2";
    drive(srv, req(8, "disconnect", Json::object()), log);
}

// 1.1.4 — spec 4.2.3 (scoped): a FAILING session 1 (nonzero exit) leaves the
// server able to run a clean session 2 with a different program.
//
// KNOWN LIMITATION (found writing this test): an UNCAUGHT debuggee throw
// terminates the whole process — the runtime's uncaught handler exits, and
// in the in-process JIT that is the server. A resident server therefore
// loses residency on an uncaught throw and is recovered by the plugin's
// respawn (plan 5.2.2). Armed break-on-throw (the normal debug flow) parks
// instead and is unaffected. Spec 4.2.3 carries the same note.
TEST(DapMultiSession, FailedSessionThenCleanSessionWithOtherProgram) {
    static const char* kFailProg =
        "package demo;\n"
        "public class Fail {\n"
        "    public static int32 main() {\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    TempProgram failing("demo", "Fail.cajeta", kFailProg);
    TempProgram calm("demo", "Calc.cajeta", kCalcProg);
    DapServer srv;

    // Session 1: nonzero exit.
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    drive(srv, req(2, "launch", launchArgs(failing, "demo.Fail.main")), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    EXPECT_EQ(exitCodeOf(log), 7);
    drive(srv, req(4, "disconnect", Json::object()), log);

    // Session 2: a different, healthy program on the same server.
    log.clear();
    drive(srv, req(5, "initialize", Json::object()), log);
    drive(srv, req(6, "launch", launchArgs(calm, "demo.Calc.main")), log);
    drive(srv, req(7, "configurationDone", Json::object()), log);
    EXPECT_EQ(exitCodeOf(log), 42);
    EXPECT_EQ(countEvent(log, "stopped"), 0);
    drive(srv, req(8, "disconnect", Json::object()), log);
}

// Spec §4.1 (plan 10.2.1). A session ends when the client says so — but a
// client can also just go away, and the server is then destroyed with the
// debuggee still PARKED at a breakpoint. ~DapServer joined the program
// thread outright, and a parked thread is waiting for a resume that will
// now never come: the process hung forever, holding the terminal.
//
// Found as the LambdaStep "parallel step-over hang": that test asserts on
// the step and returns without continuing, so teardown deadlocked. The
// stepping itself was fine — the shutdown path was not.
//
// The destruction runs on its own thread so a regression FAILS here in
// bounded time instead of wedging the whole binary.
TEST(DapMultiSession, DestroyingAServerWhileParkedDoesNotHang) {
    TempProgram p("demo", "Calc.cajeta", kCalcProg);
    auto srv = std::make_unique<DapServer>();

    std::vector<Json> log;
    drive(*srv, req(1, "initialize", Json::object()), log);
    drive(*srv, req(2, "launch", launchArgs(p, "demo.Calc.main")), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Calc.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 4;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(*srv, req(3, "setBreakpoints", bpArgs), log);
    drive(*srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1) << "expected to be parked";

    std::atomic<bool> destroyed{false};
    std::thread teardown([&] {
        srv.reset();               // no disconnect: the client vanished
        destroyed.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(60);
    while (!destroyed.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_TRUE(destroyed.load())
        << "~DapServer is joining a parked debuggee — it must let the "
           "program run out first";
    if (destroyed.load()) teardown.join(); else teardown.detach();
}
