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
#include "cajeta/buildtool/OllaStore.h"
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


// ---- §3 — bad input is survivable, and reported once ----------------------
//
// INVERTED 2026-08-28. This block replaces `malformedLineFailsDispatch`,
// which asserted that one unreadable line aborted the dispatch. That is the
// opposite of the approved spec (§4): a plugin that emits a malformed record
// keeps working, the record is dropped, and the build tool warns once.
//
// The inversion is the point of the section. Failing the action on a bad line
// makes the build tool a ceiling on every plugin that ever shipped a typo —
// and `dev.cajeta.coverage` 0.5.x is in the wild emitting unescaped messages
// today. Being lenient here and strict in the CONFORMANCE suite (§0) is what
// lets the protocol tighten without breaking anyone mid-build.

namespace {

    // Run a plugin whose stdout is `body`, capturing the build tool's stderr.
    struct Ran {
        bool ok = false;
        std::string error;
        std::string stderrText;
        ActionResult result;
    };

    Ran runPlugin(const std::string& tag, const std::string& body) {
        auto dir = tempDir(tag);
        auto bin = stageScript(dir, "p.sh", "cat > /dev/null\n" + body);
        auto m = makeManifest();
        auto props = makeProps(m);
        TaskContext ctx(props, &m);
        auto plugin = makePlugin("acme." + tag, "acme." + tag + ".go", bin);

        llvm::json::Object params;
        Ran out;
        testing::internal::CaptureStderr();
        auto r = invokePluginAction(plugin, "acme." + tag + ".go", params, ctx);
        out.ok = (bool) r;
        if (out.ok) {
            out.result = std::move(*r);
        } else {
            out.error = errorText(r.takeError());
        }
        out.stderrText = testing::internal::GetCapturedStderr();
        return out;
    }

    // Lines that BEGIN with `prefix`. The substring alone is the wrong test:
    // an escaped payload may legitimately contain the word `warning:` — that
    // is exactly what being escaped means, and counting it would report a
    // forgery where the quoting had just done its job.
    int countLinesStartingWith(const std::string& hay, const std::string& prefix) {
        int n = 0;
        size_t i = 0;
        while (i < hay.size()) {
            size_t nl = hay.find('\n', i);
            if (nl == std::string::npos) nl = hay.size();
            if (hay.compare(i, prefix.size(), prefix) == 0) ++n;
            i = nl + 1;
        }
        return n;
    }

}  // namespace

// 3.1.4 — raw non-JSON text is `printf` debugging, not a protocol violation.
// It is accepted and surfaced as a log; no warning, no failure. This is the
// exact script the old malformedLineFailsDispatch used to prove the opposite.
TEST(PluginRuntimeTests, rawTextIsAcceptedAsALog) {
    auto r = runPlugin("rawtext", R"(
printf 'not even close to json\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.stderrText.find("warning:"), std::string::npos)
        << "raw text is deliberate, so it must not warn: " << r.stderrText;
}

// 3.1.1 — a line that ATTEMPTED a record and is not valid JSON is dropped.
// The leading brace is what separates this from the case above.
TEST(PluginRuntimeTests, malformedJsonIsDroppedAndTheActionContinues) {
    auto r = runPlugin("badjson", R"(
printf '{"kind":"log","message":"unterminated\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.stderrText.find("warning:"), std::string::npos);
    EXPECT_NE(r.stderrText.find("not valid JSON"), std::string::npos)
        << r.stderrText;
}

// 3.1.2 — an unknown kind is a NEWER plugin talking to an older build tool.
// Dropped and warned, never fatal: refusing it would make this build a
// ceiling on every plugin released after it.
TEST(PluginRuntimeTests, anUnknownKindIsDroppedAndTheActionContinues) {
    auto r = runPlugin("unknownkind", R"(
printf '{"kind":"telemetry","spans":3}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.stderrText.find("unrecognised record kind"), std::string::npos)
        << r.stderrText;
}

// 3.1.3 — a known kind missing a required field. The record is dropped and
// the ones around it still land, so one bad record costs one record.
TEST(PluginRuntimeTests, aRecordMissingARequiredFieldIsDropped) {
    auto r = runPlugin("missingfield", R"(
printf '{"kind":"output","key":"only-a-key"}\n'
printf '{"kind":"output","key":"good","value":"kept"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.stderrText.find("required field"), std::string::npos)
        << r.stderrText;
    EXPECT_EQ(r.result.outputs.count("only-a-key"), 0u);
    ASSERT_EQ(r.result.outputs.count("good"), 1u)
        << "a dropped record must not take its neighbours with it";
    EXPECT_EQ(r.result.outputs["good"], "kept");
}

// 3.1.5 — ten bad records, ONE warning. A plugin in a bad state can emit
// thousands; repeating the warning would bury the build's real output, which
// is the same failure this spec exists to remove, one layer up.
TEST(PluginRuntimeTests, manyBadRecordsProduceOneWarning) {
    auto r = runPlugin("flood", R"(
i=0
while [ $i -lt 10 ]; do printf '{"kind":"nope","n":%d}\n' "$i"; i=$((i+1)); done
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(countLinesStartingWith(r.stderrText, "warning:"), 1) << r.stderrText;
    EXPECT_NE(r.stderrText.find("10 records"), std::string::npos)
        << "the warning should say how many were dropped: " << r.stderrText;
}

// 3.1.6 / 3.3.3 — the warning names the offending line. Telling an author
// that "something" was unreadable without showing what leaves them guessing
// at the one thing the build tool actually knows.
TEST(PluginRuntimeTests, theWarningNamesTheOffendingLine) {
    auto r = runPlugin("namesline", R"(
printf '{"kind":"marker-xyzzy","payload":1}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.stderrText.find("marker-xyzzy"), std::string::npos)
        << r.stderrText;
    EXPECT_NE(r.stderrText.find("acme.namesline"), std::string::npos)
        << "and names the plugin: " << r.stderrText;
}

// 3.1.7 — the offending line is bytes the PLUGIN controls. Echoing it must
// not damage the stream reporting it: a quote or backslash is escaped, so a
// diagnostic carrying it still parses.
TEST(PluginRuntimeTests, theWarningEscapesQuotesAndBackslashes) {
    auto r = runPlugin("hostilequote", R"(
printf '{"kind":"nope","p":"a\\"b C:\\\\x"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    // Escaped, not reproduced: no bare quote-backslash pair survives.
    EXPECT_NE(r.stderrText.find("\\\""), std::string::npos) << r.stderrText;
}

// 3.1.8 — a control character in the offending line cannot forge a second
// console line. A carriage return reaching a terminal verbatim would let a
// plugin overwrite the warning about itself.
TEST(PluginRuntimeTests, theWarningCannotForgeASecondConsoleLine) {
    auto r = runPlugin("forge", R"(
printf '{"kind":"nope"\rwarning: forged and harmless\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    // The forged text is present but INERT: it sits inside the escaped
    // payload of the one real warning, on the same console line.
    EXPECT_EQ(countLinesStartingWith(r.stderrText, "warning:"), 1)
        << "the plugin forged a second warning line: " << r.stderrText;
    EXPECT_EQ(r.stderrText.find('\r'), std::string::npos)
        << "a raw CR reached the console: " << r.stderrText;
    EXPECT_NE(r.stderrText.find("\\rwarning: forged"), std::string::npos)
        << "the CR should be escaped in place, not dropped: " << r.stderrText;
}

// 3.1.9 — a megabyte of garbage cannot flood the build log.
TEST(PluginRuntimeTests, anOverlongOffendingLineIsTruncated) {
    auto r = runPlugin("overlong", R"(
printf '{"kind":"nope","p":"'
i=0
while [ $i -lt 2000 ]; do printf 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'; i=$((i+1)); done
printf '"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.stderrText.find("truncated"), std::string::npos)
        << "an overlong line must be cut with an explicit marker";
    EXPECT_LT(r.stderrText.size(), 1024u)
        << "64 KB of plugin garbage reached the log";
}

// 3.1.10 — invalid UTF-8 is reported without corrupting the output encoding.
// The bytes are shown as escapes, so whatever the plugin emitted, the log
// stays decodable.
TEST(PluginRuntimeTests, invalidUtf8IsReportedWithoutCorruptingTheEncoding) {
    auto r = runPlugin("badutf8", R"(
printf '{"kind":"nope","p":"ok\200\377"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    for (unsigned char c : r.stderrText) {
        EXPECT_LT(c, 0x80u) << "a raw high byte reached the log";
    }
}

// 3.3.2 — every hostile shape at once still leaves the build alive, still
// warns once, and still delivers the records that were fine.
TEST(PluginRuntimeTests, aStreamOfHostileLinesLeavesTheBuildAlive) {
    auto r = runPlugin("hostileall", R"(
printf '{"kind":"log","message":"unterminated\n'
printf '{"kind":"telemetry"}\n'
printf '{"kind":"output","key":"no-value"}\n'
printf 'plain printf debugging\n'
printf '{"kind":"nope","p":"ok\200\377"}\n'
printf '{"kind":"output","key":"survivor","value":"still here"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(countLinesStartingWith(r.stderrText, "warning:"), 1) << r.stderrText;
    ASSERT_EQ(r.result.outputs.count("survivor"), 1u);
    EXPECT_EQ(r.result.outputs["survivor"], "still here");
}

// The negative arm. Dropping bad records must not have made the runtime
// accept ANYTHING — a plugin that never reports still fails, and a plugin
// whose only result record was unreadable is a plugin that did not report.
TEST(PluginRuntimeTests, aDroppedResultIsStillNoResult) {
    auto r = runPlugin("noresult", R"(
printf '{"kind":"log","level":"info","message":"working"}\n'
printf '{"kind":"result","status":"maybe"}\n'
)");
    EXPECT_FALSE(r.ok) << "an uninterpretable status is not a report";
    EXPECT_NE(r.error.find("no 'result' record"), std::string::npos) << r.error;
    EXPECT_NE(r.stderrText.find("unknown status"), std::string::npos)
        << r.stderrText;
}

// 3.3.1 — the compatibility promise, against the PUBLISHED artifact
//
// "Nothing published breaks" is the whole point of §3, and the only way to
// know is to run what is actually out there. A rebuild from current source
// would prove nothing: it would be built by this toolchain, against this
// stdlib, with this session's fixes already in it. The fixture is the binary
// in the local olla store, exactly as it was published.
//
// The plan named 0.5.2. The store's `versions.json` lists 0.3.0, 0.4.0,
// 0.5.0 and 0.5.1 — there is no 0.5.2, so this takes the newest version that
// is actually there and the plan/spec were corrected to match.
//
// SKIPS when the store has no such artifact: this is a machine-local fixture,
// and a test that silently passed on a machine without it would be worse than
// one that says why it did not run.
TEST(PluginRuntimeTests, thePublishedCoveragePluginStillRuns) {
    namespace fs = std::filesystem;

    const fs::path pkg =
        fs::path(cajeta::buildtool::OllaStore::resolveRoot()) /
        "dev.cajeta.coverage";
    if (!fs::is_directory(pkg)) {
        GTEST_SKIP() << "no dev.cajeta.coverage in the olla store at " << pkg;
    }

    std::string version;
    fs::path binary;
    for (const auto& e : fs::directory_iterator(pkg)) {
        if (!e.is_directory()) continue;
        auto candidate = e.path() / "bin" / "dev.cajeta.coverage";
        if (!fs::is_regular_file(candidate)) continue;
        if (e.path().filename().string() > version) {
            version = e.path().filename().string();
            binary = candidate;
        }
    }
    if (version.empty()) {
        GTEST_SKIP() << "no built dev.cajeta.coverage binary under " << pkg;
    }

    auto m = makeManifest();
    auto props = makeProps(m);
    TaskContext ctx(props, &m);

    ResolvedPlugin plugin;
    plugin.name = "dev.cajeta.coverage";
    plugin.version = version;
    plugin.resolvedFromRepo = "olla";
    plugin.artifactPath = binary.string();
    plugin.binaryPath = binary.string();
    plugin.actionNames = {"cajeta.coverage.report"};
    plugin.entries["cajeta.coverage.report"] =
        "cajeta.coco.plugin.PluginHost::serve";
    plugin.capabilities = {"filesystem", "process"};

    llvm::json::Object params;
    testing::internal::CaptureStderr();
    auto r = invokePluginAction(plugin, "cajeta.coverage.report", params, ctx);
    const bool ok = (bool) r;
    std::string error;
    if (!ok) error = errorText(r.takeError());
    const std::string err = testing::internal::GetCapturedStderr();

    // The action itself may well fail — with no instrument pass there are no
    // sites to report on, and that is the PLUGIN's own logic. What must not
    // happen is a PROTOCOL failure: the new validating path must read
    // everything this artifact emits.
    EXPECT_EQ(error.find("not valid JSON"), std::string::npos) << error;
    EXPECT_EQ(error.find("protocol violation"), std::string::npos) << error;
    EXPECT_EQ(error.find("no 'result' record"), std::string::npos) << error;
    EXPECT_EQ(countLinesStartingWith(err, "warning:"), 0)
        << "a published plugin emitted a record this build dropped: " << err;

    // Non-vacuity: the subprocess really ran and its result really was read.
    // Without this the test would pass just as happily against a binary that
    // never executed, which is the failure mode a compatibility fixture is
    // least able to afford.
    if (!ok) {
        EXPECT_EQ(error.rfind("cajeta.plugin: dev.cajeta.coverage", 0), 0u)
            << "expected a result the PLUGIN reported, got: " << error;
    }
    RecordProperty("pluginVersion", version);
    RecordProperty("pluginOutcome", ok ? std::string("ok") : error);
}
