// Tests for the `cajeta mcp` server transport skeleton (cajeta-mcp U1):
// JSON-RPC 2.0 over newline-delimited stdio, lifecycle (initialize /
// notifications/initialized / tools/list), and error envelopes.

#include "gtest/gtest.h"
#include "cajeta/mcp/McpServer.h"

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
