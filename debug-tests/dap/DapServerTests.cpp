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

#include <cstdlib>
#include <map>
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
using cajeta::dap::variableJson;
using cajeta::dbg::StopEvent;
using cajeta::dbg::DbgLocTable;
using cajeta::dbg::DbgVar;
using cajeta::dbg::AllocClass;
using cajeta::dbg::OwnershipRole;
using cajeta::dbg::LifetimeState;
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

TEST(DapServerBuilders, VariableJsonCarriesFacets) {
    DbgVar v;
    v.name = "o";
    v.type = "demo.Foo";
    v.alloc = AllocClass::Heap;
    v.ownership = OwnershipRole::Owner;
    v.lifetime = LifetimeState::AboutToDrop;
    Json var = variableJson(v, "<demo.Foo@0x1>");

    EXPECT_EQ(var.at("name").asString(), "o");
    EXPECT_EQ(var.at("type").asString(), "demo.Foo");
    EXPECT_EQ(var.at("value").asString(), "<demo.Foo@0x1>");
    EXPECT_EQ(var.at("variablesReference").asInt(), 0);
    // Namespaced facet tags.
    ASSERT_TRUE(var.has("cajeta"));
    EXPECT_EQ(var.at("cajeta").at("alloc").asString(), "heap");
    EXPECT_EQ(var.at("cajeta").at("ownership").asString(), "owner");
    EXPECT_EQ(var.at("cajeta").at("lifetime").asString(), "about-to-drop");
    // A live (about-to-drop) binding is still editable -> no readOnly hint.
    EXPECT_FALSE(var.has("presentationHint"));
}

TEST(DapServerBuilders, VariableJsonMovedOutIsReadOnly) {
    DbgVar v;
    v.name = "moved";
    v.type = "demo.Foo";
    v.alloc = AllocClass::Heap;
    v.ownership = OwnershipRole::Owner;
    v.lifetime = LifetimeState::MovedOut;
    Json var = variableJson(v, "<demo.Foo@0x1>");

    EXPECT_EQ(var.at("cajeta").at("lifetime").asString(), "moved-out");
    ASSERT_TRUE(var.has("presentationHint"));
    const Json& attrs = var.at("presentationHint").at("attributes");
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_EQ(attrs[0].asString(), "readOnly");
}

TEST(DapServerBuilders, VariableJsonUnknownFacetsAreTagged) {
    DbgVar v;        // all-default facets: a plain value
    v.name = "x";
    v.type = "int32";
    Json var = variableJson(v, "5");
    // Unknown is emitted explicitly so the plugin renders neutral, not wrong.
    EXPECT_EQ(var.at("cajeta").at("alloc").asString(), "unknown");
    EXPECT_EQ(var.at("cajeta").at("ownership").asString(), "unknown");
    EXPECT_EQ(var.at("cajeta").at("lifetime").asString(), "unknown");
    EXPECT_FALSE(var.has("presentationHint"));
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

    // scopes -> a single "Locals" scope. variablesReference is an OPAQUE handle
    // (>=1; DAP reserves 0 for "no children"). We deliberately do NOT assert any
    // arithmetic relation to frameId — the contract is the round-trip:
    // variables(varsRef) returns this frame's locals (asserted just below).
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
    EXPECT_GE(varsRef, 1);

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

// ---- variable-inspection Unit 4: aggregate expansion + rendering ----

namespace {
// Lines:                  1                     2                   3
const char* kExpandProg =
    "package demo;\n"                                            // 1
    "public class Point { public int32 x; public int32 y; "
        "public Point(int32 a, int32 b) { this.x = a; this.y = b; } }\n" // 2
    "public class Holder { public Point p; public int32 m; "
        "public Holder() { this.p = heap Point(1, 2); this.m = 99; } }\n" // 3
    "public class Calc {\n"                                      // 4
    "    public static int32 main() {\n"                         // 5
    "        int32 n = 5;\n"                                     // 6
    "        String s = \"hi\";\n"                               // 7
    "        int32[] nums = [3, 7, 9];\n"                        // 8
    "        Holder h = heap Holder();\n"                        // 9
    "        int32[] big = [0,1,2,3,4,5,6,7,8,9];\n"             // 10
    "        return n;\n"                                        // 11 <-- breakpoint
    "    }\n"                                                    // 12
    "}\n";                                                       // 13

const Json* byName(const Json& arr, const std::string& n) {
    for (size_t i = 0; i < arr.size(); ++i)
        if (arr[i].at("name").asString() == n) return &arr[i];
    return nullptr;
}
} // namespace

class DapExpandSession : public ::testing::Test {
protected:
    TempProgram prog{"demo", "Calc.cajeta", kExpandProg};
    DapServer srv;
    int localsRef = 0;

    // initialize -> launch -> setBreakpoints -> configurationDone -> stopped,
    // then stackTrace -> scopes, leaving `localsRef` pointing at the Locals.
    void driveToStop(Json launchExtra) {
        std::vector<Json> log;
        drive(srv, req(1, "initialize", Json::object()), log);
        Json la = Json::object();
        la["entry-method"] = "demo.Calc.main";
        la["sourceRoot"] = prog.sourceRoot();
        for (const auto& kv : launchExtra.items()) la[kv.first] = kv.second;
        drive(srv, req(2, "launch", la), log);
        Json bpArgs = Json::object();
        Json src = Json::object();
        src["path"] = "Calc.cajeta";
        bpArgs["source"] = src;
        Json bps = Json::array();
        Json bp = Json::object();
        bp["line"] = 11;
        bps.push_back(bp);
        bpArgs["breakpoints"] = bps;
        drive(srv, req(3, "setBreakpoints", bpArgs), log);
        log.clear();
        drive(srv, req(4, "configurationDone", Json::object()), log);
        ASSERT_EQ(countEvent(log, "stopped"), 1);
        std::vector<Json> l2;
        drive(srv, req(5, "stackTrace", Json::object()), l2);
        int frameId = findResponse(l2, "stackTrace")->at("body")
            .at("stackFrames")[0].at("id").asInt();
        std::vector<Json> l3;
        Json sa = Json::object();
        sa["frameId"] = frameId;
        drive(srv, req(6, "scopes", sa), l3);
        localsRef = findResponse(l3, "scopes")->at("body").at("scopes")[0]
            .at("variablesReference").asInt();
    }

    void SetUp() override { driveToStop(Json::object()); }

    void TearDown() override {
        std::vector<Json> l;
        drive(srv, req(98, "disconnect", Json::object()), l);
    }

    // Issue variables(ref) and return the resulting array.
    Json varsOf(int ref) {
        std::vector<Json> l;
        Json a = Json::object();
        a["variablesReference"] = ref;
        drive(srv, req(50, "variables", a), l);
        return findResponse(l, "variables")->at("body").at("variables");
    }

    // Issue setVariable(ref, name := value) and return the whole response.
    Json setVar(int ref, const std::string& name, const std::string& value) {
        std::vector<Json> l;
        Json a = Json::object();
        a["variablesReference"] = ref;
        a["name"] = name;
        a["value"] = value;
        drive(srv, req(60, "setVariable", a), l);
        return *findResponse(l, "setVariable");
    }
};

// ---- variable-inspection Unit 5: editing scalar leaves through references ----

TEST_F(DapExpandSession, SetFieldThroughReference) {
    int hRef = byName(varsOf(localsRef), "h")->at("variablesReference").asInt();
    // set the scalar field m := 42 through the object handle
    Json resp = setVar(hRef, "m", "42");
    ASSERT_TRUE(resp.at("success").asBool());
    EXPECT_EQ(resp.at("body").at("value").asString(), "42");
    // the write landed: the object's field-peek summary reflects it
    EXPECT_EQ(byName(varsOf(localsRef), "h")->at("value").asString(), "{m=42}");
}

TEST_F(DapExpandSession, SetArrayElementThroughReference) {
    int numsRef = byName(varsOf(localsRef), "nums")->at("variablesReference").asInt();
    Json resp = setVar(numsRef, "[1]", "99");
    ASSERT_TRUE(resp.at("success").asBool());
    EXPECT_EQ(resp.at("body").at("value").asString(), "99");
    // re-expand: element [1] now reads 99, in place.
    Json elems = varsOf(numsRef);
    EXPECT_EQ(byName(elems, "[1]")->at("value").asString(), "99");
    EXPECT_EQ(byName(elems, "[0]")->at("value").asString(), "3");  // neighbour intact
}

TEST_F(DapExpandSession, NonPrimitiveTargetReadOnly) {
    int hRef = byName(varsOf(localsRef), "h")->at("variablesReference").asInt();
    // the Point field p is non-primitive — writing it must fail, not corrupt.
    Json resp = setVar(hRef, "p", "0");
    EXPECT_FALSE(resp.at("success").asBool());
    // p is unchanged and still expandable
    int pRef = byName(varsOf(hRef), "p")->at("variablesReference").asInt();
    EXPECT_NE(pRef, 0);
}

// A moved-out binding stays read-only (§6.1.3): `gone`'s ownership is
// transferred via `#gone`, so it is consumed and must never be written.
TEST(DapServerSession, MovedOutLeafReadOnly) {
    const char* prog =
        "package demo;\n"
        "public class Box { public int32 v; public Box(int32 x) { this.v = x; } }\n"
        "public class Demo {\n"
        "    public static int32 main() {\n"
        "        Box gone = heap Box(2);\n"     // 5
        "        Box taken = #gone;\n"          // 6  gone is now moved-out
        "        int32 n = 1;\n"                // 7
        "        return n + taken.v;\n"         // 8 <-- breakpoint
        "    }\n"
        "}\n";
    TempProgram p("demo", "Demo.cajeta", prog);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json la = Json::object();
    la["entry-method"] = "demo.Demo.main";
    la["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", la), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Demo.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);

    std::vector<Json> l2;
    drive(srv, req(5, "stackTrace", Json::object()), l2);
    int frameId = findResponse(l2, "stackTrace")->at("body")
        .at("stackFrames")[0].at("id").asInt();
    std::vector<Json> l3;
    Json sa = Json::object();
    sa["frameId"] = frameId;
    drive(srv, req(6, "scopes", sa), l3);
    int localsRef = findResponse(l3, "scopes")->at("body").at("scopes")[0]
        .at("variablesReference").asInt();

    // `gone` must be reported read-only in the variables view...
    Json locals;
    {
        std::vector<Json> l;
        Json a = Json::object();
        a["variablesReference"] = localsRef;
        drive(srv, req(7, "variables", a), l);
        locals = findResponse(l, "variables")->at("body").at("variables");
    }
    const Json* gone = nullptr;
    for (size_t i = 0; i < locals.size(); ++i)
        if (locals[i].at("name").asString() == "gone") gone = &locals[i];
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(gone->at("cajeta").at("lifetime").asString(), "moved-out");

    // ...and writing it is refused.
    std::vector<Json> l;
    Json a = Json::object();
    a["variablesReference"] = localsRef;
    a["name"] = "gone";
    a["value"] = "5";
    drive(srv, req(8, "setVariable", a), l);
    EXPECT_FALSE(findResponse(l, "setVariable")->at("success").asBool());

    std::vector<Json> l4;
    drive(srv, req(9, "disconnect", Json::object()), l4);
}

TEST_F(DapExpandSession, AggregateGetsNonZeroReference) {
    Json locals = varsOf(localsRef);
    // a primitive leaf carries ref 0; an array and an object each carry a ref.
    ASSERT_NE(byName(locals, "n"), nullptr);
    EXPECT_EQ(byName(locals, "n")->at("variablesReference").asInt(), 0);
    EXPECT_NE(byName(locals, "nums")->at("variablesReference").asInt(), 0);
    EXPECT_NE(byName(locals, "h")->at("variablesReference").asInt(), 0);
    // a String is a leaf — no children.
    EXPECT_EQ(byName(locals, "s")->at("variablesReference").asInt(), 0);
}

TEST_F(DapExpandSession, VariablesExpandsArray) {
    Json locals = varsOf(localsRef);
    int numsRef = byName(locals, "nums")->at("variablesReference").asInt();
    const Json elems = varsOf(numsRef);
    ASSERT_EQ(elems.size(), 3u);
    EXPECT_EQ(elems[0].at("name").asString(), "[0]");
    EXPECT_EQ(elems[0].at("value").asString(), "3");
    EXPECT_EQ(elems[1].at("value").asString(), "7");
    EXPECT_EQ(elems[2].at("value").asString(), "9");
}

TEST_F(DapExpandSession, VariablesExpandsObjectFields) {
    Json locals = varsOf(localsRef);
    int hRef = byName(locals, "h")->at("variablesReference").asInt();
    Json fields = varsOf(hRef);
    // declared-name field rows; the nested Point field carries its own ref.
    const Json* fp = byName(fields, "p");
    const Json* fm = byName(fields, "m");
    ASSERT_NE(fp, nullptr);
    ASSERT_NE(fm, nullptr);
    EXPECT_EQ(fm->at("value").asString(), "99");
    EXPECT_EQ(fm->at("variablesReference").asInt(), 0);
    int pRef = fp->at("variablesReference").asInt();
    EXPECT_NE(pRef, 0);
    // and the nested field expands one level down
    Json inner = varsOf(pRef);
    EXPECT_EQ(byName(inner, "x")->at("value").asString(), "1");
    EXPECT_EQ(byName(inner, "y")->at("value").asString(), "2");
}

TEST_F(DapExpandSession, RendersStringAndSummaries) {
    Json locals = varsOf(localsRef);
    // String value is its quoted text; array value is the inline collapse;
    // object value is the scalar-field peek.
    EXPECT_EQ(byName(locals, "s")->at("value").asString(), "\"hi\"");
    EXPECT_EQ(byName(locals, "nums")->at("value").asString(), "[3, 7, 9]");
    EXPECT_EQ(byName(locals, "h")->at("value").asString(), "{m=99}");
    // facet tags are still present on every local (§4.1.6).
    EXPECT_TRUE(byName(locals, "n")->at("cajeta").has("ownership"));
    EXPECT_TRUE(byName(locals, "n")->at("cajeta").has("lifetime"));
}

// A standalone session: page size honored, and a bogus value falls back.
TEST(DapServerSession, VariablesPagesLargeArrayByLaunchParam) {
    TempProgram p("demo", "Calc.cajeta", kExpandProg);

    auto localsRefFor = [&](DapServer& srv, Json launchExtra,
                            std::vector<Json>& scratch) -> int {
        drive(srv, req(1, "initialize", Json::object()), scratch);
        Json la = Json::object();
        la["entry-method"] = "demo.Calc.main";
        la["sourceRoot"] = p.sourceRoot();
        for (const auto& kv : launchExtra.items()) la[kv.first] = kv.second;
        drive(srv, req(2, "launch", la), scratch);
        Json bpArgs = Json::object();
        Json src = Json::object();
        src["path"] = "Calc.cajeta";
        bpArgs["source"] = src;
        Json bps = Json::array();
        Json bp = Json::object();
        bp["line"] = 11;
        bps.push_back(bp);
        bpArgs["breakpoints"] = bps;
        drive(srv, req(3, "setBreakpoints", bpArgs), scratch);
        drive(srv, req(4, "configurationDone", Json::object()), scratch);
        std::vector<Json> l2;
        drive(srv, req(5, "stackTrace", Json::object()), l2);
        int frameId = findResponse(l2, "stackTrace")->at("body")
            .at("stackFrames")[0].at("id").asInt();
        std::vector<Json> l3;
        Json sa = Json::object();
        sa["frameId"] = frameId;
        drive(srv, req(6, "scopes", sa), l3);
        return findResponse(l3, "scopes")->at("body").at("scopes")[0]
            .at("variablesReference").asInt();
    };
    auto varsOf = [](DapServer& srv, int ref) -> Json {
        std::vector<Json> l;
        Json a = Json::object();
        a["variablesReference"] = ref;
        drive(srv, req(50, "variables", a), l);
        return findResponse(l, "variables")->at("body").at("variables");
    };

    // pageSize = 4 over a 10-element array -> 4 element rows + 1 "more" node.
    {
        DapServer srv;
        std::vector<Json> scratch;
        Json extra = Json::object();
        extra["pageSize"] = 4;
        int lref = localsRefFor(srv, extra, scratch);
        Json locals = varsOf(srv, lref);
        int bigRef = byName(locals, "big")->at("variablesReference").asInt();
        ASSERT_NE(bigRef, 0);
        const Json page0 = varsOf(srv, bigRef);
        ASSERT_EQ(page0.size(), 5u);  // 4 elements + a remainder node
        EXPECT_EQ(page0[0].at("name").asString(), "[0]");
        EXPECT_EQ(page0[3].at("name").asString(), "[3]");
        const Json& more = page0[4];
        EXPECT_NE(more.at("variablesReference").asInt(), 0);  // expandable
        // expanding the remainder node continues where the page left off.
        const Json page1 = varsOf(srv, more.at("variablesReference").asInt());
        EXPECT_EQ(page1[0].at("name").asString(), "[4]");
        std::vector<Json> l;
        drive(srv, req(98, "disconnect", Json::object()), l);
    }
    // A nonsensical page size falls back to the default -> all 10, no "more".
    {
        DapServer srv;
        std::vector<Json> scratch;
        Json extra = Json::object();
        extra["pageSize"] = 0;
        int lref = localsRefFor(srv, extra, scratch);
        Json locals = varsOf(srv, lref);
        int bigRef = byName(locals, "big")->at("variablesReference").asInt();
        Json page = varsOf(srv, bigRef);
        EXPECT_EQ(page.size(), 10u);
        std::vector<Json> l;
        drive(srv, req(98, "disconnect", Json::object()), l);
    }
}

// References minted at one stop are invalid after resume; a fresh stop mints
// new ones (§3.1.1, §3.2.4).
TEST(DapServerSession, ReferencesDroppedOnResume) {
    const char* prog =
        "package demo;\n"
        "public class Calc {\n"
        "    public static int32 main() {\n"
        "        int32[] a1 = [1, 2];\n"    // 4
        "        int32 x = 10;\n"           // 5 <-- bp #1
        "        int32 y = 20;\n"           // 6 <-- bp #2
        "        return x + y;\n"           // 7
        "    }\n"
        "}\n";
    TempProgram p("demo", "Calc.cajeta", prog);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json la = Json::object();
    la["entry-method"] = "demo.Calc.main";
    la["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", la), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Calc.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    for (int line : {5, 6}) {
        Json bp = Json::object();
        bp["line"] = line;
        bps.push_back(bp);
    }
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);

    auto localsRefNow = [&]() -> int {
        std::vector<Json> l2;
        drive(srv, req(20, "stackTrace", Json::object()), l2);
        int frameId = findResponse(l2, "stackTrace")->at("body")
            .at("stackFrames")[0].at("id").asInt();
        std::vector<Json> l3;
        Json sa = Json::object();
        sa["frameId"] = frameId;
        drive(srv, req(21, "scopes", sa), l3);
        return findResponse(l3, "scopes")->at("body").at("scopes")[0]
            .at("variablesReference").asInt();
    };
    auto varsOf = [&](int ref) -> Json {
        std::vector<Json> l;
        Json a = Json::object();
        a["variablesReference"] = ref;
        drive(srv, req(22, "variables", a), l);
        return findResponse(l, "variables")->at("body").at("variables");
    };

    log.clear();
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    int ref1 = localsRefNow();
    Json locals1 = varsOf(ref1);
    int agg1 = byName(locals1, "a1")->at("variablesReference").asInt();
    ASSERT_NE(agg1, 0);

    // resume to the second breakpoint.
    log.clear();
    drive(srv, req(5, "continue", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);

    // Resume drops every handle from the prior stop: expanding the old
    // aggregate handle before refreshing scopes resolves to nothing (never to
    // stale or wrong data). Handle IDs themselves recycle — nextVarRef_ resets
    // each stop — which is fine; the contract is that a client refreshes scopes
    // on each stop, and a not-yet-reminted ref carries no data.
    EXPECT_EQ(varsOf(agg1).size(), 0u);

    // A fresh stop re-mints working handles: scopes -> locals -> a1's new
    // aggregate handle expands to its live elements.
    int ref2 = localsRefNow();
    Json locals2 = varsOf(ref2);
    int agg2 = byName(locals2, "a1")->at("variablesReference").asInt();
    ASSERT_NE(agg2, 0);
    const Json elems2 = varsOf(agg2);
    ASSERT_EQ(elems2.size(), 2u);
    EXPECT_EQ(elems2[0].at("value").asString(), "1");
    EXPECT_EQ(elems2[1].at("value").asString(), "2");

    log.clear();
    drive(srv, req(9, "continue", Json::object()), log);
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

    // CP6f-2b-ii: per-fiber stackTrace — ask for the stopped fiber's frames and
    // confirm its top frame is `worker` parked at line 4.
    log.clear();
    Json stArgs = Json::object();
    stArgs["threadId"] = stoppedTid;
    drive(srv, req(6, "stackTrace", stArgs), log);
    const Json* stResp = findResponse(log, "stackTrace");
    ASSERT_NE(stResp, nullptr);
    const Json& wf = stResp->at("body").at("stackFrames");
    ASSERT_GE(wf.size(), 1u) << "stopped fiber has no frames";
    EXPECT_EQ(wf[0].at("line").asInt(), 4);
    int workerFrameId = wf[0].at("id").asInt();

    // Round-trip the opaque handle: scopes(frameId) -> ref, variables(ref) shows
    // the worker's param x == 41. No assumption about ref's numeric value.
    log.clear();
    Json scArgs = Json::object();
    scArgs["frameId"] = workerFrameId;
    drive(srv, req(7, "scopes", scArgs), log);
    const Json* scResp2 = findResponse(log, "scopes");
    ASSERT_NE(scResp2, nullptr);
    int xRef = scResp2->at("body").at("scopes")[0].at("variablesReference").asInt();
    EXPECT_GE(xRef, 1);

    log.clear();
    Json vArgs = Json::object();
    vArgs["variablesReference"] = xRef;
    drive(srv, req(8, "variables", vArgs), log);
    const Json* vResp2 = findResponse(log, "variables");
    ASSERT_NE(vResp2, nullptr);
    const Json& wvars = vResp2->at("body").at("variables");
    std::string xVal;
    for (size_t i = 0; i < wvars.size(); ++i) {
        if (wvars[i].at("name").asString() == "x")
            xVal = wvars[i].at("value").asString();
    }
    EXPECT_EQ(xVal, "41") << "worker fiber's param x not readable via its frame";

    // Distinct fibers get distinct frame ids: main (thread 0) is parked at the
    // await on line 8; its top frame id must differ from the worker's.
    log.clear();
    Json stMain = Json::object();
    stMain["threadId"] = 0;
    drive(srv, req(9, "stackTrace", stMain), log);
    const Json* mainSt = findResponse(log, "stackTrace");
    ASSERT_NE(mainSt, nullptr);
    const Json& mf = mainSt->at("body").at("stackFrames");
    if (mf.size() >= 1u) {
        EXPECT_NE(mf[0].at("id").asInt(), workerFrameId)
            << "main and worker share a frameId — table not global";
    }

    // Resume to termination so the parked carrier unblocks before the DapServer
    // dtor join()s it (else deadlock). 41 + 1 = 42.
    log.clear();
    drive(srv, req(10, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
}

// CP6f-3: with exception breakpoints armed, a thrown exception parks the
// program with reason "exception" before the catch runs; continue then lets it
// be caught and run to termination.
TEST(DapServerSession, ExceptionBreakpointStopsAtThrow) {
    static const char* kThrowProg =
        "package demo;\n"
        "public class Calc {\n"
        "    public static int32 main() {\n"
        "        int32 result = 0;\n"
        "        try {\n"
        "            throw 99;\n"           // line 6 — armed exception parks here
        "        } catch (Exception e) {\n"
        "            result = 42;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Calc.cajeta", kThrowProg);
    DapServer srv;
    std::vector<Json> log;

    // initialize advertises an exception filter.
    drive(srv, req(1, "initialize", Json::object()), log);
    const Json* initResp = findResponse(log, "initialize");
    ASSERT_NE(initResp, nullptr);
    ASSERT_GE(initResp->at("body").at("exceptionBreakpointFilters").size(), 1u);

    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);

    // Arm all-throws.
    Json xbpArgs = Json::object();
    Json filters = Json::array();
    filters.push_back(Json(std::string("all")));
    xbpArgs["filters"] = filters;
    drive(srv, req(3, "setExceptionBreakpoints", xbpArgs), log);

    // configurationDone runs the program; the throw must park with
    // reason=exception (not yet terminated).
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(countEvent(log, "terminated"), 0);
    std::string reason;
    for (const auto& m : log) {
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "stopped") {
            reason = m.at("body").at("reason").asString();
        }
    }
    EXPECT_EQ(reason, "exception");

    // continue -> the throw is caught, result becomes 42, program exits 42.
    log.clear();
    drive(srv, req(5, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
    for (const auto& m : log) {
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited") {
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
        }
    }
}

// CP6f-3: without exception breakpoints, a caught throw does NOT park.
TEST(DapServerSession, ThrowDoesNotStopWhenExceptionsNotArmed) {
    static const char* kThrowProg =
        "package demo;\n"
        "public class Calc {\n"
        "    public static int32 main() {\n"
        "        int32 result = 0;\n"
        "        try {\n"
        "            throw 99;\n"
        "        } catch (Exception e) {\n"
        "            result = 42;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Calc.cajeta", kThrowProg);
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

// Contract CHANGED by resident-debug-server 1.2.1 (2026-07-21): disconnect
// ends the SESSION and keeps the request loop alive for the next one; the
// PROCESS ends at stdin EOF (run()'s read loop) or when the launcher kills
// it. MultiSessionTests pin the full reset; this pins the loop verdict.
TEST(DapServerSession, DisconnectEndsSessionNotLoop) {
    DapServer srv;
    std::vector<Json> log;
    bool keepGoing = drive(srv, req(1, "disconnect", Json::object()), log);
    EXPECT_TRUE(keepGoing);
    EXPECT_NE(findResponse(log, "disconnect"), nullptr);
}

// --- stopOnEntry (run-config-ergonomics Unit 4 / spec §5) -------------------
// The flag has always been persisted by the plugin and sent on the launch
// request, but the server never read it, so the checkbox did nothing. These
// pin the behaviour it advertises.

// 4.1.1 / spec 5.2.1 — halts at the entry method before its body runs.
TEST(DapServerSession, StopOnEntryHaltsBeforeTheFirstStatement) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    launchArgs["stopOnEntry"] = true;
    drive(srv, req(2, "launch", launchArgs), log);
    // No breakpoints at all: the only thing that can park it is entry.
    drive(srv, req(3, "configurationDone", Json::object()), log);

    EXPECT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(countEvent(log, "terminated"), 0);  // parked, not done

    // The reason distinguishes an entry stop from a breakpoint stop.
    const Json* stop = nullptr;
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "stopped") { stop = &m; break; }
    ASSERT_NE(stop, nullptr);
    EXPECT_EQ(stop->at("body").at("reason").asString(), std::string("entry"));

    // Always resume before the session is destroyed: ~JitDebugSession join()s
    // the program thread, and a thread still parked at a stop never returns.
    drive(srv, req(4, "continue", Json::object()), log);
}

// 4.1.2 — the stop is usable: a frame at the entry method's first body line.
TEST(DapServerSession, StopOnEntryReportsAFrameAtTheEntryMethod) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    launchArgs["stopOnEntry"] = true;
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);

    log.clear();
    drive(srv, req(4, "stackTrace", Json::object()), log);
    const Json* st = findResponse(log, "stackTrace");
    ASSERT_NE(st, nullptr);
    const Json& frames = st->at("body").at("stackFrames");
    ASSERT_GT(frames.size(), 0u);
    // Line 4 is `int32 a = 6;` — the first executable statement, NOT yet run.
    EXPECT_EQ(frames[0].at("line").asInt(), 4);

    drive(srv, req(5, "continue", Json::object()), log);  // resume before teardown
}

// 4.1.3 / spec 5.2.2 — resume from an entry stop with no breakpoints runs out.
TEST(DapServerSession, StopOnEntryThenContinueRunsToTermination) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    launchArgs["stopOnEntry"] = true;
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    ASSERT_EQ(countEvent(log, "stopped"), 1);

    log.clear();
    drive(srv, req(4, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
    // Entry must fire ONCE, not at every subsequent safepoint.
    EXPECT_EQ(countEvent(log, "stopped"), 0);
}

// 4.1.4 / spec 5.2.3 — the existing-behaviour guard.
TEST(DapServerSession, WithoutStopOnEntryTheProgramDoesNotHaltAtEntry) {
    TempProgram p("demo", "Calc.cajeta", kProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Calc.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    launchArgs["stopOnEntry"] = false;
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);

    EXPECT_EQ(countEvent(log, "stopped"), 0);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
}

// --- stepping (dap-stepping Unit 2 / spec §3, §5.1) --------------------------
// next/stepIn/stepOut over the pending-step controller mode. One program with
// a user callee (add) and a stdlib call (String concat), so step-over is
// exercised across both kinds of deeper safepoints.

namespace {

const char* kStepProg =
    "package demo;\n"
    "public class Steps {\n"
    "    public static int32 add(int32 x, int32 y) {\n"
    "        int32 s = x + y;\n"          // line 4
    "        return s;\n"                 // line 5
    "    }\n"
    "    public static int32 main() {\n"
    "        int32 a = 6;\n"              // line 8
    "        int32 b = add(a, 7);\n"      // line 9  (user call)
    "        String msg = \"b=\" + b;\n"  // line 10 (stdlib concat)
    "        int32 c = b + 1;\n"          // line 11
    "        return c;\n"                 // line 12
    "    }\n"
    "}\n";

// Boot a session on `p` (entry demo.Steps.main) with breakpoints at `lines`,
// driving through configurationDone; `log` ends holding the first stop.
void bootToFirstStop(DapServer& srv, std::vector<Json>& log,
                     const TempProgram& p, const std::vector<int>& lines) {
    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Steps.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Steps.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    for (int line : lines) {
        Json bp = Json::object();
        bp["line"] = line;
        bps.push_back(bp);
    }
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
}

const Json* lastEvent(const std::vector<Json>& log, const std::string& ev) {
    const Json* found = nullptr;
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == ev) found = &m;
    return found;
}

// Send a step verb for `threadId`, capturing everything it emits.
void step(DapServer& srv, std::vector<Json>& log, int seq,
          const std::string& verb, int threadId) {
    Json args = Json::object();
    args["threadId"] = threadId;
    drive(srv, req(seq, verb, args), log);
}

// Innermost frame (line, function name) of `threadId` via stackTrace.
std::pair<int, std::string> innermostFrame(DapServer& srv, int seq,
                                           int threadId) {
    std::vector<Json> log;
    Json args = Json::object();
    args["threadId"] = threadId;
    drive(srv, req(seq, "stackTrace", args), log);
    const Json* resp = findResponse(log, "stackTrace");
    if (!resp) return {-1, ""};
    const Json& frames = resp->at("body").at("stackFrames");
    if (frames.size() == 0) return {-1, ""};
    return {frames[0].at("line").asInt(), frames[0].at("name").asString()};
}

} // namespace

// 2.1.1 `next` at a breakpoint stop: success response, then a stopped event
// with reason "step" on the same thread, parked at the next line.
TEST(DapServerSession, NextStopsAtTheNextLineWithReasonStep) {
    TempProgram p("demo", "Steps.cajeta", kStepProg);
    DapServer srv;
    std::vector<Json> log;
    bootToFirstStop(srv, log, p, {8});
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    const Json* stop = lastEvent(log, "stopped");
    EXPECT_EQ(stop->at("body").at("reason").asString(), "breakpoint");
    const int tid = stop->at("body").at("threadId").asInt();

    log.clear();
    step(srv, log, 5, "next", tid);
    const Json* resp = findResponse(log, "next");
    ASSERT_NE(resp, nullptr);
    EXPECT_TRUE(resp->at("success").asBool());
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    const Json* stepStop = lastEvent(log, "stopped");
    EXPECT_EQ(stepStop->at("body").at("reason").asString(), "step");
    EXPECT_EQ(stepStop->at("body").at("threadId").asInt(), tid);
    auto [line, func] = innermostFrame(srv, 6, tid);
    EXPECT_EQ(line, 9);
    EXPECT_NE(func.find("main"), std::string::npos);

    // Still parked: disconnect resumes the carrier and joins the
    // program thread — without it the fixture destructor deadlocks.
    log.clear();
    drive(srv, req(99, "disconnect", Json::object()), log);
}

// 2.1.2 `next` over a statement that calls a method — the callee's deeper
// safepoints don't park. Twice: over a user call (add), then over a stdlib
// call (String concat).
TEST(DapServerSession, NextOverACallStopsAtTheCallersNextLine) {
    TempProgram p("demo", "Steps.cajeta", kStepProg);
    DapServer srv;
    std::vector<Json> log;
    bootToFirstStop(srv, log, p, {9});
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    const int tid = lastEvent(log, "stopped")->at("body").at("threadId").asInt();

    log.clear();
    step(srv, log, 5, "next", tid);   // over add(a, 7)
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(lastEvent(log, "stopped")->at("body").at("reason").asString(),
              "step");
    auto [line1, func1] = innermostFrame(srv, 6, tid);
    EXPECT_EQ(line1, 10);
    EXPECT_NE(func1.find("main"), std::string::npos);

    log.clear();
    step(srv, log, 7, "next", tid);   // over the stdlib concat
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    auto [line2, func2] = innermostFrame(srv, 8, tid);
    EXPECT_EQ(line2, 11);
    EXPECT_NE(func2.find("main"), std::string::npos);

    // Still parked: disconnect resumes the carrier and joins the
    // program thread — without it the fixture destructor deadlocks.
    log.clear();
    drive(srv, req(99, "disconnect", Json::object()), log);
}

// 2.1.3 `stepIn` at a call stops at the callee's first line; `stepOut` from
// the callee stops back in the caller.
TEST(DapServerSession, StepInEntersCalleeAndStepOutReturnsToCaller) {
    TempProgram p("demo", "Steps.cajeta", kStepProg);
    DapServer srv;
    std::vector<Json> log;
    bootToFirstStop(srv, log, p, {9});
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    const int tid = lastEvent(log, "stopped")->at("body").at("threadId").asInt();

    log.clear();
    step(srv, log, 5, "stepIn", tid);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(lastEvent(log, "stopped")->at("body").at("reason").asString(),
              "step");
    auto [inLine, inFunc] = innermostFrame(srv, 6, tid);
    EXPECT_EQ(inLine, 4);
    EXPECT_NE(inFunc.find("add"), std::string::npos);

    log.clear();
    step(srv, log, 7, "stepOut", tid);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(lastEvent(log, "stopped")->at("body").at("reason").asString(),
              "step");
    auto [outLine, outFunc] = innermostFrame(srv, 8, tid);
    EXPECT_EQ(outLine, 10);
    EXPECT_NE(outFunc.find("main"), std::string::npos);

    // Still parked: disconnect resumes the carrier and joins the
    // program thread — without it the fixture destructor deadlocks.
    log.clear();
    drive(srv, req(99, "disconnect", Json::object()), log);
}

// 2.1.4 `next` on a method's last line: the frame returns, and the stop lands
// in the caller (the depth <= origin rule).
TEST(DapServerSession, NextOnAMethodsLastLineStopsInTheCaller) {
    TempProgram p("demo", "Steps.cajeta", kStepProg);
    DapServer srv;
    std::vector<Json> log;
    bootToFirstStop(srv, log, p, {5});   // return s; in add
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    const int tid = lastEvent(log, "stopped")->at("body").at("threadId").asInt();

    log.clear();
    step(srv, log, 5, "next", tid);
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(lastEvent(log, "stopped")->at("body").at("reason").asString(),
              "step");
    auto [line, func] = innermostFrame(srv, 6, tid);
    EXPECT_EQ(line, 10);
    EXPECT_NE(func.find("main"), std::string::npos);

    // Still parked: disconnect resumes the carrier and joins the
    // program thread — without it the fixture destructor deadlocks.
    log.clear();
    drive(srv, req(99, "disconnect", Json::object()), log);
}

// 2.1.5 A step with no parked stop fails cleanly with a readable message and
// leaves the session usable (a later launch still stops at its breakpoint).
TEST(DapServerSession, StepWithNoParkedStopFailsCleanly) {
    TempProgram p("demo", "Steps.cajeta", kStepProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Steps.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);

    log.clear();
    step(srv, log, 3, "next", 0);
    const Json* resp = findResponse(log, "next");
    ASSERT_NE(resp, nullptr);
    EXPECT_FALSE(resp->at("success").asBool());
    EXPECT_FALSE(resp->at("body").asString().empty());
    EXPECT_EQ(countEvent(log, "stopped"), 0);

    // Session unchanged: configure a breakpoint and run — it still parks.
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Steps.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(4, "setBreakpoints", bpArgs), log);
    drive(srv, req(5, "configurationDone", Json::object()), log);
    EXPECT_EQ(countEvent(log, "stopped"), 1);

    // Still parked: disconnect resumes the carrier and joins the
    // program thread — without it the fixture destructor deadlocks.
    log.clear();
    drive(srv, req(99, "disconnect", Json::object()), log);
}

// 2.1.6 Breakpoint wins over a pending step: stepping over a call whose body
// holds an armed breakpoint stops THERE with reason "breakpoint", and no step
// stop ever follows.
TEST(DapServerSession, BreakpointWinsOverAPendingStep) {
    TempProgram p("demo", "Steps.cajeta", kStepProg);
    DapServer srv;
    std::vector<Json> log;
    bootToFirstStop(srv, log, p, {9, 4});   // at the call + inside add
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    const int tid = lastEvent(log, "stopped")->at("body").at("threadId").asInt();
    auto [line0, func0] = innermostFrame(srv, 90, tid);
    EXPECT_EQ(line0, 9);   // first stop is the call site

    log.clear();
    step(srv, log, 5, "next", tid);   // pending step-over; bp at line 4 is deeper
    ASSERT_EQ(countEvent(log, "stopped"), 1);
    EXPECT_EQ(lastEvent(log, "stopped")->at("body").at("reason").asString(),
              "breakpoint");
    auto [line, func] = innermostFrame(srv, 6, tid);
    EXPECT_EQ(line, 4);
    EXPECT_NE(func.find("add"), std::string::npos);

    // The pending step was cleared: continue runs to termination, no third stop.
    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "stopped"), 0);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
}

// --- environment variables (run-config-ergonomics Unit 6 / spec §4) ---------
// The JIT runs IN-PROCESS, so applying the launch environment mutates this
// very process. That makes restore (4.1.4) a correctness requirement, not
// hygiene: without it the first session's variables leak into every session
// after it, and into the DAP server itself.

namespace {

const char* kEnvProg =
    "package demo;\n"
    "public class Env {\n"
    "    public static int32 main() {\n"
    "        String v = System.env.get(\"CAJETA_DAP_TEST_VAR\");\n"
    "        if (v == null) { return 0; }\n"
    "        if (v.equals(\"one\")) { return 1; }\n"
    "        if (v.equals(\"two\")) { return 2; }\n"
    "        return 9;\n"
    "    }\n"
    "}\n";

const char* kVar = "CAJETA_DAP_TEST_VAR";

// The debuggee reports what it saw through its exit code, so a single int
// distinguishes unset (0) from each configured value.
int exitCodeOf(const std::vector<Json>& log) {
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            return m.at("body").at("exitCode").asInt();
    return -1;
}

// Run one whole session to termination and report what the debuggee observed.
// `env` entries are sent as the DAP `env` object; a null `env` sends none at
// all, which must remain distinct from sending an empty one (4.2.5 / 6.1.6).
int runWithEnv(const TempProgram& p,
               const std::map<std::string, std::string>* env,
               const bool* inheritSystemEnv = nullptr) {
    DapServer srv;
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Env.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    if (env) {
        Json e = Json::object();
        for (const auto& kv : *env) e[kv.first] = kv.second;
        launchArgs["env"] = std::move(e);
    }
    if (inheritSystemEnv) launchArgs["inheritSystemEnv"] = *inheritSystemEnv;
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    drive(srv, req(4, "disconnect", Json::object()), log);
    return exitCodeOf(log);
}

// Set/clear a variable in THIS process, so a test can stage the "inherited"
// environment the server is expected to overlay or suppress.
void putVar(const char* name, const char* value) {
    if (value) ::setenv(name, value, 1); else ::unsetenv(name);
}

} // namespace

// 6.1.1 / spec 4.2.1 — a configured entry reaches System.env.get.
TEST(DapServerEnvironment, ConfiguredVariableIsVisibleToTheDebuggee) {
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    putVar(kVar, nullptr);
    std::map<std::string, std::string> env{{kVar, "one"}};
    EXPECT_EQ(runWithEnv(p, &env), 1);
}

// 6.1.2 / spec 4.2.2 — the configuration wins over the inherited value.
TEST(DapServerEnvironment, ConfiguredEntryOverridesAnInheritedVariable) {
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    putVar(kVar, "two");
    std::map<std::string, std::string> env{{kVar, "one"}};
    EXPECT_EQ(runWithEnv(p, &env), 1);
    // And the server put the inherited value back (4.1.4).
    EXPECT_STREQ(::getenv(kVar), "two");
    putVar(kVar, nullptr);
}

// 6.1.3 / spec 4.2.3 — with inheritance off, an undeclared variable that
// exists in the server's environment is NOT visible to the debuggee.
TEST(DapServerEnvironment, InheritanceOffHidesUndeclaredVariables) {
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    putVar(kVar, "two");
    std::map<std::string, std::string> env{{"CAJETA_DAP_TEST_OTHER", "x"}};
    const bool inherit = false;
    EXPECT_EQ(runWithEnv(p, &env, &inherit), 0);
    EXPECT_STREQ(::getenv(kVar), "two");
    putVar(kVar, nullptr);
}

// 6.1.4 / spec 4.2.4 — THE contamination test. Two sessions in one process:
// the second must not observe the first's variable.
TEST(DapServerEnvironment, RestoreKeepsOneSessionOutOfTheNext) {
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    putVar(kVar, nullptr);

    std::map<std::string, std::string> first{{kVar, "one"}};
    EXPECT_EQ(runWithEnv(p, &first), 1);
    EXPECT_EQ(::getenv(kVar), nullptr);   // unset again after the session

    EXPECT_EQ(runWithEnv(p, nullptr), 0); // second session: never set
}

// 6.1.5 — restore must not depend on a clean shutdown. Here the server is
// destroyed with the session still live and no disconnect ever sent; a
// destructor-driven guard restores, an explicit call at the end of a happy
// path would not.
TEST(DapServerEnvironment, RestoreHoldsWhenTheSessionEndsAbnormally) {
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    putVar(kVar, "two");
    {
        DapServer srv;
        std::vector<Json> log;
        drive(srv, req(1, "initialize", Json::object()), log);
        Json launchArgs = Json::object();
        launchArgs["entry-method"] = "demo.Env.main";
        launchArgs["sourceRoot"] = p.sourceRoot();
        Json e = Json::object();
        e[kVar] = "one";
        launchArgs["env"] = std::move(e);
        drive(srv, req(2, "launch", launchArgs), log);
        drive(srv, req(3, "configurationDone", Json::object()), log);
        // No disconnect, no terminate — just drop the server.
    }
    EXPECT_STREQ(::getenv(kVar), "two");
    putVar(kVar, nullptr);
}

// 6.1.6 / spec 4.2.5 — no `env` in the request changes nothing. The inherited
// variable stays visible, which is the pre-feature behaviour.
TEST(DapServerEnvironment, NoEnvInTheRequestLeavesTheEnvironmentUntouched) {
    TempProgram p("demo", "Env.cajeta", kEnvProg);
    putVar(kVar, "two");
    EXPECT_EQ(runWithEnv(p, nullptr), 2);
    EXPECT_STREQ(::getenv(kVar), "two");
    putVar(kVar, nullptr);
}
