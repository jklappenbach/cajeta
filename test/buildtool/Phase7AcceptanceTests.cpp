// Phase 7 — acceptance tests. Each criterion in plan/build-tool-
// plan.md "Phase 7 / Acceptance" gets a pinning test here.
//
//   #1 — `cajeta test` builds with coverage instrumentation, runs
//        tests, emits HTML + console + SARIF reports.
//        → testActionCoverageEmitsHtmlConsoleAndSarif
//   #2 — A `min: 80` threshold violation fails the task with a
//        bottom-N file citation.
//        → testActionCoverageMin80ViolationCitesBottomN
//   #3 — The security plugin flags a known secret pattern in a
//        test fixture.
//        → securityPluginFlagsSecretPattern (mock cajeta.lint
//        .security plugin via shell-script binary)
//   #4 — A plugin requesting a capability not in the allowlist
//        fails to load with a clear error.
//        → citation in plan only — pinned by PluginTests.resolve
//        PluginsRejectsCapabilityOutsideAllowlist (avoid
//        duplicating that test here).
//
// Acceptance #1 + #2 are exercised via the TestAction's coverage
// block consuming a pre-computed coverage-map fixture (the same
// shape cajeta.coverage.collect writes). End-to-end with the
// actual coverage plugin awaits compiler integration that can
// build Cajeta plugin sources into runnable binaries.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Plugin.h"
#include "cajeta/buildtool/PluginRuntime.h"
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
using cajeta::buildtool::invokePluginAction;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::ResolvedPlugin;
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

    cajeta::buildtool::Manifest makeManifest() {
        auto m = loadManifestString(R"({
            "details": { "name": "p.q", "version": "1.0.0" }
        })");
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

    cajeta::buildtool::ResolvedProperties makeProps(
        const cajeta::buildtool::Manifest& m) {
        auto r = resolveProperties(m);
        if (!r) {
            ADD_FAILURE() << errorText(r.takeError());
            return {};
        }
        return std::move(*r);
    }

    std::filesystem::path tempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-p7-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    std::filesystem::path writeExec(
        const std::filesystem::path& dir,
        const std::string& name,
        const std::string& body) {
        auto p = dir / name;
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

    ResolvedPlugin makePlugin(const std::string& id,
                              const std::string& action,
                              const std::filesystem::path& binary) {
        ResolvedPlugin r;
        r.name = id;
        r.version = "1.0.0";
        r.binaryPath = binary.string();
        r.actionNames.push_back(action);
        r.entries[action] = id + ".Scan::run";
        r.capabilities.insert("filesystem");
        return r;
    }

    void writeMap(const std::filesystem::path& p, const std::string& body) {
        std::ofstream o(p, std::ios::binary);
        o << body;
    }

} // namespace

// ─── Acceptance #1 — multi-format report emission ─────────────

TEST(Phase7AcceptanceTests, testActionCoverageEmitsHtmlConsoleAndSarif) {
    auto m = makeManifest();
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, &m);

    auto d = tempDir("multi");
    auto bin = writeExec(d, "tb", "exit 0");
    auto mapPath = d / "cov.map";
    writeMap(mapPath, R"(# cajeta-coverage-map v1 grain=line
src/a.cajeta 90 100
src/b.cajeta 95 100
)");

    auto html = (d / "cov.html").string();
    auto sarif = (d / "cov.sarif").string();
    auto console = (d / "cov.txt").string();
    auto lcov = (d / "cov.info").string();

    llvm::json::Object covObj;
    covObj["map"] = mapPath.string();
    llvm::json::Object rep;
    rep["html"] = html;
    rep["sarif"] = sarif;
    rep["console"] = console;
    rep["lcov"] = lcov;
    covObj["report"] = std::move(rep);

    llvm::json::Object params;
    params["input"] = bin.string();
    params["coverage"] = std::move(covObj);

    const auto* test = registry.get("test");
    ASSERT_NE(test, nullptr);
    auto r = test->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());

    EXPECT_TRUE(std::filesystem::exists(html));
    EXPECT_TRUE(std::filesystem::exists(sarif));
    EXPECT_TRUE(std::filesystem::exists(console));
    EXPECT_TRUE(std::filesystem::exists(lcov));
    EXPECT_NE(readFileStr(html).find("<!doctype html>"),
              std::string::npos);
    EXPECT_NE(readFileStr(sarif).find("\"version\": \"2.1.0\""),
              std::string::npos);
    EXPECT_NE(readFileStr(console).find("coverage:"),
              std::string::npos);
    EXPECT_NE(readFileStr(lcov).find("SF:src/a.cajeta"),
              std::string::npos);

    // Outputs surface the report paths so downstream actions can
    // pick them up via ${id.coverage-report-html} etc.
    EXPECT_EQ(r->outputs["coverage-report-html"],   html);
    EXPECT_EQ(r->outputs["coverage-report-sarif"],  sarif);
    EXPECT_EQ(r->outputs["coverage-report-console"], console);
    EXPECT_EQ(r->outputs["coverage-report-lcov"],   lcov);

    std::filesystem::remove_all(d);
}

// ─── Acceptance #2 — min: 80 violation cites bottom-N ─────────

TEST(Phase7AcceptanceTests, testActionCoverageMin80ViolationCitesBottomN) {
    auto m = makeManifest();
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, &m);

    auto d = tempDir("gate");
    auto bin = writeExec(d, "tb", "exit 0");
    auto mapPath = d / "cov.map";
    writeMap(mapPath, R"(# cajeta-coverage-map v1 grain=line
src/heavy.cajeta 100 100
src/middling.cajeta 60 100
src/cold.cajeta 5 100
src/icy.cajeta 10 100
)");
    // overall = (100+60+5+10)/400 = 175/400 = 43.75%

    llvm::json::Object covObj;
    covObj["map"] = mapPath.string();
    covObj["min"] = 80;

    llvm::json::Object params;
    params["input"] = bin.string();
    params["coverage"] = std::move(covObj);

    const auto* test = registry.get("test");
    auto r = test->run(params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("coverage gate failed"), std::string::npos);
    EXPECT_NE(msg.find("overall 43"), std::string::npos);
    EXPECT_NE(msg.find("bottom"), std::string::npos);
    // Bottom-N citation: worst three named in order.
    // Worst files are cited first (sorted ascending by percent).
    auto coldLoc     = msg.find("src/cold.cajeta");
    auto icyLoc      = msg.find("src/icy.cajeta");
    auto middlingLoc = msg.find("src/middling.cajeta");
    ASSERT_NE(coldLoc,     std::string::npos);
    ASSERT_NE(icyLoc,      std::string::npos);
    ASSERT_NE(middlingLoc, std::string::npos);
    EXPECT_LT(coldLoc, icyLoc)
        << "cold (5%) should come before icy (10%) in bottom-N: " << msg;
    EXPECT_LT(icyLoc, middlingLoc)
        << "icy (10%) should come before middling (60%): " << msg;

    std::filesystem::remove_all(d);
}


TEST(Phase7AcceptanceTests, testActionCoverageExcludeAppliesBeforeGate) {
    auto m = makeManifest();
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, &m);

    auto d = tempDir("excl");
    auto bin = writeExec(d, "tb", "exit 0");
    auto mapPath = d / "cov.map";
    // Without excludes: overall = (1 + 90) / 110 = 82.7%
    // With test/** excluded: overall = 90/100 = 90%
    writeMap(mapPath, R"(src/main.cajeta 90 100
test/fixture.cajeta 1 10
)");

    llvm::json::Object covObj;
    covObj["map"] = mapPath.string();
    covObj["min"] = 85;  // would fail without excludes
    llvm::json::Array excl;
    excl.emplace_back("test/**");
    covObj["exclude"] = std::move(excl);

    llvm::json::Object params;
    params["input"] = bin.string();
    params["coverage"] = std::move(covObj);

    auto r = registry.get("test")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs["coverage-file-count"], "1");

    std::filesystem::remove_all(d);
}

// ─── Acceptance #3 — security plugin flags a secret pattern ──

TEST(Phase7AcceptanceTests, securityPluginFlagsSecretPattern) {
    // Mock the cajeta.lint.security plugin as a shell script that
    // emits a finding for an AWS-key-shaped secret in a fixture.
    // The real plugin's source lives in
    // build-tools/plugins/security-lint/; this test exercises the
    // contract (capability allowlist + JSON-line finding protocol),
    // not the scanning logic itself.
    auto d = tempDir("sec");
    auto fixturePath = d / "leaky.cajeta";
    {
        std::ofstream o(fixturePath, std::ios::binary);
        o << "package x;\n"
          << "// AKIAIOSFODNN7EXAMPLE\n"
          << "class X {}\n";
    }
    // The mock plugin: reads its request, finds the fixture line
    // with the AWS-key shape, emits a finding pointing at it.
    auto bin = writeExec(d, "sec.sh", R"BIN(
cat > /dev/null
printf '{"kind":"finding",'
printf '"rule":"aws-access-key-id",'
printf '"severity":"error",'
printf '"file":"leaky.cajeta",'
printf '"line":2,"column":4,'
printf '"message":"AWS access key ID pattern matched (AKIA...)"}\n'
printf '{"kind":"result","status":"ok"}\n'
)BIN");

    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("cajeta.lint.security",
                             "cajeta.lint.security.scan",
                             bin);

    llvm::json::Object params;
    params["target-dir"] = d.string();

    auto r = invokePluginAction(
        plugin, "cajeta.lint.security.scan", params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());

    ASSERT_EQ(r->findings.size(), 1u);
    EXPECT_EQ(r->findings[0].rule,     "aws-access-key-id");
    EXPECT_EQ(r->findings[0].severity, "error");
    EXPECT_EQ(r->findings[0].file,     "leaky.cajeta");
    EXPECT_EQ(r->findings[0].line,     2);
    EXPECT_NE(r->findings[0].message.find("AWS"), std::string::npos);

    std::filesystem::remove_all(d);
}

// ─── Acceptance #4 — capability denial ───────────────────────
