//
// CP7 integration sample: drive one real program through the JIT DAP session
// and assert the `variables` response carries the right memory facets end to
// end (compiler classify -> runtime frame chain -> host derive -> DAP wire).
// Exercises stack vs heap allocation, owner vs borrow ownership, and a
// moved-out binding. (`shared` is the XPU placement axis and is deferred — the
// call sites pass isShared=false today — so it is intentionally not covered.)
//
#include <gtest/gtest.h>

#include <iostream>
#include <map>
#include <string>
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
    r["seq"] = seq; r["type"] = "request"; r["command"] = command;
    r["arguments"] = std::move(args);
    return r;
}
const Json* findResponse(const std::vector<Json>& log, const std::string& cmd) {
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == cmd) return &m;
    return nullptr;
}

struct Facets { std::string alloc, ownership, lifetime; };

// 1 package demo;
// 2 public class Box {
// 3     int32 v;
// 4     Box(int32 x) { this.v = x; }
// 5 }
// 6 public class Demo {
// 7     public static int32 main() {
// 8         int32 n = 5;
// 9         Box owned = new Box(1);
// 10        Box alias = owned;
// 11        Box gone = new Box(2);
// 12        Box taken = #gone;
// 13        int32 here = 0;
// 14        return n + owned.v + alias.v + taken.v + here;   <-- breakpoint
// 15    }
// 16 }
const char* kProg =
    "package demo;\n"
    "public class Box {\n"
    "    int32 v;\n"
    "    Box(int32 x) { this.v = x; }\n"
    "}\n"
    "public class Demo {\n"
    "    public static int32 main() {\n"
    "        int32 n = 5;\n"
    "        Box owned = new Box(1);\n"
    "        Box alias = owned;\n"
    "        Box gone = new Box(2);\n"
    "        Box taken = #gone;\n"
    "        int32 here = 0;\n"
    "        return n + owned.v + alias.v + taken.v + here;\n"
    "    }\n"
    "}\n";

// A non-primitive parameter is a borrow (the callee doesn't own it). Breakpoint
// inside `peek` (line 8) where `b` is in scope.
// 7    static int32 peek(Box b) {
// 8        int32 r = b.v;          <-- breakpoint; b is a borrowed param
const char* kBorrowProg =
    "package demo;\n"
    "public class Box {\n"
    "    int32 v;\n"
    "    Box(int32 x) { this.v = x; }\n"
    "}\n"
    "public class Demo {\n"
    "    static int32 peek(Box b) {\n"
    "        int32 r = b.v;\n"
    "        return r;\n"
    "    }\n"
    "    public static int32 main() {\n"
    "        Box owned = new Box(8);\n"
    "        return peek(owned);\n"
    "    }\n"
    "}\n";

// Run to the breakpoint and collect each local's facets by name.
std::map<std::string, Facets> facetsAtStop(TempProgram& p, int line) {
    DapServer srv;
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Demo.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);
    Json bpArgs = Json::object();
    Json src = Json::object(); src["path"] = "Demo.cajeta"; bpArgs["source"] = src;
    Json bps = Json::array(); Json bp = Json::object(); bp["line"] = line;
    bps.push_back(bp); bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);

    std::map<std::string, Facets> out;
    log.clear();
    drive(srv, req(5, "stackTrace", Json::object()), log);
    const Json* st = findResponse(log, "stackTrace");
    if (!st) return out;
    int frameId = st->at("body").at("stackFrames")[0].at("id").asInt();
    log.clear();
    Json scopesArgs = Json::object(); scopesArgs["frameId"] = frameId;
    drive(srv, req(6, "scopes", scopesArgs), log);
    const Json* sc = findResponse(log, "scopes");
    if (!sc) return out;
    int varsRef = sc->at("body").at("scopes")[0].at("variablesReference").asInt();
    log.clear();
    Json varsArgs = Json::object(); varsArgs["variablesReference"] = varsRef;
    drive(srv, req(7, "variables", varsArgs), log);
    const Json* vr = findResponse(log, "variables");
    if (!vr) return out;
    const Json& vars = vr->at("body").at("variables");
    for (size_t i = 0; i < vars.size(); ++i) {
        const Json& v = vars[i];
        std::string nm = v.at("name").asString();
        Facets f;
        if (v.has("cajeta")) {
            f.alloc = v.at("cajeta").at("alloc").asString();
            f.ownership = v.at("cajeta").at("ownership").asString();
            f.lifetime = v.at("cajeta").at("lifetime").asString();
        }
        out[nm] = f;
    }
    // Resume so the program runs to completion before the DapServer is
    // destroyed — otherwise ~DapServer()'s session join() blocks on the still-
    // parked carrier (the source of the original hang here).
    log.clear();
    drive(srv, req(8, "continue", Json::object()), log);
    return out;
}

} // namespace

TEST(FacetIntegration, AllAxesAtAStop) {
    TempProgram p("demo", "Demo.cajeta", kProg);
    auto f = facetsAtStop(p, /*line=*/14);

    // Diagnostic dump (visible in test output) — the ground truth for the axes.
    std::cout << "=== facets at stop ===\n";
    for (auto& [name, fc] : f) {
        std::cout << "  " << name << ": alloc=" << fc.alloc
                  << " ownership=" << fc.ownership
                  << " lifetime=" << fc.lifetime << "\n";
    }

    ASSERT_FALSE(f.empty()) << "no variables captured at the stop";

    // Stack primitive: stack allocation, plain value, live.
    ASSERT_TRUE(f.count("n"));
    EXPECT_EQ(f["n"].alloc, "stack");
    EXPECT_EQ(f["n"].ownership, "unknown");
    EXPECT_EQ(f["n"].lifetime, "live");

    // Heap-owned object: heap allocation, owner, scheduled to drop.
    ASSERT_TRUE(f.count("owned"));
    EXPECT_EQ(f["owned"].alloc, "heap");
    EXPECT_EQ(f["owned"].ownership, "owner");
    EXPECT_EQ(f["owned"].lifetime, "about-to-drop");

    // Moved-out binding: ownership transferred via #gone -> consumed.
    ASSERT_TRUE(f.count("gone"));
    EXPECT_EQ(f["gone"].lifetime, "moved-out");

    // The recipient of the move owns it now.
    ASSERT_TRUE(f.count("taken"));
    EXPECT_EQ(f["taken"].alloc, "heap");
    EXPECT_EQ(f["taken"].ownership, "owner");
}

// The borrow ownership role, exercised via a non-primitive parameter (the
// callee borrows it; the caller keeps ownership).
TEST(FacetIntegration, BorrowParameter) {
    TempProgram p("demo", "Demo.cajeta", kBorrowProg);
    auto f = facetsAtStop(p, /*line=*/8);

    std::cout << "=== facets at peek() stop ===\n";
    for (auto& [name, fc] : f) {
        std::cout << "  " << name << ": alloc=" << fc.alloc
                  << " ownership=" << fc.ownership
                  << " lifetime=" << fc.lifetime << "\n";
    }

    ASSERT_TRUE(f.count("b"));
    EXPECT_EQ(f["b"].alloc, "heap");
    EXPECT_EQ(f["b"].ownership, "borrow");
    EXPECT_EQ(f["b"].lifetime, "live");
}
