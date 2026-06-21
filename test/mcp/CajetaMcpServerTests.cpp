// Integration tests for the CAJETA-written MCP server (tools/mcp).
//
// The server is a cajeta program; we build it once with `cajeta build` and drive
// the resulting exe over stdio in batch mode (feed JSON-RPC request lines on
// stdin, read response lines from stdout), asserting with the host JSON model.
// POSIX-only (paths + Subprocess).

#include "gtest/gtest.h"
#include "cajeta/buildtool/Subprocess.h"
#include "cajeta/dap/Json.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

using cajeta::dap::Json;
using cajeta::buildtool::runSubprocess;
using cajeta::buildtool::SubprocessOptions;

namespace {

#ifndef _WIN32

namespace fs = std::filesystem;

// build/test/cajeta_test → build/src/cajeta and <repo>/tools/mcp.
fs::path buildDir() { return fs::canonical("/proc/self/exe").parent_path().parent_path(); }
std::string cajetaExe() { return (buildDir() / "src" / "cajeta").string(); }
fs::path mcpProject() { return buildDir().parent_path() / "tools" / "mcp"; }
std::string mcpExe() { return (mcpProject() / "build" / "cajeta-mcp").string(); }

// Build the cajeta server once for the whole suite.
bool ensureServerBuilt() {
    static int state = -1;   // -1 unknown, 0 fail, 1 ok
    if (state >= 0) return state == 1;
    if (fs::exists(mcpExe())) { state = 1; return true; }
    std::string cwd = mcpProject().string();
    std::string out, err;
    SubprocessOptions opt;
    opt.argv = {cajetaExe(), "build"};
    opt.cwd = &cwd;
    opt.outData = &out;
    opt.errData = &err;
    auto r = runSubprocess(opt);
    state = (r.launched && r.code() == 0 && fs::exists(mcpExe())) ? 1 : 0;
    if (state == 0) {
        ADD_FAILURE() << "cajeta build of tools/mcp failed:\n" << err << out;
    }
    return state == 1;
}

// Feed `requests` to the server on stdin; return parsed response objects.
std::vector<Json> drive(const std::string& requests) {
    std::string out, err;
    SubprocessOptions opt;
    opt.argv = {mcpExe(), "--cajeta=" + cajetaExe()};
    opt.stdinData = &requests;
    opt.outData = &out;
    opt.errData = &err;
    auto r = runSubprocess(opt);
    EXPECT_TRUE(r.launched) << "spawn failed: " << err;
    std::vector<Json> responses;
    std::string line;
    for (char c : out) {
        if (c == '\n') {
            if (!line.empty()) {
                bool ok = false;
                Json j = Json::parse(line, &ok);
                if (ok) responses.push_back(j);
            }
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    return responses;
}

} // namespace

// C1.1.1 / C1.1.2 / C1.1.3 — lifecycle handshake + error envelopes.
TEST(CajetaMcpServerTests, lifecycleAndErrors) {
    if (!ensureServerBuilt()) return;
    auto r = drive(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"bogus\"}\n"
        "not json\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");

    // 5 requests, 4 responses (the notification is silent).
    ASSERT_EQ(r.size(), 4u);

    // initialize
    EXPECT_EQ(r[0].at("id").asInt(), 1);
    EXPECT_EQ(r[0].at("result").at("serverInfo").at("name").asString(), "cajeta-mcp");
    EXPECT_TRUE(r[0].at("result").at("capabilities").has("tools"));

    // tools/list — the five tool names
    const Json& tools = r[1].at("result").at("tools");
    ASSERT_TRUE(tools.isArray());
    std::set<std::string> names;
    for (size_t i = 0; i < tools.size(); i++) names.insert(tools[i].at("name").asString());
    EXPECT_EQ(names, (std::set<std::string>{
        "searchSkills", "listSkills", "getSkills", "compile", "jit_execute"}));

    // unknown method → -32601; bad JSON → -32700
    EXPECT_EQ(r[2].at("error").at("code").asInt(), -32601);
    EXPECT_EQ(r[3].at("error").at("code").asInt(), -32700);
}

// C2.1.3 + mechanism — skill tools validate params and return well-formed,
// shape-correct results. (A real-uri assertion needs an on-disk skill fixture
// with a cajeta.lock; without one in cwd the cores return empty sets, which is
// what we assert here. Real-fixture coverage is a follow-up.)
TEST(CajetaMcpServerTests, skillToolsValidationAndShape) {
    if (!ensureServerBuilt()) return;
    auto r = drive(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"searchSkills\",\"arguments\":{}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"searchSkills\",\"arguments\":{\"name\":\"cajeta/io/File\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"getSkills\",\"arguments\":{}}}\n");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].at("error").at("code").asInt(), -32602);   // missing name
    EXPECT_TRUE(r[1].at("result").at("results").isArray());   // well-formed shape
    EXPECT_EQ(r[2].at("error").at("code").asInt(), -32602);   // missing uris
}

#endif // !_WIN32
