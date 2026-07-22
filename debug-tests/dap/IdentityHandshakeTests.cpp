//
// resident-debug-server Unit 5 — compiler-identity handshake (plan 5.1.1,
// spec §5). A resident server must never serve sessions from a stale binary
// image: `initialize` re-stats the server's own executable against its
// startup snapshot, and honors the plugin's `compilerPath` (the binary the
// plugin INTENDS to be talking to). Any mismatch refuses the session
// cleanly and ends the request loop, so the launcher can respawn fresh.
//
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"

using cajeta::dap::DapServer;
using cajeta::dap::Json;

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

const Json* response(const std::vector<Json>& log, const std::string& cmd) {
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == cmd) return &m;
    return nullptr;
}

} // namespace

// 5.1.1a — the server's own binary changed under it: refuse + end the loop.
TEST(IdentityHandshake, ChangedSelfBinaryRefusesAndEndsLoop) {
    DapServer srv;
    srv.overrideSelfIdentityForTest("bogus-identity-from-before-a-rebuild");

    std::vector<Json> log;
    bool keepGoing = drive(srv, req(1, "initialize", Json::object()), log);
    EXPECT_FALSE(keepGoing) << "a stale server must exit for the respawn";
    const Json* resp = response(log, "initialize");
    ASSERT_NE(resp, nullptr);
    EXPECT_FALSE(resp->at("success").asBool());
}

// 5.1.1b — the plugin expects a DIFFERENT binary than the one running:
// refuse + end the loop (covers a changed compilerPath setting).
TEST(IdentityHandshake, CompilerPathMismatchRefusesAndEndsLoop) {
    DapServer srv;
    Json args = Json::object();
    args["compilerPath"] = "/definitely/not/this/test/binary";

    std::vector<Json> log;
    bool keepGoing = drive(srv, req(1, "initialize", args), log);
    EXPECT_FALSE(keepGoing);
    const Json* resp = response(log, "initialize");
    ASSERT_NE(resp, nullptr);
    EXPECT_FALSE(resp->at("success").asBool());
}

// 5.1.1c — a MATCHING compilerPath (this very executable) proceeds normally.
TEST(IdentityHandshake, MatchingCompilerPathProceeds) {
    DapServer srv;
    Json args = Json::object();
    args["compilerPath"] = DapServer::selfExePathForTest();

    std::vector<Json> log;
    bool keepGoing = drive(srv, req(1, "initialize", args), log);
    EXPECT_TRUE(keepGoing);
    const Json* resp = response(log, "initialize");
    ASSERT_NE(resp, nullptr);
    EXPECT_TRUE(resp->at("success").asBool());
    // The normal handshake continues: initialized event present.
    bool sawInitialized = false;
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "initialized") sawInitialized = true;
    EXPECT_TRUE(sawInitialized);
}
