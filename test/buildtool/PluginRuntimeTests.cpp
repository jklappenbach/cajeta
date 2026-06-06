// Regression tests for the Phase 7c plugin subprocess runtime.
//
// Strategy: stage a shell-script "plugin binary" per scenario. The
// script speaks the JSON-line protocol documented in
// PluginRuntime.cpp's header — emits log/output/finding/result
// records on stdout, exits 0 unless the test wants a crash.
//
// Each fixture builds a ResolvedPlugin pointing at the script and
// invokes the runtime; the assertions pin the parent-side parsing
// + result aggregation.

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
#if defined(_WIN32)
#  include <process.h>   // _getpid
#  define CAJETA_GETPID _getpid
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  define CAJETA_GETPID ::getpid
#endif

using cajeta::buildtool::ActionResult;
using cajeta::buildtool::invokePluginAction;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
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

    std::filesystem::path tempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-plugin-" + tag + "-" +
                  std::to_string(CAJETA_GETPID()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Write a shell script + chmod +x; return its path. The script
    // body is whatever the test wants the plugin to emit — typically
    // a fixed JSON-line stream piped to stdout + exit 0.
    std::filesystem::path stageScript(
        const std::filesystem::path& dir,
        const std::string& name,
        const std::string& body) {
        auto path = dir / name;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "#!/bin/sh\n" << body;
        }
#if !defined(_WIN32)
        ::chmod(path.string().c_str(), 0755);  // exec bit is a POSIX concept
#endif
        return path;
    }

    // Build a manifest with the matching consumer details so the
    // TaskContext has a Manifest to read project-name/version from.
    Manifest makeManifest() {
        auto m = loadManifestString(
            R"({"details":{"name":"acme.app","version":"1.2.3"}})");
        EXPECT_TRUE((bool)m);
        return std::move(*m);
    }

    cajeta::buildtool::ResolvedProperties makeProps(const Manifest& m) {
        auto r = resolveProperties(m);
        EXPECT_TRUE((bool)r);
        return std::move(*r);
    }

    ResolvedPlugin makePlugin(
        const std::string& name,
        const std::string& actionName,
        const std::filesystem::path& binary) {
        ResolvedPlugin p;
        p.name = name;
        p.version = "1.0.0";
        p.resolvedFromRepo = "test";
        p.artifactPath = binary.string();
        p.binaryPath = binary.string();
        p.actionNames = {actionName};
        p.entries[actionName] = name + ".Entry::run";
        p.capabilities = {"filesystem"};
        return p;
    }

} // namespace

TEST(PluginRuntimeTests, propagatesOutputsFromProtocolStream) {
    auto dir = tempDir("outputs");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf '{"kind":"output","key":"a","value":"1"}\n'
printf '{"kind":"output","key":"b","value":"2"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.demo", "acme.demo.go", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.demo.go", params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs["a"], "1");
    EXPECT_EQ(r->outputs["b"], "2");
}

TEST(PluginRuntimeTests, findingsArriveAsTypedActionFindings) {
    auto dir = tempDir("findings");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf '{"kind":"finding","rule":"r1","severity":"warning","file":"f.cajeta","line":12,"column":3,"message":"m1"}\n'
printf '{"kind":"finding","rule":"r2","severity":"info","file":"g.cajeta","line":42,"column":1,"message":"m2"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.lint", "acme.lint.scan", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.lint.scan", params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    // Typed delivery: ActionResult.findings is a vector of
    // ActionFinding, not a JSON-concat string. The lint task can
    // aggregate across actions without re-parsing.
    ASSERT_EQ(r->findings.size(), 2u);
    EXPECT_EQ(r->findings[0].rule,     "r1");
    EXPECT_EQ(r->findings[0].severity, "warning");
    EXPECT_EQ(r->findings[0].file,     "f.cajeta");
    EXPECT_EQ(r->findings[0].line,     12);
    EXPECT_EQ(r->findings[0].column,   3);
    EXPECT_EQ(r->findings[0].message,  "m1");
    EXPECT_EQ(r->findings[1].rule,     "r2");
    EXPECT_EQ(r->findings[1].severity, "info");
    // Default severity is "info" when the plugin omits the field.
}

TEST(PluginRuntimeTests, findingWithoutSeverityDefaultsToInfo) {
    auto dir = tempDir("findings-def");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf '{"kind":"finding","rule":"r","file":"f","line":1,"column":1,"message":"m"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.lint", "acme.lint.scan", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.lint.scan", params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    ASSERT_EQ(r->findings.size(), 1u);
    EXPECT_EQ(r->findings[0].severity, "info");
}

TEST(PluginRuntimeTests, resultErrorFailsTheAction) {
    auto dir = tempDir("err");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf '{"kind":"output","key":"percent","value":"73.5"}\n'
printf '{"kind":"result","status":"error","message":"73.5%% < min 80%%"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.cov", "acme.cov.gate", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.cov.gate", params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("73.5"), std::string::npos);
    EXPECT_NE(msg.find("acme.cov.gate"), std::string::npos);
}

TEST(PluginRuntimeTests, missingResultRecordIsProtocolError) {
    auto dir = tempDir("noresult");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf '{"kind":"output","key":"x","value":"1"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.bad", "acme.bad.go", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.bad.go", params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("no 'result' record"), std::string::npos);
}

TEST(PluginRuntimeTests, nonZeroExitSurfacesAsError) {
    auto dir = tempDir("crash");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
exit 7
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.x", "acme.x.go", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.x.go", params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("exited 7"), std::string::npos);
}

TEST(PluginRuntimeTests, missingBinaryFailsBeforeFork) {
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    ResolvedPlugin plugin;
    plugin.name = "acme.no-binary";
    plugin.actionNames = {"acme.no-binary.go"};
    // No binaryPath set.

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.no-binary.go", params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("no binary declared"), std::string::npos);
}

TEST(PluginRuntimeTests, requestCarriesParamsAndCapabilities) {
    // Echo the stdin verbatim into a per-test scratch file the test
    // can inspect to confirm the request shape.
    auto dir = tempDir("echo");
    auto echoFile = dir / "request.json";
    auto bin = stageScript(dir, "p.sh",
        std::string("cat > '") + echoFile.generic_string() + "'\n"
        "printf '{\"kind\":\"result\",\"status\":\"ok\"}\\n'\n");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.echo", "acme.echo.go", bin);
    plugin.capabilities = {"filesystem", "network"};

    llvm::json::Object params;
    params["min"] = 80;
    params["report"] = llvm::json::Array{"html", "console"};

    auto r = invokePluginAction(plugin, "acme.echo.go", params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());

    std::ifstream in(echoFile);
    std::stringstream ss; ss << in.rdbuf();
    std::string body = ss.str();
    EXPECT_NE(body.find("\"action\":\"acme.echo.go\""), std::string::npos);
    EXPECT_NE(body.find("\"min\":80"), std::string::npos);
    EXPECT_NE(body.find("\"html\""), std::string::npos);
    EXPECT_NE(body.find("\"filesystem\""), std::string::npos);
    EXPECT_NE(body.find("\"network\""), std::string::npos);
    EXPECT_NE(body.find("\"project-name\":\"acme.app\""), std::string::npos);
    EXPECT_NE(body.find("\"project-version\":\"1.2.3\""), std::string::npos);
    EXPECT_NE(body.find("\"entry\":\"acme.echo.Entry::run\""),
              std::string::npos);
}

TEST(PluginRuntimeTests, unknownRecordKindIsIgnoredForwardCompat) {
    auto dir = tempDir("fwd");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf '{"kind":"futurekind","payload":42}\n'
printf '{"kind":"output","key":"k","value":"v"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.f", "acme.f.go", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.f.go", params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs["k"], "v");
}

TEST(PluginRuntimeTests, malformedLineFailsDispatch) {
    auto dir = tempDir("bad");
    auto bin = stageScript(dir, "p.sh", R"(
cat > /dev/null
printf 'not even close to json\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.bad", "acme.bad.go", bin);

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.bad.go", params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not valid JSON"), std::string::npos);
}
