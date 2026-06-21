// Tests for the `cajeta mcp` server transport skeleton (cajeta-mcp U1):
// JSON-RPC 2.0 over newline-delimited stdio, lifecycle (initialize /
// notifications/initialized / tools/list), and error envelopes.

#include "gtest/gtest.h"
#include "cajeta/mcp/McpServer.h"
#include "cajeta/buildtool/repo/TarZstd.h"

#include <llvm/Support/Base64.h>

#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using cajeta::dap::Json;
using cajeta::mcp::McpServer;

namespace {

// Feed `input` (newline-delimited requests) through a fresh server and return
// the parsed response objects (one per non-empty output line).
std::vector<Json> runLines(const std::string& input) {
    McpServer server;
    std::istringstream in(input);
    std::ostringstream out;
    server.run(in, out);
    std::vector<Json> responses;
    std::istringstream outStream(out.str());
    std::string line;
    while (std::getline(outStream, line)) {
        if (line.empty()) continue;
        bool ok = false;
        Json j = Json::parse(line, &ok);
        if (ok) responses.push_back(j);
    }
    return responses;
}

} // namespace

// 1.1.1
TEST(McpServerTests, initializeReturnsServerInfo) {
    auto r = runLines(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].at("jsonrpc").asString(), "2.0");
    EXPECT_EQ(r[0].at("id").asInt(), 1);
    const Json& res = r[0].at("result");
    EXPECT_TRUE(res.has("protocolVersion"));
    EXPECT_TRUE(res.at("capabilities").has("tools"));
    EXPECT_EQ(res.at("serverInfo").at("name").asString(), "cajeta");
}

// 1.1.2
TEST(McpServerTests, toolsListReturnsManifest) {
    auto r = runLines("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
    ASSERT_EQ(r.size(), 1u);
    const Json& tools = r[0].at("result").at("tools");
    ASSERT_TRUE(tools.isArray());
    std::set<std::string> names;
    for (size_t i = 0; i < tools.size(); i++) {
        names.insert(tools[i].at("name").asString());
    }
    EXPECT_EQ(names, (std::set<std::string>{
        "searchSkills", "getSkills", "listSkills", "compile", "jit_execute"}));
}

// 1.1.3
TEST(McpServerTests, unknownMethodIsJsonRpcError) {
    auto r = runLines("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"bogus/method\"}\n");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].has("error"));
    EXPECT_EQ(r[0].at("error").at("code").asInt(), -32601);
}

// 1.1.4
TEST(McpServerTests, malformedJsonIsParseError) {
    auto r = runLines("this is not json\n");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].has("error"));
    EXPECT_EQ(r[0].at("error").at("code").asInt(), -32700);
}

// 1.1.5
TEST(McpServerTests, notificationsInitializedIsAccepted) {
    auto r = runLines(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    EXPECT_TRUE(r.empty());
}

// ---- U2: skill tools (searchSkills / getSkills / listSkills) ------------

namespace {

using cajeta::mcp::SkillBackend;
namespace sk = cajeta::buildtool::skill;

// A backend returning fixtures so the tests exercise the MCP marshalling layer
// (the cores have their own suites).
SkillBackend fixtureBackend() {
    SkillBackend b;
    b.search = [](const std::string&, std::optional<std::string>,
                  std::optional<std::string>, sk::MatchOptions) {
        sk::SkillSearchResult r{};
        r.uri = "cja-skill://cajeta.io@1.0.0/file-overview";
        r.matchedName = "cajeta/io/File";
        r.tier = sk::MatchTier::Exact;
        r.distance = 0;
        return std::vector<sk::SkillSearchResult>{r};
    };
    b.list = [](std::optional<std::string>, std::optional<std::string>,
                std::optional<std::string>) {
        sk::SkillListEntry e{};
        e.uri = "cja-skill://cajeta.io@1.0.0/file-overview";
        e.names = {"cajeta/io/File"};
        e.title = "File I/O overview";
        return std::vector<sk::SkillListEntry>{e};
    };
    b.get = [](const std::vector<std::string>& uris) {
        std::vector<sk::SkillGetResult> out;
        for (const auto& u : uris) {
            sk::SkillGetResult r{};
            r.uri = u;
            if (u == "cja-skill://cajeta.io@1.0.0/file-overview") {
                r.payload = "# File\nbody";
            } else {
                r.error = "unknown uri";
            }
            out.push_back(r);
        }
        return out;
    };
    return b;
}

// Drive a single tools/call through `handle()` with a configured server.
Json toolCall(McpServer& server, const std::string& name, Json args) {
    Json params = Json::object();
    params["name"] = name;
    params["arguments"] = std::move(args);
    Json req = Json::object();
    req["jsonrpc"] = "2.0";
    req["id"] = 7;
    req["method"] = "tools/call";
    req["params"] = std::move(params);
    return server.handle(req);
}

} // namespace

// 2.1.1
TEST(McpServerTests, searchSkillsToolReturnsUris) {
    McpServer server;
    server.setSkillBackend(fixtureBackend());
    Json args = Json::object();
    args["name"] = "cajeta/io/File";
    Json resp = toolCall(server, "searchSkills", args);
    const Json& results = resp.at("result").at("results");
    ASSERT_TRUE(results.isArray());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].at("uri").asString(),
              "cja-skill://cajeta.io@1.0.0/file-overview");
    EXPECT_EQ(results[0].at("tier").asString(), "exact");
}

// 2.1.2
TEST(McpServerTests, getSkillsToolReturnsPayloads) {
    McpServer server;
    server.setSkillBackend(fixtureBackend());
    Json uris = Json::array();
    uris.push_back(std::string("cja-skill://cajeta.io@1.0.0/file-overview"));
    uris.push_back(std::string("cja-skill://bogus@9.9.9/nope"));
    Json args = Json::object();
    args["uris"] = uris;
    Json resp = toolCall(server, "getSkills", args);
    const Json& skills = resp.at("result").at("skills");
    ASSERT_EQ(skills.size(), 2u);
    EXPECT_TRUE(skills[0].at("ok").asBool());
    EXPECT_EQ(skills[0].at("payload").asString(), "# File\nbody");
    EXPECT_FALSE(skills[1].at("ok").asBool());
    EXPECT_EQ(skills[1].at("error").asString(), "unknown uri");
}

// 2.1.3
TEST(McpServerTests, listSkillsToolReturnsCatalog) {
    McpServer server;
    server.setSkillBackend(fixtureBackend());
    Json resp = toolCall(server, "listSkills", Json::object());
    const Json& skills = resp.at("result").at("skills");
    ASSERT_EQ(skills.size(), 1u);
    EXPECT_EQ(skills[0].at("title").asString(), "File I/O overview");
    EXPECT_EQ(skills[0].at("names")[size_t(0)].asString(), "cajeta/io/File");
}

// 2.1.4
TEST(McpServerTests, skillToolMissingRequiredParamIsInvalidParams) {
    McpServer server;
    server.setSkillBackend(fixtureBackend());
    Json resp = toolCall(server, "searchSkills", Json::object());  // no name
    ASSERT_TRUE(resp.has("error"));
    EXPECT_EQ(resp.at("error").at("code").asInt(), -32602);
}

// ---- U3: compile tool --------------------------------------------------

namespace {

namespace fs = std::filesystem;

// The built `cajeta` binary sits next to the test binary's parent: the test is
// at <build>/test/cajeta_test, the compiler at <build>/src/cajeta.
std::string cajetaExe() {
    fs::path self = fs::canonical("/proc/self/exe");
    return (self.parent_path().parent_path() / "src" / "cajeta").string();
}

// base64(tar.zstd) of the given entries — the `compile`/`jit_execute` archive
// wire form.
std::string buildArchive(const std::vector<cajeta::buildtool::TarEntry>& entries) {
    auto z = cajeta::buildtool::writeTarZstd(entries);
    EXPECT_TRUE((bool) z);
    if (!z) { consumeError(z.takeError()); return ""; }
    return llvm::encodeBase64(*z);
}

McpServer compileServer() {
    McpServer s;
    s.setCompilerExePath(cajetaExe());
    return s;
}

} // namespace

// 3.1.1
TEST(McpServerTests, compileValidArchiveSucceeds) {
    std::vector<cajeta::buildtool::TarEntry> entries = {
        {"test/Foo.cajeta",
         "package test;\npublic class Foo {\n"
         "    public static int32 answer() { return 42; }\n}\n"}};
    Json args = Json::object();
    args["archive"] = buildArchive(entries);
    args["emit"] = "cja";
    McpServer s = compileServer();
    Json resp = toolCall(s, "compile", args);
    const Json& res = resp.at("result");
    EXPECT_EQ(res.at("exitStatus").asInt(), 0);
    EXPECT_EQ(res.at("diagnostics").size(), 0u);
    EXPECT_TRUE(res.has("artifact"));
    EXPECT_FALSE(res.at("artifact").asString().empty());
}

// 3.1.2
TEST(McpServerTests, compileBadSourceReportsDiagnostics) {
    std::vector<cajeta::buildtool::TarEntry> entries = {
        {"test/Bad.cajeta",
         "package test;\npublic class Bad {\n"
         "    public static int32 answer() { return notAThing + 1; }\n}\n"}};
    Json args = Json::object();
    args["archive"] = buildArchive(entries);
    args["emit"] = "cja";
    McpServer s = compileServer();
    Json resp = toolCall(s, "compile", args);
    const Json& res = resp.at("result");
    EXPECT_NE(res.at("exitStatus").asInt(), 0);
    EXPECT_GT(res.at("diagnostics").size(), 0u);
}

// 3.1.3
TEST(McpServerTests, malformedArchiveIsInvalidParams) {
    // Valid base64, but not a tar.zstd archive → invalid params.
    Json args = Json::object();
    args["archive"] = llvm::encodeBase64(std::string("not an archive"));
    McpServer s = compileServer();
    Json resp = toolCall(s, "compile", args);
    ASSERT_TRUE(resp.has("error"));
    EXPECT_EQ(resp.at("error").at("code").asInt(), -32602);
}
