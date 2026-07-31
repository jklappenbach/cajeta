// compiler-mcp Unit 2 — the in-compiler MCP stdio server's handler core
// (spec §2, §8.1). Handler-level: JSON-RPC request strings in, response
// objects asserted — no live process, no stdio.

#include "cajeta/buildtool/mcp/CompilerMcpServer.h"
#include "cajeta/dap/Json.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

using cajeta::buildtool::mcp::CompilerMcpServer;
using cajeta::dap::Json;

namespace {
    namespace fs = std::filesystem;

    // A server rooted in an empty temp dir: no project, no cajeta.lock —
    // context must still seed from the embedded corpus (spec 2.1.4).
    class CompilerMcpServerTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dir_ = fs::temp_directory_path() /
                   ("compiler_mcp_" + std::string(
                        ::testing::UnitTest::GetInstance()->current_test_info()->name()));
            fs::remove_all(dir_);
            fs::create_directories(dir_);
            auto s = CompilerMcpServer::create("9.9.9-test", dir_.string());
            ASSERT_TRUE((bool) s) << "server must construct with no project";
            server_.emplace(std::move(*s));
        }
        void TearDown() override { fs::remove_all(dir_); }

        Json call(const std::string& msg) {
            auto resp = server_->handleMessage(msg);
            EXPECT_TRUE(resp.has_value()) << "expected a response for: " << msg;
            bool ok = false;
            Json j = Json::parse(resp.value_or("null"), &ok);
            EXPECT_TRUE(ok) << "response must be valid JSON: " << resp.value_or("");
            return j;
        }

        fs::path dir_;
        std::optional<CompilerMcpServer> server_;
    };
}

// 2.1.1 — initialize: tools capability, identity, version, instructions.
TEST_F(CompilerMcpServerTest, initializeDeclaresIdentityAndInstructions) {
    Json r = call(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    EXPECT_EQ(r.at("jsonrpc").asString(), "2.0");
    EXPECT_EQ(r.at("id").asInt(), 1);
    const Json& res = r.at("result");
    ASSERT_TRUE(res.isObject());
    EXPECT_TRUE(res.at("capabilities").at("tools").isObject());
    EXPECT_EQ(res.at("serverInfo").at("name").asString(), "compiler-mcp");
    EXPECT_EQ(res.at("serverInfo").at("version").asString(), "9.9.9-test");
    EXPECT_FALSE(res.at("protocolVersion").asString().empty());
    // Instructions direct the agent to search before writing cajeta code.
    EXPECT_NE(res.at("instructions").asString().find("searchSkills"),
              std::string::npos);
}

// 2.1.2 — tools/list: exactly the three skill tools, each fully described.
TEST_F(CompilerMcpServerTest, toolsListEnumeratesThreeSkillTools) {
    Json r = call(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    const Json& tools = r.at("result").at("tools");
    ASSERT_TRUE(tools.isArray());
    ASSERT_EQ(tools.size(), 3u);
    bool saw[3] = {false, false, false};
    for (size_t i = 0; i < tools.size(); ++i) {
        const Json& t = tools[i];
        const std::string& name = t.at("name").asString();
        if (name == "searchSkills") saw[0] = true;
        else if (name == "listSkills") saw[1] = true;
        else if (name == "getSkills") saw[2] = true;
        EXPECT_FALSE(t.at("description").asString().empty()) << name;
        EXPECT_EQ(t.at("inputSchema").at("type").asString(), "object") << name;
    }
    EXPECT_TRUE(saw[0] && saw[1] && saw[2]);
}

// 2.1.3 — protocol errors are well-formed and non-fatal.
TEST_F(CompilerMcpServerTest, errorsAreWellFormedAndServerStaysUp) {
    // Unknown method → -32601.
    Json r1 = call(R"({"jsonrpc":"2.0","id":3,"method":"no/such/method"})");
    EXPECT_EQ(r1.at("error").at("code").asInt(), -32601);
    EXPECT_FALSE(r1.at("error").at("message").asString().empty());

    // Unknown tool → invalid params.
    Json r2 = call(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"noSuchTool","arguments":{}}})");
    EXPECT_EQ(r2.at("error").at("code").asInt(), -32602);

    // Malformed JSON → parse error (id unknowable → null id).
    Json r3 = call("{this is not json");
    EXPECT_EQ(r3.at("error").at("code").asInt(), -32700);

    // Non-object request → invalid request.
    Json r4 = call("42");
    EXPECT_EQ(r4.at("error").at("code").asInt(), -32600);

    // The server still answers a valid request afterward.
    Json r5 = call(R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    EXPECT_TRUE(r5.at("result").at("tools").isArray());
}

// 2.1.3 / JSON-RPC — notifications (no id) get no response.
TEST_F(CompilerMcpServerTest, notificationsProduceNoResponse) {
    auto resp = server_->handleMessage(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    EXPECT_FALSE(resp.has_value());
}

// 2.1.4 — with no project, the context still serves the embedded corpus:
// initialize succeeds (asserted in SetUp) and a stats probe shows archives.
TEST_F(CompilerMcpServerTest, contextSeededFromEmbeddedCorpusWithNoProject) {
    EXPECT_GE(server_->archiveCount(), 16u)   // 15 stdlib + cajeta.toolchain
        << "embedded corpora must seed the context without a lockfile";
}
