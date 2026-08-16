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
// POSIX setenv/unsetenv do not exist in the Windows CRT (which spells both
// _putenv_s), so calling them directly fails to COMPILE on the mingw release
// leg. cajeta::util wraps the split — see util/Environment.h.
#include "cajeta/util/Environment.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <cstring>
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
    // Neither binaryPath nor mainEntry set — nothing to dispatch to.

    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.no-binary.go", params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    // With the `main` distribution model, the pre-fork refusal names BOTH
    // absent selectors (`details.plugin.binary` / `details.plugin.main`).
    EXPECT_NE(msg.find("declares neither"), std::string::npos) << msg;
}


// A released binary bakes ITS BUILD MACHINE's LLVM dir (for CI releases,
// /home/runner/cajeta-llvm/bin), so the context's llc/llvm-dis must be
// resolved against the machine actually running — never handed out
// unchecked. $CAJETA_LLVM_BIN is the explicit override and wins.
TEST(PluginRuntimeTests, toolchainPathsResolveOnThisMachine) {
    auto dir = tempDir("llvmbin");
    auto echoFile = dir / "request.json";
    auto bin = stageScript(dir, "p.sh",
        std::string("cat > '") + echoFile.generic_string() + "'\n"
        "printf '{\"kind\":\"result\",\"status\":\"ok\"}\\n'\n");

    // A directory holding a real (if inert) `llc` — the override target.
    auto llvmDir = dir / "llvm";
    std::filesystem::create_directories(llvmDir);
    auto fakeLlc = stageScript(llvmDir, "llc", "exit 0\n");

    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);
    auto plugin = makePlugin("acme.tc", "acme.tc.go", bin);

    cajeta::util::setEnvVar("CAJETA_LLVM_BIN", llvmDir.generic_string());
    llvm::json::Object params;
    auto r = invokePluginAction(plugin, "acme.tc.go", params, ctx);
    cajeta::util::unsetEnvVar("CAJETA_LLVM_BIN");
    ASSERT_TRUE((bool)r) << errorText(r.takeError());

    std::ifstream in(echoFile);
    std::stringstream ss; ss << in.rdbuf();
    std::string body = ss.str();

    // The override was honored...
    EXPECT_NE(body.find(fakeLlc.generic_string()), std::string::npos);
    // ...and every advertised tool path is one that exists here: an
    // absolute path is only emitted after an is_regular_file check, so
    // any absolute path in the request must resolve.
    for (const char* key : {"\"llc\":\"", "\"llvm-dis\":\""}) {
        auto at = body.find(key);
        ASSERT_NE(at, std::string::npos) << key;
        at += std::strlen(key);
        auto end = body.find('"', at);
        ASSERT_NE(end, std::string::npos);
        std::string path = body.substr(at, end - at);
        if (!path.empty() && path[0] == '/') {
            EXPECT_TRUE(std::filesystem::is_regular_file(path))
                << key << " advertised a nonexistent path: " << path;
        }
    }
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
