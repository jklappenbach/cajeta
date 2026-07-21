//
// fast-debug-launch Unit 2 — launch progress over DAP output events (plan
// 2.1.1/2.1.2, spec 3.2.1). The compile that runs inside configurationDone
// must narrate itself: "compile started", per-source progress, "compile
// finished" — all as `output` events the plugin already renders — so a
// working launch never reads as a hang.
//
#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "../TempProgram.h"

using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::debugtest::TempProgram;

namespace {

const char* kProg =
    "package demo;\n"
    "public class Prog {\n"
    "    public static int32 main() {\n"
    "        int32 a = 6;\n"
    "        return a * 7;\n"
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

// Index of the first event with this name, or -1.
int firstEventIndex(const std::vector<Json>& log, const std::string& ev) {
    for (size_t i = 0; i < log.size(); ++i)
        if (log[i].at("type").asString() == "event" &&
            log[i].at("event").asString() == ev) return (int) i;
    return -1;
}

// All `output` event texts, in order.
std::vector<std::string> outputTexts(const std::vector<Json>& log) {
    std::vector<std::string> texts;
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "output")
            texts.push_back(m.at("body").at("output").asString());
    return texts;
}

// Run initialize -> launch -> configurationDone (no breakpoints; the program
// runs straight to exit) and return the full message log.
std::vector<Json> runSession(const TempProgram& p) {
    DapServer srv;
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Prog.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    return log;
}

} // namespace

// 2.1.1 — a launch narrates the compile: started, per-source progress, and
// finished, all before the program's first stopped/exited event.
TEST(DapLaunchProgress, CompileNarratedBeforeProgramEvents) {
    TempProgram p("demo", "Prog.cajeta", kProg);
    std::vector<Json> log = runSession(p);

    std::vector<std::string> outputs = outputTexts(log);
    ASSERT_FALSE(outputs.empty()) << "no output events emitted during launch";

    int startedIdx = -1, sourceIdx = -1, finishedIdx = -1;
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].find("compile started") != std::string::npos &&
            startedIdx < 0) startedIdx = (int) i;
        if (outputs[i].find("1/1") != std::string::npos &&
            outputs[i].find("Prog.cajeta") != std::string::npos &&
            sourceIdx < 0) sourceIdx = (int) i;
        if (outputs[i].find("compile finished") != std::string::npos &&
            finishedIdx < 0) finishedIdx = (int) i;
    }
    EXPECT_GE(startedIdx, 0) << "no 'compile started' output";
    EXPECT_GE(sourceIdx, 0) << "no per-source '1/1 ... Prog.cajeta' output";
    EXPECT_GE(finishedIdx, 0) << "no 'compile finished' output";
    EXPECT_LT(startedIdx, sourceIdx);
    EXPECT_LT(sourceIdx, finishedIdx);

    // Every progress event lands before the program's own lifecycle events.
    int exitedIdx = firstEventIndex(log, "exited");
    ASSERT_GE(exitedIdx, 0);  // no breakpoints: the program ran to exit
    int lastOutputIdx = -1;
    for (size_t i = 0; i < log.size(); ++i)
        if (log[i].at("type").asString() == "event" &&
            log[i].at("event").asString() == "output")
            lastOutputIdx = (int) i;
    EXPECT_LT(lastOutputIdx, exitedIdx);
}

// 2.1.2 — output events are well-formed: after the initialized event, with a
// category, and newline-terminated text.
TEST(DapLaunchProgress, OutputEventsWellFormed) {
    TempProgram p("demo", "Prog.cajeta", kProg);
    std::vector<Json> log = runSession(p);

    int initializedIdx = firstEventIndex(log, "initialized");
    ASSERT_GE(initializedIdx, 0);

    int outputCount = 0;
    for (size_t i = 0; i < log.size(); ++i) {
        const Json& m = log[i];
        if (m.at("type").asString() != "event" ||
            m.at("event").asString() != "output") continue;
        outputCount++;
        EXPECT_GT((int) i, initializedIdx)
            << "output event before initialized";
        const std::string category = m.at("body").at("category").asString();
        EXPECT_EQ(category, "console");
        const std::string text = m.at("body").at("output").asString();
        ASSERT_FALSE(text.empty());
        EXPECT_EQ(text.back(), '\n') << "output not newline-terminated: "
                                     << text;
    }
    EXPECT_GT(outputCount, 0);
}

// ---- fast-debug-launch Unit 5: launch carries cacheDir (plan 5.1.1) -------
// With cacheDir in the launch request, the second identical session is served
// from the whole-program slot: the console says so, and no per-source compile
// lines appear. (Without cacheDir every launch compiles — pinned by every
// other test in this binary.)

namespace {

std::vector<Json> runSessionWithCache(const TempProgram& p,
                                      const std::string& cacheDir) {
    DapServer srv;
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json launchArgs = Json::object();
    launchArgs["entry-method"] = "demo.Prog.main";
    launchArgs["sourceRoot"] = p.sourceRoot();
    launchArgs["cacheDir"] = cacheDir;
    drive(srv, req(2, "launch", launchArgs), log);
    drive(srv, req(3, "configurationDone", Json::object()), log);
    return log;
}

} // namespace

TEST(DapLaunchProgress, CacheDirMakesSecondSessionCached) {
    TempProgram p("demo", "Prog.cajeta", kProg);
    static std::mt19937_64 rng(std::random_device{}());
    std::filesystem::path cacheDir =
        std::filesystem::temp_directory_path()
        / ("cajeta_dapcache_test_" + std::to_string(rng()));
    std::filesystem::create_directories(cacheDir);

    // Session 1: cold — narrates the compile, populates the slot.
    std::vector<Json> coldLog = runSessionWithCache(p, cacheDir.string());
    bool coldCompiled = false;
    for (const auto& t : outputTexts(coldLog))
        if (t.find("compile started") != std::string::npos) coldCompiled = true;
    ASSERT_TRUE(coldCompiled);
    ASSERT_GE(firstEventIndex(coldLog, "exited"), 0);

    // Session 2: warm — says "cached", never narrates per-source compiling,
    // and the program still runs to the same exit.
    std::vector<Json> warmLog = runSessionWithCache(p, cacheDir.string());
    bool saidCached = false, saidCompiling = false;
    for (const auto& t : outputTexts(warmLog)) {
        if (t.find("cached") != std::string::npos) saidCached = true;
        if (t.find("compiling [") != std::string::npos) saidCompiling = true;
    }
    EXPECT_TRUE(saidCached);
    EXPECT_FALSE(saidCompiling);
    int exited = firstEventIndex(warmLog, "exited");
    ASSERT_GE(exited, 0);
    EXPECT_EQ(warmLog[exited].at("body").at("exitCode").asInt(), 42);

    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
}
