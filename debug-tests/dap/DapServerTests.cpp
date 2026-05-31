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
