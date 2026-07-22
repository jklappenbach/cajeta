//
// resident-debug-server Unit 4 — resident world v1 (plan 4.1.x, spec §3/§4).
// With `resident: true` on the launch request, the server keeps the primed
// stdlib front-end (StdlibReuseCore) across sessions: a no-edit relaunch is
// a manifest hit, an edit relaunch reuses the resident world (narrated) and
// resweeps user sources only, statics start fresh every session, and a
// vanished cache degrades to a full rebuild — never a failure.
//
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "../TempProgram.h"

namespace fs = std::filesystem;
using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::debugtest::TempProgram;

namespace {

const char* kProgV1 =
    "package demo;\n"
    "public class Prog {\n"
    "    public static int32 main() {\n"
    "        return 6 * 7;\n"
    "    }\n"
    "}\n";

const char* kProgV2 =
    "package demo;\n"
    "public class Prog {\n"
    "    public static int32 main() {\n"
    "        return 5 * 7;\n"
    "    }\n"
    "}\n";

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

int exitCodeOf(const std::vector<Json>& log) {
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            return m.at("body").at("exitCode").asInt();
    return -1;
}

bool sawOutput(const std::vector<Json>& log, const std::string& needle) {
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "output" &&
            m.at("body").at("output").asString().find(needle)
                != std::string::npos)
            return true;
    return false;
}

// One session: initialize -> launch(resident, cacheDir) -> configurationDone
// -> (exit) -> disconnect. Returns the full log.
std::vector<Json> session(DapServer& srv, const TempProgram& p,
                          const std::string& cacheDir, const char* entry) {
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = entry;
    args["sourceRoot"] = p.sourceRoot();
    args["cacheDir"] = cacheDir;
    args["resident"] = true;
    drive(srv, req(2, "launch", args), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    drive(srv, req(4, "disconnect", Json::object()), log);
    return log;
}

struct CacheDirFixture {
    fs::path dir;
    CacheDirFixture() {
        static std::mt19937_64 rng(std::random_device{}());
        dir = fs::temp_directory_path()
            / ("cajeta_resident_test_" + std::to_string(rng()));
        fs::create_directories(dir);
    }
    ~CacheDirFixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

// 4.1.1 — resident no-edit relaunch is a manifest hit (narrated as cached).
TEST(ResidentWorld, NoEditRelaunchIsCacheHit) {
    TempProgram p("demo", "Prog.cajeta", kProgV1);
    CacheDirFixture cache;
    DapServer srv;

    std::vector<Json> s1 = session(srv, p, cache.dir.string(), "demo.Prog.main");
    ASSERT_EQ(exitCodeOf(s1), 42);

    std::vector<Json> s2 = session(srv, p, cache.dir.string(), "demo.Prog.main");
    EXPECT_EQ(exitCodeOf(s2), 42);
    EXPECT_TRUE(sawOutput(s2, "using cached build"));
}

// 4.1.2 — an EDIT relaunch reuses the resident world (stdlib retained,
// narrated) and produces the edited behavior.
TEST(ResidentWorld, EditRelaunchReusesResidentWorld) {
    TempProgram p("demo", "Prog.cajeta", kProgV1);
    CacheDirFixture cache;
    DapServer srv;

    std::vector<Json> s1 = session(srv, p, cache.dir.string(), "demo.Prog.main");
    ASSERT_EQ(exitCodeOf(s1), 42);

    {
        std::ofstream out(p.root / "demo" / "Prog.cajeta", std::ios::trunc);
        out << kProgV2;
    }

    std::vector<Json> s2 = session(srv, p, cache.dir.string(), "demo.Prog.main");
    EXPECT_EQ(exitCodeOf(s2), 35);                       // edited behavior
    EXPECT_FALSE(sawOutput(s2, "using cached build"));   // real rebuild
    EXPECT_TRUE(sawOutput(s2, "resident world reused"))
        << "the rebuild did not go through the resident core";
}

// 4.1.3 — spec 4.2.1: statics reset between sessions on one resident server.
TEST(ResidentWorld, StaticsAreFreshEverySession) {
    static const char* kStaticProg =
        "package demo;\n"
        "public class Counter {\n"
        "    static int32 count = 0;\n"
        "    public static int32 main() {\n"
        "        count = count + 41;\n"
        "        return count + 1;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Counter.cajeta", kStaticProg);
    CacheDirFixture cache;
    DapServer srv;

    std::vector<Json> s1 = session(srv, p, cache.dir.string(),
                                   "demo.Counter.main");
    ASSERT_EQ(exitCodeOf(s1), 42);

    // A leaked static would make this 83.
    std::vector<Json> s2 = session(srv, p, cache.dir.string(),
                                   "demo.Counter.main");
    EXPECT_EQ(exitCodeOf(s2), 42);
}

// 4.1.4 — reuse doubt degrades, never fails: the cache tree vanishing
// between sessions costs a full rebuild and nothing else.
TEST(ResidentWorld, VanishedCacheDegradesToFullRebuild) {
    TempProgram p("demo", "Prog.cajeta", kProgV1);
    CacheDirFixture cache;
    DapServer srv;

    std::vector<Json> s1 = session(srv, p, cache.dir.string(), "demo.Prog.main");
    ASSERT_EQ(exitCodeOf(s1), 42);

    std::error_code ec;
    fs::remove_all(cache.dir, ec);   // the whole tree, pools and all

    std::vector<Json> s2 = session(srv, p, cache.dir.string(), "demo.Prog.main");
    EXPECT_EQ(exitCodeOf(s2), 42);
    EXPECT_FALSE(sawOutput(s2, "using cached build"));
}
