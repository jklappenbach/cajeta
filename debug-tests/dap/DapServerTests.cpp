//
// CP4 tests for the DAP server. Two layers:
//  1) pure message-builder tests (makeResponse/makeEvent/stackTraceBody) -
//     microsecond, no JIT.
//  2) a scripted end-to-end session driving DapServer::handle() through
//     initialize -> launch -> setBreakpoints -> configurationDone -> (stopped)
//     -> threads -> stackTrace -> continue -> (terminated), asserting the
//     emitted DAP messages. This runs the real JIT pipeline (~one compile).
//
#include <gtest/gtest.h>

#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "../TempProgram.h"

using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::dap::makeResponse;
using cajeta::dap::makeEvent;
using cajeta::dap::stackTraceBody;
using cajeta::dbg::StopEvent;
using cajeta::dbg::DbgLocTable;
using cajeta::debugtest::TempProgram;

// ---- pure builders ----

TEST(DapServerBuilders, ResponseEnvelope) {
    Json body = Json::object();
    body["x"] = 1;
    Json r = makeResponse(10, 3, "stackTrace", true, body);
    EXPECT_EQ(r.at("type").asString(), "response");
    EXPECT_EQ(r.at("seq").asInt(), 10);
    EXPECT_EQ(r.at("request_seq").asInt(), 3);
    EXPECT_EQ(r.at("command").asString(), "stackTrace");
    EXPECT_TRUE(r.at("success").asBool());
    EXPECT_EQ(r.at("body").at("x").asInt(), 1);
}

TEST(DapServerBuilders, EventEnvelope) {
    Json e = makeEvent(5, "stopped", Json::object());
    EXPECT_EQ(e.at("type").asString(), "event");
    EXPECT_EQ(e.at("event").asString(), "stopped");
    EXPECT_EQ(e.at("seq").asInt(), 5);
}

TEST(DapServerBuilders, StackTraceFromLoc) {
    DbgLocTable table;
    int32_t id = table.add("D:/proj/demo/Calc.cajeta", 5, 9, "demo.Calc::main");
    StopEvent stop{id, 0};
    Json body = stackTraceBody(stop, table);
    ASSERT_EQ(body.at("stackFrames").size(), 1u);
    const Json& f = body.at("stackFrames")[0];
    EXPECT_EQ(f.at("line").asInt(), 5);
    EXPECT_EQ(f.at("name").asString(), "demo.Calc::main");
    EXPECT_EQ(f.at("source").at("name").asString(), "Calc.cajeta");
    EXPECT_EQ(body.at("totalFrames").asInt(), 1);
}

// ---- scripted end-to-end ----

namespace {
const char* kProg =
    "package demo;\n"
    "public class Calc {\n"
    "    public static int32 main() {\n"
    "        int32 a = 6;\n"      // line 4
    "        int32 b = 7;\n"      // line 5
    "        return a * b;\n"     // line 6
    "    }\n"
    "}\n";

// Drive one request through the server, appending every emitted message to
// `log`. Returns the server's keep-going flag.
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

// Count messages of a given event name in the log.
int countEvent(const std::vector<Json>& log, const std::string& ev) {
    int n = 0;
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == ev) n++;
    return n;
}

// Find the first response for a command.
const Json* findResponse(const std::vector<Json>& log, const std::string& cmd) {
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == cmd) return &m;
    return nullptr;
}
} // namespace

TEST(DapServerSession, BreakpointStopsThenStackTraceThenContinue) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    // initialize -> response + initialized event
    drive(srv, req(1, "initialize", Json::object()), log);
    EXPECT_EQ(countEvent(log, "initialized"), 1);

    // launch (entry + source root)
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);

    // setBreakpoints on line 5
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
    const Json* sbResp = findResponse(log, "setBreakpoints");
    ASSERT_NE(sbResp, nullptr);
    EXPECT_TRUE(sbResp->at("body").at("breakpoints")[0].at("verified").asBool());

    // configurationDone -> starts program -> should hit the breakpoint
    drive(srv, req(4, "configurationDone", Json::object()), log);
    EXPECT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(countEvent(log, "terminated"), 0);  // parked, not done

    // stackTrace -> one frame on line 5
    log.clear();
    drive(srv, req(5, "stackTrace", Json::object()), log);
    const Json* stResp = findResponse(log, "stackTrace");
    ASSERT_NE(stResp, nullptr);
    const Json& frames = stResp->at("body").at("stackFrames");
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("line").asInt(), 5);
    EXPECT_EQ(frames[0].at("source").at("name").asString(), "Calc.cajeta");

    // continue -> program runs to completion -> terminated
    log.clear();
    bool keepGoing = drive(srv, req(6, "continue", Json::object()), log);
    EXPECT_TRUE(keepGoing);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
    // exited event carries the int32 return (42)
    for (const auto& m : log) {
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited") {
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
        }
    }
}

// CP5: at a stop, scopes -> Locals, variables -> a/b with values + types,
// setVariable mutates a primitive (a := 100), and continue runs to a return
// computing the new value (100 * 7 = 700).
TEST(DapServerSession, ScopesVariablesAndSetVariable) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    const Json* initResp = findResponse(log, "initialize");
    ASSERT_NE(initResp, nullptr);
    EXPECT_TRUE(initResp->at("body").at("supportsSetVariable").asBool());

    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);

    // Breakpoint on line 6 (return a * b;) — both a and b are declared by then.
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Calc.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 6;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);

    drive(srv, req(4, "configurationDone", Json::object()), log);
    EXPECT_EQ(countEvent(log, "stopped"), 1);

    // stackTrace -> frame id 0 on line 6.
    log.clear();
    drive(srv, req(5, "stackTrace", Json::object()), log);
    const Json* stResp = findResponse(log, "stackTrace");
    ASSERT_NE(stResp, nullptr);
    const Json& frames = stResp->at("body").at("stackFrames");
    ASSERT_GE(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("line").asInt(), 6);
    int frameId = frames[0].at("id").asInt();

    // scopes -> a single "Locals" scope with variablesReference frameId+1.
    log.clear();
    Json scopesArgs = Json::object();
    scopesArgs["frameId"] = frameId;
    drive(srv, req(6, "scopes", scopesArgs), log);
    const Json* scResp = findResponse(log, "scopes");
    ASSERT_NE(scResp, nullptr);
    const Json& scopes = scResp->at("body").at("scopes");
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes[0].at("name").asString(), "Locals");
    int varsRef = scopes[0].at("variablesReference").asInt();
    EXPECT_EQ(varsRef, frameId + 1);

    // variables -> a == 6, b == 7, both typed int32.
    log.clear();
    Json varsArgs = Json::object();
    varsArgs["variablesReference"] = varsRef;
    drive(srv, req(7, "variables", varsArgs), log);
    const Json* vResp = findResponse(log, "variables");
    ASSERT_NE(vResp, nullptr);
    const Json& vars = vResp->at("body").at("variables");
    std::string aVal, bVal, aType;
    for (size_t i = 0; i < vars.size(); ++i) {
        const std::string nm = vars[i].at("name").asString();
        if (nm == "a") { aVal = vars[i].at("value").asString();
                         aType = vars[i].at("type").asString(); }
        if (nm == "b") { bVal = vars[i].at("value").asString(); }
    }
    EXPECT_EQ(aVal, "6");
    EXPECT_EQ(bVal, "7");
    EXPECT_EQ(aType, "int32");

    // setVariable a := 100 -> response echoes the new value.
    log.clear();
    Json setArgs = Json::object();
    setArgs["variablesReference"] = varsRef;
    setArgs["name"] = "a";
    setArgs["value"] = "100";
    drive(srv, req(8, "setVariable", setArgs), log);
    const Json* setResp = findResponse(log, "setVariable");
    ASSERT_NE(setResp, nullptr);
    EXPECT_TRUE(setResp->at("success").asBool());
    EXPECT_EQ(setResp->at("body").at("value").asString(), "100");

    // continue -> return a * b now computes 100 * 7 = 700.
    log.clear();
    drive(srv, req(9, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
    for (const auto& m : log) {
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited") {
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 700);
        }
    }
}

// CP6f: a conditional breakpoint whose condition holds stops; one whose
// condition is false runs straight through to termination.
TEST(DapServerSession, ConditionalBreakpointStopsOnlyWhenConditionHolds) {
    // a == 6 is true at line 6 -> the server must stop there.
    {
        TempProgram p("demo", "Calc.cajeta", kProg);
        DapServer srv;
        std::vector<Json> log;
        drive(srv, req(1, "initialize", Json::object()), log);
        Json launchArgs = Json::object();
        launchArgs["entry-method"] = "demo.Calc.main";
        launchArgs["sourceRoot"] = p.sourceRoot();
        drive(srv, req(2, "launch", launchArgs), log);

        Json bpArgs = Json::object();
        Json src = Json::object(); src["path"] = "Calc.cajeta";
        bpArgs["source"] = src;
        Json bps = Json::array();
        Json bp = Json::object(); bp["line"] = 6; bp["condition"] = "a == 6";
        bps.push_back(bp);
        bpArgs["breakpoints"] = bps;
        drive(srv, req(3, "setBreakpoints", bpArgs), log);

        drive(srv, req(4, "configurationDone", Json::object()), log);
        EXPECT_EQ(countEvent(log, "stopped"), 1);
        EXPECT_EQ(countEvent(log, "terminated"), 0);
        // Resume to completion so the parked carrier unblocks before the
        // DapServer is destroyed (its dtor join()s the carrier thread; leaving
        // it parked would deadlock).
        drive(srv, req(5, "continue", Json::object()), log);
        EXPECT_EQ(countEvent(log, "terminated"), 1);
    }
    // a == 999 is never true -> no stop, runs to termination (exit 42).
    {
        TempProgram p("demo", "Calc.cajeta", kProg);
        DapServer srv;
        std::vector<Json> log;
        drive(srv, req(1, "initialize", Json::object()), log);
        Json launchArgs = Json::object();
        launchArgs["entry-method"] = "demo.Calc.main";
        launchArgs["sourceRoot"] = p.sourceRoot();
        drive(srv, req(2, "launch", launchArgs), log);

        Json bpArgs = Json::object();
        Json src = Json::object(); src["path"] = "Calc.cajeta";
        bpArgs["source"] = src;
        Json bps = Json::array();
        Json bp = Json::object(); bp["line"] = 6; bp["condition"] = "a == 999";
        bps.push_back(bp);
        bpArgs["breakpoints"] = bps;
        drive(srv, req(3, "setBreakpoints", bpArgs), log);

        drive(srv, req(4, "configurationDone", Json::object()), log);
        EXPECT_EQ(countEvent(log, "stopped"), 0);
        EXPECT_EQ(countEvent(log, "terminated"), 1);
        for (const auto& m : log) {
            if (m.at("type").asString() == "event" &&
                m.at("event").asString() == "exited") {
                EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
            }
        }
    }
}

// CP6f-2b: a spawn program parks inside a fiber; `threads` lists main (id 0)
// plus the spawned fiber, and the `stopped` event carries the fiber's id.
TEST(DapServerSession, ThreadsListsSpawnedFiberAndStoppedThreadId) {
    static const char* kSpawnProg =
        "package demo;\n"
        "public class Calc {\n"
        "    public static async int32 worker(int32 x) {\n"
        "        int32 y = x + 1;\n"     // line 4 — breakpoint inside the fiber
        "        return y;\n"            // line 5
        "    }\n"
        "    public static int32 main() {\n"
        "        int32 r = await spawn worker(41);\n"  // line 8
        "        return r;\n"            // line 9
        "    }\n"
        "}\n";
    TempProgram p("demo", "Calc.cajeta", kSpawnProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);

    Json bpArgs = Json::object();
    Json src = Json::object(); src["path"] = "Calc.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object(); bp["line"] = 4;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);

    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    // The stopped event names the spawned fiber (id >= 1), not main.
    int stoppedTid = -1;
    for (const auto& m : log) {
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "stopped") {
            stoppedTid = m.at("body").at("threadId").asInt();
        }
    }
    EXPECT_GE(stoppedTid, 1);

    // threads: main (id 0) + the live spawned fiber (the stopped tid).
    log.clear();
    drive(srv, req(5, "threads", Json::object()), log);
    const Json* thResp = findResponse(log, "threads");
    ASSERT_NE(thResp, nullptr);
    const Json& threads = thResp->at("body").at("threads");
    ASSERT_GE(threads.size(), 2u) << "expected main + at least one fiber";
    bool sawMain = false, sawStopped = false;
    for (size_t i = 0; i < threads.size(); ++i) {
        int id = threads[i].at("id").asInt();
        if (id == 0) sawMain = true;
        if (id == stoppedTid) sawStopped = true;
    }
    EXPECT_TRUE(sawMain) << "main thread (id 0) missing";
    EXPECT_TRUE(sawStopped) << "stopped fiber id missing from threads";

    // Resume to termination so the parked carrier unblocks before the DapServer
    // dtor join()s it (else deadlock). 41 + 1 = 42.
    log.clear();
    drive(srv, req(6, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
}

TEST(DapServerSession, NoBreakpointsRunsToTermination) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);

    EXPECT_EQ(countEvent(log, "stopped"), 0);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
}

TEST(DapServerSession, DisconnectEndsLoop) {
    DapServer srv;
    std::vector<Json> log;
    bool keepGoing = drive(srv, req(1, "disconnect", Json::object()), log);
    EXPECT_FALSE(keepGoing);
    EXPECT_NE(findResponse(log, "disconnect"), nullptr);
}
