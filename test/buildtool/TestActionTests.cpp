// Regression tests for the Phase 7a `test` action. The action
// wraps a pre-built test binary, runs it as a child process, and
// derives pass/fail counts from the exit code (until the
// structured-findings stream lands with the plugin runtime).
//
// Fixture strategy: use /bin/sh as the "test binary" with a tiny
// shell script for each scenario. Real cajeta test binaries land
// once the language-side test framework ships; this exercises the
// action's contract independently of that.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::ActionRegistry;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::TaskContext;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    cajeta::buildtool::ResolvedProperties makeProps() {
        auto m = loadManifestString(
            R"({"details":{"name":"a.b","version":"0.1"}})");
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        auto r = resolveProperties(*m);
        if (!r) {
            ADD_FAILURE() << errorText(r.takeError());
            return {};
        }
        return std::move(*r);
    }

    std::filesystem::path tempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-testaction-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Write an executable shell script with the given body. The
    // tests use this as a stand-in for a real test binary.
    std::filesystem::path writeScript(const std::filesystem::path& dir,
                                      const std::string& body) {
        auto p = dir / "test-bin";
        std::ofstream o(p, std::ios::binary);
        o << "#!/bin/sh\n" << body << "\n";
        o.close();
        std::filesystem::permissions(
            p,
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace);
        return p;
    }

    std::string readFileStr(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }

} // namespace

// ─── happy path ───────────────────────────────────────────────────

TEST(TestActionTests, passingBinaryReportsPassed) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("pass");
    auto bin = writeScript(d, "exit 0");

    llvm::json::Object params;
    params["input"] = bin.string();
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs["passed"], "1");
    EXPECT_EQ(r->outputs["failed"], "0");
    EXPECT_EQ(r->outputs["crashed"], "0");
    EXPECT_EQ(r->outputs["exit-code"], "0");
    std::filesystem::remove_all(d);
}

TEST(TestActionTests, failingBinaryReportsFailed) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("fail");
    auto bin = writeScript(d, "exit 1");

    llvm::json::Object params;
    params["input"] = bin.string();
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_FALSE((bool)r);
    EXPECT_NE(errorText(r.takeError()).find("failed (exit 1)"),
              std::string::npos);
    std::filesystem::remove_all(d);
}

TEST(TestActionTests, crashedBinaryReportsCrashed) {
#if defined(_WIN32)
    // Crash detection is signal-based (WIFSIGNALED). Windows has no signal
    // model — a process that dies abnormally just yields a non-zero exit code,
    // indistinguishable from a normal failure — so there is nothing to assert.
    GTEST_SKIP() << "signal-based crash detection is POSIX-only";
#else
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("crash");
    // SIGKILL self: triggers WIFSIGNALED on the parent's wait.
    auto bin = writeScript(d, "kill -9 $$");

    llvm::json::Object params;
    params["input"] = bin.string();
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_FALSE((bool)r);
    EXPECT_NE(errorText(r.takeError()).find("crashed"),
              std::string::npos);
    std::filesystem::remove_all(d);
#endif
}

// ─── args / filter / parallel forwarded as flags ──────────────────

TEST(TestActionTests, filterFlagPassedToBinary) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("filter");
    // The script asserts argv[1] equals "--filter=onlyThis".
    auto bin = writeScript(
        d,
        "if [ \"$1\" = \"--filter=onlyThis\" ]; then exit 0; "
        "else echo \"got: $1\" >&2; exit 1; fi");

    llvm::json::Object params;
    params["input"]  = bin.string();
    params["filter"] = "onlyThis";
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    std::filesystem::remove_all(d);
}

TEST(TestActionTests, parallelIntForwardedAsFlag) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("par");
    auto bin = writeScript(
        d,
        "if [ \"$1\" = \"--parallel=4\" ]; then exit 0; "
        "else echo \"got: $1\" >&2; exit 1; fi");

    llvm::json::Object params;
    params["input"]    = bin.string();
    params["parallel"] = 4;
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    std::filesystem::remove_all(d);
}

TEST(TestActionTests, extraArgsArrayForwarded) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("extra-args");
    auto bin = writeScript(
        d,
        "if [ \"$1\" = \"first\" ] && [ \"$2\" = \"second\" ]; "
        "then exit 0; else echo \"$@\" >&2; exit 1; fi");

    llvm::json::Object params;
    params["input"] = bin.string();
    llvm::json::Array args;
    args.push_back("first");
    args.push_back("second");
    params["args"] = std::move(args);
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    std::filesystem::remove_all(d);
}

// ─── report file ──────────────────────────────────────────────────

TEST(TestActionTests, reportFileWrittenWithSummary) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("report");
    auto bin = writeScript(d, "exit 0");
    auto report = d / "report.txt";

    llvm::json::Object params;
    params["input"]  = bin.string();
    params["report"] = report.string();
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs["report-path"], report.string());
    EXPECT_TRUE(std::filesystem::exists(report));
    auto contents = readFileStr(report);
    EXPECT_NE(contents.find("passed: 1"), std::string::npos);
    EXPECT_NE(contents.find("failed: 0"), std::string::npos);
    std::filesystem::remove_all(d);
}

// ─── validation ───────────────────────────────────────────────────

TEST(TestActionTests, missingInputErrorsClearly) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    llvm::json::Object params;  // no input
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_FALSE((bool)r);
    EXPECT_NE(errorText(r.takeError()).find("missing required 'input'"),
              std::string::npos);
}

TEST(TestActionTests, registryAdvertisesTestAction) {
    ActionRegistry registry;
    EXPECT_TRUE(registry.get("test") != nullptr);
    auto names = registry.list();
    EXPECT_NE(std::find(names.begin(), names.end(), "test"),
              names.end());
}

// ─── output threading: ${id.passed} etc. exposed ──────────────────

// ─── coverage param accepted but ignored (Phase 7d wires it) ──────

TEST(TestActionTests, coverageParamAcceptedAsForwardCompatPlaceholder) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("coverage");
    auto bin = writeScript(d, "exit 0");

    llvm::json::Object params;
    params["input"]    = bin.string();
    params["coverage"] = true;
    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    std::filesystem::remove_all(d);
}
