//
// Live report 2026-07-22 (tour line 132): step-over on a
// `list.stream().forEach((x) -> x.f())` line never lands on the next line.
// The call site and the lambda body share the line, and the lambda executes
// once per element through stdlib frames — the pending-step logic must skip
// all of it and stop on the FOLLOWING line of the origin frame.
//
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "../TempProgram.h"

using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::debugtest::TempProgram;

namespace {

const char* kMain =
    "package demo;\n"
    "import cajeta.collection.ArrayList;\n"
    "public class Prog {\n"
    "    public static int32 main() {\n"
    "        ArrayList<Item> items = heap ArrayList<Item>();\n"   // line 5
    "        items.add(heap Item());\n"                           // line 6
    "        items.add(heap Item());\n"                           // line 7
    "        items.stream().forEach((it) -> it.bump());\n"        // line 8
    "        return Item.total + 40;\n"                           // line 9
    "    }\n"
    "}\n";

const char* kItem =
    "package demo;\n"
    "public class Item {\n"
    "    static int32 total = 0;\n"
    "    public void bump() {\n"
    "        total = total + 1;\n"
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

std::string stoppedReason(const std::vector<Json>& log) {
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "stopped")
            return m.at("body").at("reason").asString();
    return "";
}

} // namespace

TEST(LambdaStep, StepOverForEachLandsOnNextLine) {
    TempProgram p("demo", "Prog.cajeta", kMain);
    { std::ofstream out(p.root / "demo" / "Item.cajeta"); out << kItem; }
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    // Step over the forEach: both lambda iterations and the stdlib frames
    // must be skipped; land on line 9 with reason "step".
    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step") << "step-over ran away (or exited)";

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) line = fr[0].at("line").asInt();
        }
    EXPECT_EQ(line, 9) << "step-over landed on the wrong line";

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
}

// Tour's line-132 forEach runs demos that SPAWN FIBERS. The pending step is
// fiber-filtered (DebugController:88), but this pins the WHOLE interaction:
// a step-over across a lambda whose body spawns + joins fibers must still
// land on the following line of the origin frame.
TEST(LambdaStep, StepOverForEachWithFiberSpawningBodyLands) {
    static const char* kMainF =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        ArrayList<Item> items = heap ArrayList<Item>();\n"
        "        items.add(heap Item());\n"
        "        items.add(heap Item());\n"
        "        items.stream().forEach((it) -> it.bump());\n"    // line 8
        "        return Item.total + 38;\n"                       // line 9
        "    }\n"
        "}\n";
    static const char* kItemF =
        "package demo;\n"
        "public class Item {\n"
        "    static int32 total = 0;\n"
        "    public void bump() {\n"
        "        scope {\n"
        "            spawn inc();\n"
        "            spawn inc();\n"
        "        }\n"
        "    }\n"
        "    public static void inc() {\n"
        "        total = total + 1;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Prog.cajeta", kMainF);
    { std::ofstream out(p.root / "demo" / "Item.cajeta"); out << kItemF; }
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step")
        << "step-over across fiber-spawning lambda ran away";

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) line = fr[0].at("line").asInt();
        }
    EXPECT_EQ(line, 9);

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
}
