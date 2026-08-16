//
// DapServer inspection + stepping sessions (test-battery-restructure 4.5).
//
// DapServerTests pins the breakpoint/stackTrace/continue spine; these
// sessions drive the arms that spine never touches — scopes, variables
// (locals rendering, aggregate expansion, paging), setVariable (a write the
// program's own exit code then proves), evaluate (bare names and
// `.field`/`[i]` paths, plus both refusal shapes), next/stepIn/stepOut, and
// the entry/exception stop reasons. Everything below the server rides along:
// ValueInspector decode/children, DebugVars, writeValue, the step machinery.
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

struct Collector {
    std::vector<Json> frames;
    DapServer::Emit emit() {
        return [this](const Json& j) { frames.push_back(j); };
    }
    const Json* lastEvent(const std::string& name) const {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            if (it->at("type").asString() == "event"
                    && it->at("event").asString() == name)
                return &*it;
        return nullptr;
    }
    // The response to the most recent request for `command` (sessions here
    // repeat commands, so first-match would return a stale frame).
    const Json* lastResponseTo(const std::string& command) const {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            if (it->at("type").asString() == "response"
                    && it->at("command").asString() == command)
                return &*it;
        return nullptr;
    }
    std::string dumpAll() const {
        std::string out;
        for (const auto& f : frames) out += f.dump() + "\n";
        return out;
    }
};

// A temp source root holding one test/D.cajeta with the given body lines.
struct TempProgram {
    std::filesystem::path root;
    std::filesystem::path file;
    TempProgram(const std::string& source) {
        static thread_local std::mt19937_64 rng(std::random_device{}());
        root = std::filesystem::temp_directory_path()
             / ("cajeta_dapinsp_" + std::to_string(rng()));
        std::filesystem::create_directories(root / "test");
        file = root / "test" / "D.cajeta";
        std::ofstream out(file);
        out << source;
    }
    ~TempProgram() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// Drive initialize → launch → setBreakpoints(bpLine) → configurationDone.
// Returns the stopped thread id (asserts a breakpoint stop happened).
int startToBreakpoint(DapServer& server, Collector& out, int& seq,
                      const TempProgram& prog, int bpLine,
                      int pageSize = 0, bool stopOnEntry = false) {
    auto emit = out.emit();
    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "initialize", Json::object()), emit));
    {
        Json args = Json::object();
        args["entry-method"] = std::string("test.D.run");
        args["sourceRoot"] = prog.root.string();
        args["stopOnEntry"] = stopOnEntry;
        if (pageSize > 0) args["pageSize"] = pageSize;
        EXPECT_TRUE(server.handle(makeRequest(seq++, "launch", args), emit));
    }
    if (bpLine > 0) {
        Json src = Json::object();
        src["path"] = prog.file.string();
        Json bp = Json::object();
        bp["line"] = bpLine;
        Json bps = Json::array();
        bps.push_back(std::move(bp));
        Json args = Json::object();
        args["source"] = std::move(src);
        args["breakpoints"] = std::move(bps);
        EXPECT_TRUE(server.handle(
            makeRequest(seq++, "setBreakpoints", args), emit));
    }
    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "configurationDone", Json::object()), emit));
    const Json* stopped = out.lastEvent("stopped");
    EXPECT_NE(stopped, nullptr) << out.dumpAll();
    if (!stopped) return -1;
    return stopped->at("body").at("threadId").asInt();
}

// stackTrace for `threadId`; returns the frames array (empty on failure).
std::vector<Json> stackFrames(DapServer& server, Collector& out, int& seq,
                              int threadId) {
    auto emit = out.emit();
    Json args = Json::object();
    args["threadId"] = threadId;
    EXPECT_TRUE(server.handle(makeRequest(seq++, "stackTrace", args), emit));
    const Json* r = out.lastResponseTo("stackTrace");
    EXPECT_NE(r, nullptr);
    if (!r || !r->at("success").asBool()) return {};
    return r->at("body").at("stackFrames").elements();
}

// scopes(frameId) → the Locals variablesReference (0 on failure).
int localsRef(DapServer& server, Collector& out, int& seq, int frameId) {
    auto emit = out.emit();
    Json args = Json::object();
    args["frameId"] = frameId;
    EXPECT_TRUE(server.handle(makeRequest(seq++, "scopes", args), emit));
    const Json* r = out.lastResponseTo("scopes");
    EXPECT_NE(r, nullptr);
    if (!r) return 0;
    const auto& scopes = r->at("body").at("scopes").elements();
    EXPECT_FALSE(scopes.empty()) << out.dumpAll();
    if (scopes.empty()) return 0;
    return scopes.front().at("variablesReference").asInt();
}

// variables(ref) → the variables array.
std::vector<Json> variablesOf(DapServer& server, Collector& out, int& seq,
                              int ref) {
    auto emit = out.emit();
    Json args = Json::object();
    args["variablesReference"] = ref;
    EXPECT_TRUE(server.handle(makeRequest(seq++, "variables", args), emit));
    const Json* r = out.lastResponseTo("variables");
    EXPECT_NE(r, nullptr);
    if (!r) return {};
    return r->at("body").at("variables").elements();
}

const Json* findVar(const std::vector<Json>& vars, const std::string& name) {
    for (const auto& v : vars)
        if (v.at("name").asString() == name) return &v;
    return nullptr;
}

} // namespace

// scopes → variables renders every local through the bridge; an array pages
// through expansion handles ([pageSize] then "[N more…]"); an object expands
// to its fields; setVariable writes a primitive the exit code then proves;
// evaluate resolves names and paths and refuses the two documented ways.
TEST(DapInspectionTests, scopesVariablesSetVariableAndEvaluate) {
    TempProgram prog(
        "package test;\n"                                          // 1
        "public class P {\n"                                       // 2
        "    public int32 f;\n"                                    // 3
        "    public String t;\n"                                   // 4
        "}\n"                                                      // 5
        "public final class D {\n"                                 // 6
        "    public static int32 run() {\n"                        // 7
        "        int32 x = 41;\n"                                  // 8
        "        String s = \"hey\";\n"                            // 9
        "        int32[] arr = heap int32[5];\n"                   // 10
        "        arr[0] = 10; arr[1] = 11; arr[2] = 12;\n"         // 11
        "        arr[3] = 13; arr[4] = 14;\n"                      // 12
        "        P p = heap P();\n"                                // 13
        "        p.f = 5;\n"                                       // 14
        "        String pt = \"tag\";\n"                           // 15
        "        p.t = pt;\n"                                      // 16
        "        int32 y = x + p.f + arr[2];\n"                    // 17  <- bp
        "        return y;\n"                                      // 18
        "    }\n"
        "}\n");
    const int bpLine = 17;

    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;
    // pageSize 3: a 5-element array pages as 3 + "[2 more…]".
    int threadId = startToBreakpoint(server, out, seq, prog, bpLine,
                                     /*pageSize=*/3);
    ASSERT_GE(threadId, 0) << out.dumpAll();

    auto frames = stackFrames(server, out, seq, threadId);
    ASSERT_FALSE(frames.empty()) << out.dumpAll();
    const int frameId = frames.front().at("id").asInt();

    int ref = localsRef(server, out, seq, frameId);
    ASSERT_GT(ref, 0) << out.dumpAll();

    auto vars = variablesOf(server, out, seq, ref);
    ASSERT_FALSE(vars.empty()) << out.dumpAll();

    // Primitive local: exact render, no expansion handle.
    const Json* vx = findVar(vars, "x");
    ASSERT_NE(vx, nullptr) << out.dumpAll();
    EXPECT_EQ(vx->at("value").asString(), "41") << vx->dump();
    EXPECT_EQ(vx->at("variablesReference").asInt(), 0) << vx->dump();

    // String local: the summary carries the text.
    const Json* vs = findVar(vars, "s");
    ASSERT_NE(vs, nullptr) << out.dumpAll();
    EXPECT_NE(vs->at("value").asString().find("hey"), std::string::npos)
        << vs->dump();

    // Array local: aggregate → expansion handle; page 1 is [0..2] + more.
    const Json* varr = findVar(vars, "arr");
    ASSERT_NE(varr, nullptr) << out.dumpAll();
    const int arrRef = varr->at("variablesReference").asInt();
    ASSERT_GT(arrRef, 0) << varr->dump();
    {
        auto page1 = variablesOf(server, out, seq, arrRef);
        ASSERT_EQ(page1.size(), 4u) << out.dumpAll();  // 3 elements + more-node
        EXPECT_EQ(page1[0].at("name").asString(), "[0]");
        EXPECT_EQ(page1[0].at("value").asString(), "10");
        EXPECT_EQ(page1[2].at("value").asString(), "12");
        const Json& more = page1[3];
        EXPECT_NE(more.at("name").asString().find("2 more"), std::string::npos)
            << more.dump();
        const int moreRef = more.at("variablesReference").asInt();
        ASSERT_GT(moreRef, 0) << more.dump();
        auto page2 = variablesOf(server, out, seq, moreRef);
        ASSERT_EQ(page2.size(), 2u) << out.dumpAll();
        EXPECT_EQ(page2[0].at("name").asString(), "[3]");
        EXPECT_EQ(page2[0].at("value").asString(), "13");
        EXPECT_EQ(page2[1].at("value").asString(), "14");
    }

    // Object local: expands to its fields.
    const Json* vp = findVar(vars, "p");
    ASSERT_NE(vp, nullptr) << out.dumpAll();
    const int pRef = vp->at("variablesReference").asInt();
    ASSERT_GT(pRef, 0) << vp->dump();
    {
        auto fields = variablesOf(server, out, seq, pRef);
        const Json* ff = findVar(fields, "f");
        ASSERT_NE(ff, nullptr) << out.dumpAll();
        EXPECT_EQ(ff->at("value").asString(), "5") << ff->dump();
        const Json* ft = findVar(fields, "t");
        ASSERT_NE(ft, nullptr) << out.dumpAll();
        EXPECT_NE(ft->at("value").asString().find("tag"), std::string::npos)
            << ft->dump();
    }

    // evaluate: bare name, field path, index path.
    {
        Json args = Json::object();
        args["expression"] = std::string("p.f");
        args["frameId"] = frameId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "evaluate", args), emit));
        const Json* r = out.lastResponseTo("evaluate");
        ASSERT_NE(r, nullptr);
        ASSERT_TRUE(r->at("success").asBool()) << r->dump();
        EXPECT_EQ(r->at("body").at("result").asString(), "5") << r->dump();
    }
    {
        Json args = Json::object();
        args["expression"] = std::string("arr[2]");
        args["frameId"] = frameId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "evaluate", args), emit));
        const Json* r = out.lastResponseTo("evaluate");
        ASSERT_TRUE(r->at("success").asBool()) << r->dump();
        EXPECT_EQ(r->at("body").at("result").asString(), "12") << r->dump();
    }
    {
        // Navigation only — an operator expression is refused as unsupported.
        Json args = Json::object();
        args["expression"] = std::string("x + 1");
        args["frameId"] = frameId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "evaluate", args), emit));
        const Json* r = out.lastResponseTo("evaluate");
        EXPECT_FALSE(r->at("success").asBool()) << r->dump();
    }
    {
        // An unknown name is "not available", not a crash.
        Json args = Json::object();
        args["expression"] = std::string("zzz");
        args["frameId"] = frameId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "evaluate", args), emit));
        const Json* r = out.lastResponseTo("evaluate");
        EXPECT_FALSE(r->at("success").asBool()) << r->dump();
    }

    // setVariable x=100: the response renders the new value, and the resumed
    // program computes y from it — the exit code proves the write landed.
    {
        Json args = Json::object();
        args["variablesReference"] = ref;
        args["name"] = std::string("x");
        args["value"] = std::string("100");
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setVariable", args), emit));
        const Json* r = out.lastResponseTo("setVariable");
        ASSERT_NE(r, nullptr);
        ASSERT_TRUE(r->at("success").asBool()) << r->dump();
        EXPECT_EQ(r->at("body").at("value").asString(), "100") << r->dump();
    }
    {
        // setVariable on a name that doesn't exist fails with a message.
        Json args = Json::object();
        args["variablesReference"] = ref;
        args["name"] = std::string("nope");
        args["value"] = std::string("1");
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setVariable", args), emit));
        const Json* r = out.lastResponseTo("setVariable");
        EXPECT_FALSE(r->at("success").asBool()) << r->dump();
    }

    // continue → y = 100 + 5 + 12 = 117.
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", args), emit));
        const Json* exited = out.lastEvent("exited");
        ASSERT_NE(exited, nullptr) << out.dumpAll();
        EXPECT_EQ(exited->at("body").at("exitCode").asInt(), 117)
            << out.dumpAll();
        EXPECT_NE(out.lastEvent("terminated"), nullptr) << out.dumpAll();
    }

    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "disconnect", Json::object()), emit));
}

// next stays in the frame, stepIn deepens the stack into the callee, stepOut
// returns; a step against the wrong thread and a step after termination both
// fail with a message.
TEST(DapInspectionTests, steppingNextInOutAndRefusals) {
    TempProgram prog(
        "package test;\n"                                  // 1
        "public final class D {\n"                         // 2
        "    public static int32 helper(int32 v) {\n"      // 3
        "        int32 w = v + 1;\n"                       // 4
        "        return w;\n"                              // 5
        "    }\n"                                          // 6
        "    public static int32 run() {\n"                // 7
        "        int32 a = 1;\n"                           // 8   <- bp
        "        int32 b = helper(a);\n"                   // 9
        "        int32 c = b + 1;\n"                       // 10
        "        return c;\n"                              // 11
        "    }\n"
        "}\n");
    const int bpLine = 8;

    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;
    int threadId = startToBreakpoint(server, out, seq, prog, bpLine);
    ASSERT_GE(threadId, 0) << out.dumpAll();
    {
        auto frames = stackFrames(server, out, seq, threadId);
        ASSERT_FALSE(frames.empty());
        EXPECT_EQ(frames.front().at("line").asInt(), bpLine);
    }

    auto step = [&](const std::string& cmd) {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, cmd, args), emit))
            << out.dumpAll();
    };

    // next: same frame, the following line.
    step("next");
    {
        const Json* stopped = out.lastEvent("stopped");
        ASSERT_NE(stopped, nullptr) << out.dumpAll();
        EXPECT_EQ(stopped->at("body").at("reason").asString(), "step");
        auto frames = stackFrames(server, out, seq, threadId);
        ASSERT_FALSE(frames.empty());
        EXPECT_EQ(frames.size(), 1u) << out.dumpAll();
        EXPECT_EQ(frames.front().at("line").asInt(), 9) << out.dumpAll();
    }

    // stepIn: lands inside helper — the stack is two deep.
    step("stepIn");
    {
        const Json* stopped = out.lastEvent("stopped");
        ASSERT_NE(stopped, nullptr) << out.dumpAll();
        EXPECT_EQ(stopped->at("body").at("reason").asString(), "step");
        auto frames = stackFrames(server, out, seq, threadId);
        ASSERT_EQ(frames.size(), 2u) << out.dumpAll();
    }

    // stepOut: back to run's frame.
    step("stepOut");
    {
        const Json* stopped = out.lastEvent("stopped");
        ASSERT_NE(stopped, nullptr) << out.dumpAll();
        auto frames = stackFrames(server, out, seq, threadId);
        ASSERT_EQ(frames.size(), 1u) << out.dumpAll();
    }

    // A step naming a thread that isn't the stopped one is refused.
    {
        Json args = Json::object();
        args["threadId"] = threadId + 7;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "next", args), emit));
        const Json* r = out.lastResponseTo("next");
        EXPECT_FALSE(r->at("success").asBool()) << r->dump();
    }

    // continue to exit: a=1 → helper 2 → c=3.
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", args), emit));
        const Json* exited = out.lastEvent("exited");
        ASSERT_NE(exited, nullptr) << out.dumpAll();
        EXPECT_EQ(exited->at("body").at("exitCode").asInt(), 3);
    }

    // After termination stepping is refused, not crashed.
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "next", args), emit));
        const Json* r = out.lastResponseTo("next");
        EXPECT_FALSE(r->at("success").asBool()) << r->dump();
    }

    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "disconnect", Json::object()), emit));
}

// stopOnEntry stops before the first user line with reason "entry";
// setExceptionBreakpoints arms break-on-throw, the throw stops with reason
// "exception", and the program's own catch then runs to a clean exit.
TEST(DapInspectionTests, entryStopAndExceptionBreakpoint) {
    TempProgram prog(
        "package test;\n"                                          // 1
        "public final class D {\n"                                 // 2
        "    public static int32 run() {\n"                        // 3
        "        int32 r = 7;\n"                                   // 4
        "        try {\n"                                          // 5
        "            throw heap Exception(\"boom\");\n"            // 6
        "        } catch (Exception e) {\n"                        // 7
        "            r = 21;\n"                                    // 8
        "        }\n"                                              // 9
        "        return r;\n"                                      // 10
        "    }\n"
        "}\n");

    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;

    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "initialize", Json::object()), emit));
    {
        Json args = Json::object();
        args["entry-method"] = std::string("test.D.run");
        args["sourceRoot"] = prog.root.string();
        args["stopOnEntry"] = true;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "launch", args), emit));
    }
    {
        Json filters = Json::array();
        filters.push_back(Json(std::string("all")));
        Json args = Json::object();
        args["filters"] = std::move(filters);
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setExceptionBreakpoints", args), emit));
        const Json* r = out.lastResponseTo("setExceptionBreakpoints");
        ASSERT_NE(r, nullptr);
        EXPECT_TRUE(r->at("success").asBool()) << r->dump();
    }
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "configurationDone", Json::object()), emit));

    // First stop: entry.
    const Json* stopped = out.lastEvent("stopped");
    ASSERT_NE(stopped, nullptr) << out.dumpAll();
    EXPECT_EQ(stopped->at("body").at("reason").asString(), "entry")
        << out.dumpAll();
    const int threadId = stopped->at("body").at("threadId").asInt();

    // Resume → the armed throw stops with reason "exception".
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", args), emit));
        const Json* exStop = out.lastEvent("stopped");
        ASSERT_NE(exStop, nullptr) << out.dumpAll();
        EXPECT_EQ(exStop->at("body").at("reason").asString(), "exception")
            << out.dumpAll();
    }

    // Resume again → the catch handles it; clean exit through the handler.
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", args), emit));
        const Json* exited = out.lastEvent("exited");
        ASSERT_NE(exited, nullptr) << out.dumpAll();
        EXPECT_EQ(exited->at("body").at("exitCode").asInt(), 21)
            << out.dumpAll();
    }

    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "disconnect", Json::object()), emit));
}

// A conditional breakpoint stops only when `<local> <op> <literal>` holds
// at the breakpoint line; a never-true condition never stops the program.
TEST(DapInspectionTests, conditionalBreakpointStopsWhenConditionHolds) {
    TempProgram prog(
        "package test;\n"                          // 1
        "public final class D {\n"                 // 2
        "    public static int32 run() {\n"        // 3
        "        int32 acc = 0;\n"                 // 4
        "        int32 i = 0;\n"                   // 5
        "        while (i < 5) {\n"                // 6
        "            acc = acc + i;\n"             // 7   <- conditional bp
        "            i = i + 1;\n"                 // 8
        "        }\n"                              // 9
        "        return acc;\n"                    // 10
        "    }\n"
        "}\n");

    DapServer server;
    Collector out;
    auto emit = out.emit();
    int seq = 1;

    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "initialize", Json::object()), emit));
    {
        Json args = Json::object();
        args["entry-method"] = std::string("test.D.run");
        args["sourceRoot"] = prog.root.string();
        args["stopOnEntry"] = false;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "launch", args), emit));
    }
    {
        Json src = Json::object();
        src["path"] = prog.file.string();
        Json bp = Json::object();
        bp["line"] = 7;
        bp["condition"] = std::string("i == 3");
        Json bps = Json::array();
        bps.push_back(std::move(bp));
        Json args = Json::object();
        args["source"] = std::move(src);
        args["breakpoints"] = std::move(bps);
        ASSERT_TRUE(server.handle(
            makeRequest(seq++, "setBreakpoints", args), emit));
    }
    ASSERT_TRUE(server.handle(
        makeRequest(seq++, "configurationDone", Json::object()), emit));

    // The loop's earlier iterations (i = 0..2) must not stop; the first
    // stop arrives with i == 3.
    const Json* stopped = out.lastEvent("stopped");
    ASSERT_NE(stopped, nullptr) << out.dumpAll();
    EXPECT_EQ(stopped->at("body").at("reason").asString(), "breakpoint")
        << out.dumpAll();
    const int threadId = stopped->at("body").at("threadId").asInt();
    {
        Json args = Json::object();
        args["expression"] = std::string("i");
        args["frameId"] = 0;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "evaluate", args), emit));
        const Json* r = out.lastResponseTo("evaluate");
        ASSERT_TRUE(r->at("success").asBool()) << r->dump();
        EXPECT_EQ(r->at("body").at("result").asString(), "3") << r->dump();
    }

    // Continue: i == 3 never holds again → run to exit (0+1+2+3+4 = 10).
    {
        Json args = Json::object();
        args["threadId"] = threadId;
        ASSERT_TRUE(server.handle(makeRequest(seq++, "continue", args), emit));
        const Json* exited = out.lastEvent("exited");
        ASSERT_NE(exited, nullptr) << out.dumpAll();
        EXPECT_EQ(exited->at("body").at("exitCode").asInt(), 10)
            << out.dumpAll();
    }
    EXPECT_TRUE(server.handle(
        makeRequest(seq++, "disconnect", Json::object()), emit));
}

// A never-true condition suppresses every hit — the program runs straight
// to termination without a single stopped event.
