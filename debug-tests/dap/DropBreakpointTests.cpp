//
// CP7-6: drop / destructor breakpoints. With `~Object` declared as the root
// virtual destructor, every class has an inheritable/overridable `~T()`, and a
// `~T()` body carries normal statement safepoints under --debug-info. So "break
// when this instance is destructed" is just an ordinary source breakpoint on
// the destructor line — no special breakpoint type, no synthesized-wrapper
// machinery. These tests drive the real JIT DAP session to prove:
//
//   1. a breakpoint on a `~T()` body line parks when the instance drops, and
//   2. a class with NO explicit destructor still drops cleanly (regression
//      guard for `Object.drop()` now sitting in every class's drop chain).
//
#include <gtest/gtest.h>

#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "../TempProgram.h"

using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::debugtest::TempProgram;

namespace {

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

const Json* findResponse(const std::vector<Json>& log, const std::string& cmd) {
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == cmd) return &m;
    return nullptr;
}

// A class with an explicit destructor whose body has a breakable statement
// (line 8), plus an entry that creates a Heap-owned instance and lets it drop
// at scope exit (the `return`, line 14).
const char* kDtorProg =
    "package demo;\n"                       // 1
    "public class Foo {\n"                  // 2
    "    int32 v;\n"                         // 3
    "    Foo(int32 x) {\n"                   // 4
    "        this.v = x;\n"                  // 5
    "    }\n"                                // 6
    "    ~Foo() {\n"                         // 7
    "        int32 marker = this.v;\n"       // 8  <-- breakpoint here
    "    }\n"                                // 9
    "}\n"                                    // 10
    "public class Demo {\n"                  // 11
    "    public static int32 main() {\n"     // 12
    "        Foo f = new Foo(7);\n"          // 13
    "        return 0;\n"                    // 14  <-- f drops here -> ~Foo runs
    "    }\n"                                // 15
    "}\n";                                   // 16

// A class with NO explicit destructor; the entry still constructs and drops one.
const char* kNoDtorProg =
    "package demo;\n"
    "public class Bare {\n"
    "    int32 v;\n"
    "    Bare(int32 x) {\n"
    "        this.v = x;\n"
    "    }\n"
    "}\n"
    "public class Demo {\n"
    "    public static int32 main() {\n"
    "        Bare b = new Bare(9);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

void launchTo(DapServer& srv, std::vector<Json>& log, const TempProgram& p,
              int bpLine /* 0 = none */) {
    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Demo.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);
    if (bpLine > 0) {
        Json bpArgs = Json::object();
        Json src = Json::object();
        src["path"] = "Demo.cajeta";
        bpArgs["source"] = src;
        Json bps = Json::array();
        Json bp = Json::object();
        bp["line"] = bpLine;
        bps.push_back(bp);
        bpArgs["breakpoints"] = bps;
        drive(srv, req(3, "setBreakpoints", bpArgs), log);
    }
    drive(srv, req(4, "configurationDone", Json::object()), log);
}

} // namespace

// FR-9.1/9.2: a breakpoint on the destructor body parks when the instance is
// dropped at scope exit, via the ordinary safepoint/DebugController path.
TEST(DropBreakpoint, StopsOnDestructorBody) {
    TempProgram p("demo", "Demo.cajeta", kDtorProg);
    DapServer srv;
    std::vector<Json> log;

    launchTo(srv, log, p, /*bpLine=*/8);

    const Json* sb = findResponse(log, "setBreakpoints");
    ASSERT_NE(sb, nullptr);
    EXPECT_TRUE(sb->at("body").at("breakpoints")[0].at("verified").asBool())
        << "destructor line should resolve to a safepoint loc";

    EXPECT_EQ(countEvent(log, "stopped"), 1) << "should park in ~Foo() at drop";
    EXPECT_EQ(countEvent(log, "terminated"), 0);

    // The stopped frame is the destructor, at line 8.
    log.clear();
    drive(srv, req(5, "stackTrace", Json::object()), log);
    const Json* st = findResponse(log, "stackTrace");
    ASSERT_NE(st, nullptr);
    const Json& frames = st->at("body").at("stackFrames");
    ASSERT_GE(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("line").asInt(), 8);

    // Resuming runs the program to completion.
    log.clear();
    drive(srv, req(6, "continue", Json::object()), log);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
}

// Regression guard for the ~Object change: a class with no explicit ~T() still
// constructs and drops cleanly (Object.drop() now sits at the tail of every
// class's drop chain). No breakpoint -> runs straight to termination.
TEST(DropBreakpoint, ClassWithoutDestructorDropsCleanly) {
    TempProgram p("demo", "Demo.cajeta", kNoDtorProg);
    DapServer srv;
    std::vector<Json> log;

    launchTo(srv, log, p, /*bpLine=*/0);

    EXPECT_EQ(countEvent(log, "stopped"), 0);
    EXPECT_EQ(countEvent(log, "terminated"), 1);
    for (const auto& m : log) {
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited") {
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 0);
        }
    }
}
