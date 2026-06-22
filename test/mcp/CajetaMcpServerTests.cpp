// Integration tests for the CAJETA-written MCP server (tools/mcp).
//
// The server is a cajeta program; we build it once with `cajeta build` and drive
// the resulting exe over stdio in batch mode (feed JSON-RPC request lines on
// stdin, read response lines from stdout), asserting with the host JSON model.
// POSIX-only (paths + Subprocess).

#include "gtest/gtest.h"
#include "cajeta/buildtool/Subprocess.h"
#include "cajeta/dap/Json.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>   // environ

using cajeta::dap::Json;
using cajeta::buildtool::runSubprocess;
using cajeta::buildtool::SubprocessOptions;

#ifndef _WIN32

namespace {

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

// Parse newline-delimited JSON-RPC responses out of the server's stdout.
std::vector<Json> parseResponses(const std::string& out) {
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

// Drive the server with extra CLI args + an optional replacement env (which
// MUST include PATH — the server shells out to sh/printenv/date/stat).
std::vector<Json> driveArgs(const std::string& requests,
                            const std::vector<std::string>& extraArgs,
                            const std::vector<std::string>* env = nullptr) {
    std::string out, err;
    SubprocessOptions opt;
    opt.argv = {mcpExe(), "--cajeta=" + cajetaExe()};
    for (auto& a : extraArgs) opt.argv.push_back(a);
    opt.stdinData = &requests;
    opt.outData = &out;
    opt.errData = &err;
    opt.env = env;
    auto r = runSubprocess(opt);
    EXPECT_TRUE(r.launched) << "spawn failed: " << err;
    return parseResponses(out);
}

// Default driver: caching OFF so C1–C4 stay deterministic and isolated.
std::vector<Json> drive(const std::string& requests) {
    return driveArgs(requests, {"--no-cache"});
}

// A fresh, empty cache directory unique to one test invocation.
fs::path freshCacheDir(const std::string& tag) {
    static int counter = 0;
    auto p = fs::temp_directory_path()
           / ("cajeta_mcp_cache_" + tag + "_" + std::to_string(++counter));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// The parent environment as KEY=VALUE pairs (so PATH etc. survive), plus extras.
std::vector<std::string> envWith(const std::vector<std::string>& extra) {
    std::vector<std::string> e;
    for (char** p = environ; p && *p; ++p) e.emplace_back(*p);
    for (auto& x : extra) e.push_back(x);
    return e;
}

} // namespace

// Build a single `tools/call compile` request line for `files`+`entry`.
std::string compileRequest(int id, const std::vector<std::pair<std::string, std::string>>& files,
                           const std::string& entry, bool withEntry = true) {
    Json args = Json::object();
    Json fmap = Json::object();
    for (auto& kv : files) fmap[kv.first] = Json(kv.second);
    args["files"] = fmap;
    if (withEntry) args["entry"] = Json(entry);
    Json params = Json::object();
    params["name"] = Json("compile");
    params["arguments"] = args;
    Json req = Json::object();
    req["jsonrpc"] = Json("2.0");
    req["id"] = Json(id);
    req["method"] = Json("tools/call");
    req["params"] = params;
    return req.dump() + "\n";
}

// Build a single `tools/call jit_execute` request line for `files`+`entry`.
std::string jitRequest(int id, const std::vector<std::pair<std::string, std::string>>& files,
                       const std::string& entry) {
    Json args = Json::object();
    Json fmap = Json::object();
    for (auto& kv : files) fmap[kv.first] = Json(kv.second);
    args["files"] = fmap;
    args["entry"] = Json(entry);
    Json params = Json::object();
    params["name"] = Json("jit_execute");
    params["arguments"] = args;
    Json req = Json::object();
    req["jsonrpc"] = Json("2.0");
    req["id"] = Json(id);
    req["method"] = Json("tools/call");
    req["params"] = params;
    return req.dump() + "\n";
}

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

// mcp-compression U1 — the transport seam (StdioTransport.readMessage) frames
// requests independently of dispatch: blank lines between framed requests are
// skipped, and each request still gets exactly one framed response in order.
TEST(CajetaMcpServerTests, transportSeamSkipsBlankLinesAndFrames) {
    if (!ensureServerBuilt()) return;
    auto r = drive(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n"
        "\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].at("id").asInt(), 1);
    EXPECT_EQ(r[1].at("id").asInt(), 2);
    EXPECT_TRUE(r[1].at("result").has("tools"));
}

// C2.1.3 — skill tools validate their params (missing name / missing uris).
TEST(CajetaMcpServerTests, skillToolsValidation) {
    if (!ensureServerBuilt()) return;
    auto r = drive(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"searchSkills\",\"arguments\":{}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"getSkills\",\"arguments\":{}}}\n");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].at("error").at("code").asInt(), -32602);   // missing name
    EXPECT_EQ(r[1].at("error").at("code").asInt(), -32602);   // missing uris
}

// F.4.1 — the MCP skill tools shell out to the CLI, which always seeds the
// embedded stdlib corpus (spec §2.5). So with NO project / cajeta.lock,
// searchSkills surfaces a stdlib URI and getSkills returns its payload.
TEST(CajetaMcpServerTests, skillToolsReturnStdlibWithNoProject) {
    if (!ensureServerBuilt()) return;
    auto r = drive(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"searchSkills\",\"arguments\":{\"name\":\"cajeta.process\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"getSkills\",\"arguments\":{\"uris\":["
        "\"cja-skill://cajeta.process@1.0/process-overview\"]}}}\n");
    ASSERT_EQ(r.size(), 2u);

    // searchSkills → non-empty results carrying an embedded stdlib URI.
    const Json& results = r[0].at("result").at("results");
    ASSERT_TRUE(results.isArray());
    ASSERT_GT(results.size(), 0u);
    bool sawProcess = false;
    for (size_t i = 0; i < results.size(); i++) {
        const std::string uri = results[i].at("uri").asString();
        if (uri.find("cja-skill://cajeta.process@") != std::string::npos) sawProcess = true;
    }
    EXPECT_TRUE(sawProcess);

    // getSkills → the decompressed payload for the embedded URI.
    const Json& skills = r[1].at("result").at("skills");
    ASSERT_TRUE(skills.isArray());
    ASSERT_EQ(skills.size(), 1u);
    EXPECT_TRUE(skills[0].at("ok").asBool());
    EXPECT_NE(skills[0].at("payload").asString().find("id: process-overview"),
              std::string::npos);
}

// C3.1.1 — compile of a valid package ⇒ exitStatus 0, empty diagnostics, artifact present.
TEST(CajetaMcpServerTests, compileValidProducesArtifact) {
    if (!ensureServerBuilt()) return;
    auto r = drive(compileRequest(1,
        {{"demo/Hello.cajeta",
          "package demo;\n"
          "public final class Hello {\n"
          "    public static int32 run() { return 0; }\n"
          "}\n"}},
        "demo.Hello.run"));
    ASSERT_EQ(r.size(), 1u);
    const Json& res = r[0].at("result");
    EXPECT_EQ(res.at("exitStatus").asInt(), 0);
    ASSERT_TRUE(res.at("diagnostics").isArray());
    EXPECT_EQ(res.at("diagnostics").size(), 0u);
    ASSERT_TRUE(res.has("artifact"));
    EXPECT_FALSE(res.at("artifact").asString().empty());   // base64 .cja
}

// C3.1.2 — bad source ⇒ non-zero exitStatus + diagnostics, no artifact.
TEST(CajetaMcpServerTests, compileBadSourceReturnsDiagnostics) {
    if (!ensureServerBuilt()) return;
    auto r = drive(compileRequest(1,
        {{"demo/Bad.cajeta",
          "package demo;\n"
          "public final class Bad { this is not valid cajeta\n"}},
        "demo.Bad.run"));
    ASSERT_EQ(r.size(), 1u);
    const Json& res = r[0].at("result");
    EXPECT_NE(res.at("exitStatus").asInt(), 0);
    ASSERT_TRUE(res.at("diagnostics").isArray());
    EXPECT_GT(res.at("diagnostics").size(), 0u);
    EXPECT_FALSE(res.has("artifact"));
}

// C3.1.3 — missing/empty files or entry ⇒ -32602.
TEST(CajetaMcpServerTests, compileMissingParamsIsInvalid) {
    if (!ensureServerBuilt()) return;
    auto r = drive(
        // no files at all
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"compile\",\"arguments\":{}}}\n"
        // empty files map
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"compile\",\"arguments\":{\"files\":{},\"entry\":\"a.B.c\"}}}\n"
        // files present but no entry
        + compileRequest(3, {{"demo/Hello.cajeta", "package demo;\n"}}, "", false));
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].at("error").at("code").asInt(), -32602);   // missing files
    EXPECT_EQ(r[1].at("error").at("code").asInt(), -32602);   // empty files
    EXPECT_EQ(r[2].at("error").at("code").asInt(), -32602);   // missing entry
}

// C4.1.1 — jit_execute returns the entry's value + captured stdout; the
// program's stdout is clean (jit-run chatter lands on stderr).
TEST(CajetaMcpServerTests, jitExecuteCapturesReturnAndStdout) {
    if (!ensureServerBuilt()) return;
    auto r = drive(jitRequest(1,
        {{"demo/Hello.cajeta",
          "package demo;\n"
          "import cajeta.lang.System;\n"
          "public final class Hello {\n"
          "    public static int32 run() {\n"
          "        System.stdout.println(\"hello-from-jit\");\n"
          "        return 7;\n"
          "    }\n"
          "}\n"}},
        "demo.Hello.run"));
    ASSERT_EQ(r.size(), 1u);
    const Json& res = r[0].at("result");
    EXPECT_EQ(res.at("returnValue").asInt(), 7);
    EXPECT_EQ(res.at("exitStatus").asInt(), 7);
    EXPECT_FALSE(res.at("cacheHit").asBool());
    const std::string out = res.at("stdout").asString();
    EXPECT_NE(out.find("hello-from-jit"), std::string::npos);   // program output
    EXPECT_EQ(out.find("[jit-run]"), std::string::npos);        // chatter NOT on stdout
}

// C4.1.2 — process-per-execute isolation: a static-counter program run twice
// returns the same value (each call is a fresh process, no shared state).
TEST(CajetaMcpServerTests, jitExecuteIsolatesProcessState) {
    if (!ensureServerBuilt()) return;
    const std::vector<std::pair<std::string, std::string>> files = {
        {"demo/Counter.cajeta",
         "package demo;\n"
         "public final class Counter {\n"
         "    public static int32 n;\n"
         "    public static int32 run() {\n"
         "        Counter.n = Counter.n + 1;\n"
         "        return Counter.n;\n"
         "    }\n"
         "}\n"}};
    auto r = drive(jitRequest(1, files, "demo.Counter.run")
                 + jitRequest(2, files, "demo.Counter.run"));
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].at("result").at("returnValue").asInt(), 1);
    EXPECT_EQ(r[1].at("result").at("returnValue").asInt(), 1);   // not 2 — fresh process
}

// A small jit_execute fixture (returns 5, prints "hi").
static const std::vector<std::pair<std::string, std::string>> kHelloFiles = {
    {"demo/Hello.cajeta",
     "package demo;\n"
     "import cajeta.lang.System;\n"
     "public final class Hello {\n"
     "    public static int32 run() { System.stdout.println(\"hi\"); return 5; }\n"
     "}\n"}};

// C5.1.1 / C5.3.1 — same submission twice ⇒ second is a cache hit, and the
// on-disk cache survives a server RESTART (two separate processes, one dir).
TEST(CajetaMcpServerTests, cacheHitOnRepeatSubmission) {
    if (!ensureServerBuilt()) return;
    auto dir = freshCacheDir("hit");
    // first process: cold miss, writes the cache entry
    auto r1 = driveArgs(jitRequest(1, kHelloFiles, "demo.Hello.run"),
                        {"--cache-dir=" + dir.string()});
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_FALSE(r1[0].at("result").at("cacheHit").asBool());
    // second process (restart): hit from the on-disk entry
    auto r2 = driveArgs(jitRequest(2, kHelloFiles, "demo.Hello.run"),
                        {"--cache-dir=" + dir.string()});
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_TRUE(r2[0].at("result").at("cacheHit").asBool());
    EXPECT_EQ(r2[0].at("result").at("returnValue").asInt(), 5);
    fs::remove_all(dir);
}

// C5.1.2 — different inputs ⇒ cache miss (distinct keys).
TEST(CajetaMcpServerTests, cacheMissOnDifferentInputs) {
    if (!ensureServerBuilt()) return;
    auto dir = freshCacheDir("miss");
    const std::vector<std::pair<std::string, std::string>> other = {
        {"demo/Hello.cajeta",
         "package demo;\n"
         "public final class Hello {\n"
         "    public static int32 run() { return 9; }\n"
         "}\n"}};
    auto r = driveArgs(jitRequest(1, kHelloFiles, "demo.Hello.run")
                     + jitRequest(2, other, "demo.Hello.run"),
                       {"--cache-dir=" + dir.string()});
    ASSERT_EQ(r.size(), 2u);
    EXPECT_FALSE(r[0].at("result").at("cacheHit").asBool());
    EXPECT_FALSE(r[1].at("result").at("cacheHit").asBool());   // different source
    fs::remove_all(dir);
}

// C5.1.3 — caps + TTL: entries-cap eviction, byte-cap eviction, TTL expiry.
TEST(CajetaMcpServerTests, cacheCapsAndTtl) {
    if (!ensureServerBuilt()) return;
    const std::vector<std::pair<std::string, std::string>> a = kHelloFiles;
    const std::vector<std::pair<std::string, std::string>> b = {
        {"demo/Hello.cajeta",
         "package demo;\n"
         "public final class Hello {\n"
         "    public static int32 run() { return 3; }\n"
         "}\n"}};

    // (a) max-entries=1: A, B (evicts A), A again ⇒ A is a miss.
    {
        auto dir = freshCacheDir("entries");
        auto r = driveArgs(jitRequest(1, a, "demo.Hello.run")
                         + jitRequest(2, b, "demo.Hello.run")
                         + jitRequest(3, a, "demo.Hello.run"),
                           {"--cache-dir=" + dir.string(), "--cache-max-entries=1"});
        ASSERT_EQ(r.size(), 3u);
        EXPECT_TRUE(r[2].at("result").at("cacheHit").asBool() == false);   // A evicted
        fs::remove_all(dir);
    }

    // (b) max-bytes=1: every entry exceeds the cap and is evicted immediately,
    //     so a repeat submission still misses.
    {
        auto dir = freshCacheDir("bytes");
        auto r = driveArgs(jitRequest(1, a, "demo.Hello.run")
                         + jitRequest(2, a, "demo.Hello.run"),
                           {"--cache-dir=" + dir.string(), "--cache-max-bytes=1"});
        ASSERT_EQ(r.size(), 2u);
        EXPECT_FALSE(r[1].at("result").at("cacheHit").asBool());
        fs::remove_all(dir);
    }

    // (c) TTL: cache A, backdate the entry past the TTL, re-submit ⇒ miss.
    {
        auto dir = freshCacheDir("ttl");
        auto r1 = driveArgs(jitRequest(1, a, "demo.Hello.run"),
                            {"--cache-dir=" + dir.string()});
        ASSERT_EQ(r1.size(), 1u);
        EXPECT_FALSE(r1[0].at("result").at("cacheHit").asBool());
        // backdate every cache file by an hour
        for (auto& e : fs::directory_iterator(dir)) {
            auto t = fs::file_time_type::clock::now() - std::chrono::hours(1);
            fs::last_write_time(e.path(), t);
        }
        auto r2 = driveArgs(jitRequest(2, a, "demo.Hello.run"),
                            {"--cache-dir=" + dir.string(), "--cache-ttl=5"});
        ASSERT_EQ(r2.size(), 1u);
        EXPECT_FALSE(r2[0].at("result").at("cacheHit").asBool());   // expired
        fs::remove_all(dir);
    }
}

// C5.1.4 — config precedence: CLI > env > --config file > default.
TEST(CajetaMcpServerTests, configPrecedence) {
    if (!ensureServerBuilt()) return;
    const std::string init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n";

    auto ttlOf = [&](const std::vector<std::string>& args,
                     const std::vector<std::string>* env) {
        auto r = driveArgs(init, args, env);
        EXPECT_EQ(r.size(), 1u);
        return r[0].at("result").at("cacheConfig").at("ttl").asInt();
    };

    // default
    EXPECT_EQ(ttlOf({"--no-cache"}, nullptr), 3600);

    // --config file with ttl=111
    auto cfgDir = freshCacheDir("cfg");
    auto cfgPath = (cfgDir / "config.json").string();
    {
        std::ofstream o(cfgPath);
        o << "{\"cache\":{\"ttl\":111}}";
    }
    EXPECT_EQ(ttlOf({"--no-cache", "--config=" + cfgPath}, nullptr), 111);

    // env overrides file
    auto env222 = envWith({"CAJETA_MCP_CACHE_TTL=222"});
    EXPECT_EQ(ttlOf({"--no-cache", "--config=" + cfgPath}, &env222), 222);

    // CLI overrides env + file
    EXPECT_EQ(ttlOf({"--no-cache", "--config=" + cfgPath, "--cache-ttl=333"}, &env222), 333);

    fs::remove_all(cfgDir);
}

#endif // !_WIN32
