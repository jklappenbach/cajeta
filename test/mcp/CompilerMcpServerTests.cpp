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

// ---- Unit 3 — skill tools wired in-process (spec §3) ----

// 3.1.1 — typo-tolerant search through tools/call, CLI --json item shape.
TEST_F(CompilerMcpServerTest, searchSkillsToolResolvesTypoedName) {
    Json r = call(
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"searchSkills","arguments":{"name":"cajeta/io/file/Fiel"}}})");
    const Json& results = r.at("result").at("results");
    ASSERT_TRUE(results.isArray()) << "result must carry a results array";
    bool sawFile = false;
    for (size_t i = 0; i < results.size(); ++i) {
        const Json& m = results[i];
        EXPECT_TRUE(m.has("uri") && m.has("matchedName") && m.has("tier") &&
                    m.has("distance"));
        if (m.at("uri").asString().find("io-file-File") != std::string::npos)
            sawFile = true;
    }
    EXPECT_TRUE(sawFile) << "typo'd query must resolve io-file-File";
}

// 3.1.2 — scoped list; batch get with partial success.
TEST_F(CompilerMcpServerTest, listAndGetSkillsTools) {
    Json r = call(
        R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"listSkills","arguments":{"scope":"cajeta/toolchain"}}})");
    const Json& skills = r.at("result").at("skills");
    ASSERT_TRUE(skills.isArray());
    EXPECT_GE(skills.size(), 5u);
    EXPECT_TRUE(skills[0].has("uri") && skills[0].has("title") &&
                skills[0].has("names"));

    Json g = call(
        R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"getSkills","arguments":{"uris":["cja-skill://cajeta.toolchain@1.0/cajeta-driver-overview","cja-skill://cajeta.toolchain@1.0/no-such-skill"]}}})");
    const Json& got = g.at("result").at("skills");
    ASSERT_TRUE(got.isArray());
    ASSERT_EQ(got.size(), 2u);
    EXPECT_TRUE(got[0].at("ok").asBool());
    EXPECT_NE(got[0].at("payload").asString().find("id: cajeta-driver-overview"),
              std::string::npos);
    EXPECT_FALSE(got[1].at("ok").asBool(true));
    EXPECT_FALSE(got[1].at("error").asString().empty());
}

// 3.1.3 — no match is an empty result, not an error; bad args are -32602.

#ifndef _WIN32

#include "cajeta/buildtool/Subprocess.h"

namespace {
    std::string mcpCajetaExe() {
        auto build = fs::canonical("/proc/self/exe").parent_path().parent_path();
        return (build / "src" / "cajeta").string();
    }

    // Run `cajeta <args...> --json` in `cwd`; return parsed stdout JSON.
    Json runCliJson(const std::vector<std::string>& args, const std::string& cwd) {
        std::vector<std::string> argv = {mcpCajetaExe()};
        for (auto& a : args) argv.push_back(a);
        argv.push_back("--json");
        std::string out, err;
        cajeta::buildtool::SubprocessOptions opt;
        opt.argv = argv;
        opt.cwd = &cwd;
        opt.outData = &out;
        opt.errData = &err;
        auto r = cajeta::buildtool::runSubprocess(opt);
        EXPECT_TRUE(r.launched) << "spawn failed: " << err;
        bool ok = false;
        Json j = Json::parse(out, &ok);
        EXPECT_TRUE(ok) << "CLI --json must parse: " << out;
        return j;
    }
}

// 3.1.4 — field-for-field parity between the MCP tools and the CLI --json.

#endif // !_WIN32
