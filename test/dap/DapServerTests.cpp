//
// DapServer scripted-session harness (test-battery-restructure 4.1).
//
// The debugger stack (DapServer + DebugController/DbgLocTable/DebugVars/
// ValueInspector) was 0% covered: its only consumers are IDE clients over
// stdio, so no in-repo test ever drove it. DapServer was DESIGNED for this
// harness — handle() processes ONE request and emits responses/events
// through a callback, no real I/O, no threads at the test level — so a
// deterministic scripted session covers the protocol arms and everything
// they pull in (the in-process debug build, breakpoint arming, the stop/
// resume machinery, loc-table frame resolution).
//
#include "gtest/gtest.h"

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::dap::DapServer;
using cajeta::dap::Json;

namespace {

Json makeRequest(int seq, const std::string& command, Json args) {
    Json req = Json::object();
    req["seq"] = seq;
    req["type"] = std::string("request");
    req["command"] = command;
    req["arguments"] = std::move(args);
    return req;
}

// Collects every emitted frame; helpers find responses/events by kind.
struct Collector {
    std::vector<Json> frames;
    DapServer::Emit emit() {
        return [this](const Json& j) { frames.push_back(j); };
    }
    const Json* firstEvent(const std::string& name) const {
        for (const auto& f : frames)
            if (f.at("type").asString() == "event"
                    && f.at("event").asString() == name)
                return &f;
        return nullptr;
    }
    const Json* responseTo(const std::string& command) const {
        for (const auto& f : frames)
            if (f.at("type").asString() == "response"
                    && f.at("command").asString() == command)
                return &f;
        return nullptr;
    }
    size_t countEvents(const std::string& name) const {
        size_t n = 0;
        for (const auto& f : frames)
            if (f.at("type").asString() == "event"
                    && f.at("event").asString() == name) ++n;
        return n;
    }
    std::string dumpAll() const {
        std::string out;
        for (const auto& f : frames) out += f.dump() + "\n";
        return out;
    }
};

// A source root holding one program with a known breakpoint line.
struct TempProgram {
    std::filesystem::path root;
    std::filesystem::path file;
    int bpLine = 0;
    TempProgram() {
        static thread_local std::mt19937_64 rng(std::random_device{}());
        root = std::filesystem::temp_directory_path()
             / ("cajeta_dap_" + std::to_string(rng()));
        std::filesystem::create_directories(root / "test");
        file = root / "test" / "D.cajeta";
        std::ofstream out(file);
        out << "package test;\n"            // 1
            << "public final class D {\n"   // 2
            << "    public static int32 run() {\n";  // 3
        out << "        int32 x = 41;\n";   // 4
        out << "        int32 y = x + 1;\n";  // 5  <- breakpoint
        bpLine = 5;
        out << "        return y;\n"        // 6
            << "    }\n"
            << "}\n";
    }
    ~TempProgram() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// A program whose breakpoint line sits INSIDE a loop, so a breakpoint that was
// removed from the registry but never disarmed on the live controller stops
// again on the next iteration — which is exactly the reported symptom
// (Julian, 2026-08-31: "Removed breakpoint. Still getting stops at 155").
// TempProgram's straight-line body cannot show this: its line is reached once.
struct LoopProgram {
    std::filesystem::path root;
    std::filesystem::path file;
    int bpLine = 0;
    LoopProgram() {
        static thread_local std::mt19937_64 rng(std::random_device{}());
        root = std::filesystem::temp_directory_path()
             / ("cajeta_dap_loop_" + std::to_string(rng()));
        std::filesystem::create_directories(root / "test");
        file = root / "test" / "L.cajeta";
        std::ofstream out(file);
        out << "package test;\n"                  // 1
            << "public final class L {\n"         // 2
            << "    public static int32 run() {\n"// 3
            << "        int32 i = 0;\n"           // 4
            << "        int32 s = 0;\n"           // 5
            << "        while (i < 5) {\n"        // 6
            << "            s = s + i;\n"         // 7  <- breakpoint, 5 hits
            << "            i = i + 1;\n"         // 8
            << "        }\n"                      // 9
            << "        return s;\n"              // 10
            << "    }\n"
            << "}\n";
        bpLine = 7;
    }
    ~LoopProgram() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

// The pure message builders: response/event envelope shape (seq threading,
// request_seq linkage, success flag) without any session.
TEST(DapServerTests, responseAndEventEnvelopesCarryProtocolFields) {
    Json body = Json::object();
    body["k"] = std::string("v");
    Json resp = cajeta::dap::makeResponse(7, 3, "initialize", true, body);
    EXPECT_EQ(resp.at("type").asString(), "response");
    EXPECT_EQ(resp.at("seq").asInt(), 7);
    EXPECT_EQ(resp.at("request_seq").asInt(), 3);
    EXPECT_EQ(resp.at("command").asString(), "initialize");
    EXPECT_TRUE(resp.at("success").asBool());
    EXPECT_EQ(resp.at("body").at("k").asString(), "v");

    Json ev = cajeta::dap::makeEvent(9, "stopped", Json::object());
    EXPECT_EQ(ev.at("type").asString(), "event");
    EXPECT_EQ(ev.at("seq").asInt(), 9);
    EXPECT_EQ(ev.at("event").asString(), "stopped");
}

// One scripted session, the whole core protocol: initialize (capabilities +
// initialized event), launch, setBreakpoints (verified), configurationDone
// (in-process debug build + run to the breakpoint -> stopped event),
// threads, stackTrace (file/line/function from the loc table), continue
// (run to completion -> terminated), disconnect ends the session.
TEST(DapServerTests, breakpointSessionStopsTracesResumesAndTerminates) {
    TempProgram prog;
    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;

    // initialize
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "initialize", Json::object()), emit))
        << out.dumpAll();
    {
        const Json* r = out.responseTo("initialize");
        ASSERT_NE(r, nullptr) << out.dumpAll();
        ASSERT_TRUE(r->at("success").asBool()) << out.dumpAll();
        EXPECT_TRUE(r->at("body")
                        .at("supportsConfigurationDoneRequest").asBool());
        EXPECT_NE(out.firstEvent("initialized"), nullptr) << out.dumpAll();
    }

    // launch (deferred until configurationDone)
    {
        Json args = Json::object();
        args["entry-method"] = std::string("test.D.run");
        args["sourceRoot"] = prog.root.string();
        args["stopOnEntry"] = false;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "launch", args), emit))
            << out.dumpAll();
        const Json* r = out.responseTo("launch");
        ASSERT_NE(r, nullptr) << out.dumpAll();
        ASSERT_TRUE(r->at("success").asBool()) << out.dumpAll();
    }

    // setBreakpoints on the known line
    {
        Json src = Json::object();
        src["path"] = prog.file.string();
        Json bp = Json::object();
        bp["line"] = prog.bpLine;
        Json bps = Json::array();
        bps.push_back(std::move(bp));
        Json args = Json::object();
        args["source"] = std::move(src);
        args["breakpoints"] = std::move(bps);
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setBreakpoints", args), emit))
            << out.dumpAll();
        const Json* r = out.responseTo("setBreakpoints");
        ASSERT_NE(r, nullptr) << out.dumpAll();
        ASSERT_TRUE(r->at("success").asBool()) << out.dumpAll();
    }

    // configurationDone: builds in-process, runs to the breakpoint.
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "configurationDone", Json::object()), emit))
        << out.dumpAll();
    const Json* stopped = out.firstEvent("stopped");
    ASSERT_NE(stopped, nullptr)
        << "no stopped event after configurationDone:\n" << out.dumpAll();
    EXPECT_EQ(stopped->at("body").at("reason").asString(), "breakpoint")
        << out.dumpAll();
    const int threadId = stopped->at("body").at("threadId").asInt();

    // threads: at least the stopped one.
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "threads", Json::object()), emit))
        << out.dumpAll();

    // stackTrace: the stopped frame carries our file and breakpoint line.
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "stackTrace", args), emit))
            << out.dumpAll();
        const Json* r = out.responseTo("stackTrace");
        ASSERT_NE(r, nullptr) << out.dumpAll();
        ASSERT_TRUE(r->at("success").asBool()) << out.dumpAll();
        const Json& frames = r->at("body").at("stackFrames");
        ASSERT_TRUE(frames.isArray()) << r->dump();
        ASSERT_FALSE(frames.elements().empty()) << r->dump();
        const Json& top = frames.elements().front();
        EXPECT_EQ(top.at("line").asInt(), prog.bpLine) << top.dump();
        EXPECT_EQ(top.at("source").at("name").asString(), "D.cajeta")
            << top.dump();
    }

    // continue: runs to completion -> terminated.
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "continue", args), emit))
            << out.dumpAll();
        ASSERT_NE(out.firstEvent("terminated"), nullptr)
            << "no terminated event after continue:\n" << out.dumpAll();
    }

    // disconnect ends the SESSION but not the serve loop (resident
    // lifecycle): handle stays true, the response succeeds, and per-session
    // state has reset for the next initialize.
    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "disconnect", Json::object()), emit))
        << out.dumpAll();
    {
        const Json* r = out.responseTo("disconnect");
        ASSERT_NE(r, nullptr) << out.dumpAll();
        EXPECT_TRUE(r->at("success").asBool()) << out.dumpAll();
    }
}

// Removing a breakpoint mid-session must DISARM it, not merely forget it.
//
// setBreakpoints edited `breakpoints_`, which is consumed once — at
// configurationDone, by startDebugSession — so nothing reached the live
// controller. DebugController has arm()/disarm() and its header names
// setBreakpoints as their driver; setBreakpoints called neither. Forty lines
// below it, setExceptionBreakpoints does the same thing correctly with
// armException()/disarmException(), which is what makes this an oversight
// rather than a design.
TEST(DapServerTests, removingABreakpointMidSessionDisarmsItOnTheLiveController) {
    LoopProgram prog;
    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;

    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "initialize", Json::object()), emit)) << out.dumpAll();
    {
        Json args = Json::object();
        args["entry-method"] = std::string("test.L.run");
        args["sourceRoot"] = prog.root.string();
        args["stopOnEntry"] = false;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "launch", args), emit))
            << out.dumpAll();
    }
    auto setBreakpointLines = [&](const std::vector<int>& lines) {
        Json src = Json::object();
        src["path"] = prog.file.string();
        Json bps = Json::array();
        for (int line : lines) {
            Json bp = Json::object();
            bp["line"] = line;
            bps.push_back(std::move(bp));
        }
        Json args = Json::object();
        args["source"] = std::move(src);
        args["breakpoints"] = std::move(bps);
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setBreakpoints", args), emit)) << out.dumpAll();
    };

    setBreakpointLines({prog.bpLine});
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "configurationDone", Json::object()), emit))
        << out.dumpAll();
    const Json* stopped = out.firstEvent("stopped");
    ASSERT_NE(stopped, nullptr) << "never stopped at all:\n" << out.dumpAll();
    const int threadId = stopped->at("body").at("threadId").asInt();
    const size_t stopsWhileArmed = out.countEvents("stopped");

    // The removal: a whole-file replace with an EMPTY set, which is exactly
    // what the plugin sends when the last breakpoint in a file is deleted.
    setBreakpointLines({});

    Json cont = Json::object();
    cont["threadId"] = threadId;
    ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", cont), emit))
        << out.dumpAll();

    EXPECT_EQ(out.countEvents("stopped"), stopsWhileArmed)
        << "stopped again at a removed breakpoint — it was forgotten, not "
           "disarmed:\n" << out.dumpAll();
    EXPECT_NE(out.firstEvent("terminated"), nullptr)
        << "did not run to completion after the breakpoint was removed:\n"
        << out.dumpAll();
}

// The other half of the same gap: a breakpoint ADDED after configurationDone
// never reached the controller either, so it could not bind at all.
TEST(DapServerTests, addingABreakpointMidSessionArmsItOnTheLiveController) {
    LoopProgram prog;
    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;

    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "initialize", Json::object()), emit)) << out.dumpAll();
    {
        Json args = Json::object();
        args["entry-method"] = std::string("test.L.run");
        args["sourceRoot"] = prog.root.string();
        args["stopOnEntry"] = true;          // park before the loop runs
        ASSERT_TRUE(server.handle(makeRequest(seq++, "launch", args), emit))
            << out.dumpAll();
    }
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "configurationDone", Json::object()), emit))
        << out.dumpAll();
    const Json* entry = out.firstEvent("stopped");
    ASSERT_NE(entry, nullptr) << "stopOnEntry did not park:\n" << out.dumpAll();
    const int threadId = entry->at("body").at("threadId").asInt();
    const size_t stopsBefore = out.countEvents("stopped");

    // Arm a line that was not in the launch set.
    {
        Json src = Json::object();
        src["path"] = prog.file.string();
        Json bp = Json::object();
        bp["line"] = prog.bpLine;
        Json bps = Json::array();
        bps.push_back(std::move(bp));
        Json args = Json::object();
        args["source"] = std::move(src);
        args["breakpoints"] = std::move(bps);
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setBreakpoints", args), emit)) << out.dumpAll();
    }

    Json cont = Json::object();
    cont["threadId"] = threadId;
    ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", cont), emit))
        << out.dumpAll();

    EXPECT_GT(out.countEvents("stopped"), stopsBefore)
        << "a breakpoint added mid-session never armed:\n" << out.dumpAll();
}
