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
#include "cajeta/buildtool/DiagnosticFormat.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/error/Diagnostics.h"
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
#include <regex>
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
        std::string stdoutText;
        ActionResult result;
    };

    // --diag-format=json is process-global (two switches: the build tool's
    // format and the emitter's gate — `resolveDiagFormatFromArgv` ties them in
    // the real binary, and a test that set only one would silently emit
    // nothing). RAII so a failing assertion cannot leave JSON mode on for
    // every test that runs after it.
    struct JsonModeForTest {
        JsonModeForTest() {
            cajeta::buildtool::setDiagnosticFormat(cajeta::DiagFormat::Json);
            cajeta::setJsonProgressEnabled(true);
        }
        ~JsonModeForTest() {
            cajeta::buildtool::setDiagnosticFormat(cajeta::DiagFormat::Text);
            cajeta::setJsonProgressEnabled(false);
        }
    };

    // Every NDJSON object on stderr, in order.
    std::vector<llvm::json::Object> records(const std::string& text) {
        std::vector<llvm::json::Object> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto parsed = llvm::json::parse(line);
            if (!parsed) { llvm::consumeError(parsed.takeError()); continue; }
            if (auto* o = parsed->getAsObject()) out.push_back(*o);
        }
        return out;
    }

    std::string field(const llvm::json::Object& o, const char* k) {
        auto v = o.getString(k);
        return v ? v->str() : std::string();
    }

    Ran runPlugin(const std::string& tag, const std::string& body) {
        auto dir = tempDir(tag);
        auto bin = stageScript(dir, "p.sh", "cat > /dev/null\n" + body);
        auto m = makeManifest();
        auto props = makeProps(m);
        TaskContext ctx(props, &m);
        auto plugin = makePlugin("acme." + tag, "acme." + tag + ".go", bin);

        llvm::json::Object params;
        Ran out;
        testing::internal::CaptureStdout();
        testing::internal::CaptureStderr();
        auto r = invokePluginAction(plugin, "acme." + tag + ".go", params, ctx);
        out.ok = (bool) r;
        if (out.ok) {
            out.result = std::move(*r);
        } else {
            out.error = errorText(r.takeError());
        }
        out.stderrText = testing::internal::GetCapturedStderr();
        out.stdoutText = testing::internal::GetCapturedStdout();
        return out;
    }

    // One plugin emitting one of everything. Shared so the JSON and text
    // assertions below are about the same input.
    //
    // Deliberately NON-FAILING: since §7a an `error` finding fails the task,
    // and this fixture exists to exercise every record KIND, not to gate. Its
    // located finding is a warning for that reason. Error severity is
    // exercised in §6, where failing is the point — when that rule landed, 12
    // tests here went red at once, which was the rule working rather than a
    // regression.
    const char* kEveryKindScript = R"(
printf '{"kind":"log","level":"info","message":"progress"}\n'
printf '{"kind":"warn","message":"config is odd"}\n'
printf '{"kind":"finding","severity":"warning","rule":"cov","file":"src/A.cajeta","line":12,"column":5,"message":"uncovered"}\n'
printf '{"kind":"finding","severity":"info","message":"no position"}\n'
printf '{"kind":"write","text":"78.2%% covered"}\n'
printf '{"kind":"output","key":"path","value":"build/report.html"}\n'
printf '{"kind":"result","status":"ok"}\n'
)";

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

    // PINNED, not "newest available", and the pin is the whole point.
    //
    // 0.5.1 is the last version published BEFORE this protocol work. Taking
    // the newest instead would silently retarget this test the moment coco's
    // migrated build is installed locally (§7 does exactly that to run its
    // tour gate) — and it would still pass, because a migrated coco conforms.
    // A compatibility test that quietly starts measuring the new thing is
    // worse than one that does not run: it reports a guarantee nobody
    // checked.
    const std::string version = "0.5.1";
    const fs::path binary = pkg / version / "bin" / "dev.cajeta.coverage";
    if (!fs::is_regular_file(binary)) {
        GTEST_SKIP() << "no published dev.cajeta.coverage " << version
                     << " binary at " << binary;
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

// ---- §4 — JSON mode: one stream, one grammar -------------------------------
//
// Under --diag-format=json a plugin's output JOINS the diagnostic stream
// rather than forming a parallel one. A consumer that already parses compiler
// diagnostics gets coco's findings with no new parsing, and can tell they came
// from a plugin.

// 4.1.1 / 4.1.2 — a located finding is a navigable diagnostic at its own
// severity. This is the use case the section exists for.
TEST(PluginRuntimeTests, jsonModeTurnsALocatedFindingIntoANavigableDiagnostic) {
    JsonModeForTest json;
    auto r = runPlugin("jsonfinding", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    const auto recs = records(r.stderrText);
    const llvm::json::Object* located = nullptr;
    for (const auto& o : recs) {
        if (field(o, "kind") == "diagnostic" && field(o, "message") == "uncovered") {
            located = &o;
        }
    }
    ASSERT_NE(located, nullptr) << r.stderrText;
    EXPECT_EQ(field(*located, "severity"), "warning");
    EXPECT_EQ(field(*located, "code"), "cov");
    EXPECT_EQ(field(*located, "file"), "src/A.cajeta");
    EXPECT_EQ(located->getInteger("line").value_or(0), 12);
    EXPECT_EQ(located->getInteger("column").value_or(0), 5);
}

// 4.1.3 — the negative arm, and the one that matters. A fabricated 0:0
// navigates SOMEWHERE, which is worse than navigating nowhere.
TEST(PluginRuntimeTests, jsonModeGivesAnUnlocatedFindingNoLocation) {
    JsonModeForTest json;
    auto r = runPlugin("jsonbare", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    const auto recs = records(r.stderrText);
    const llvm::json::Object* bare = nullptr;
    for (const auto& o : recs) {
        if (field(o, "kind") == "diagnostic" && field(o, "message") == "no position") {
            bare = &o;
        }
    }
    ASSERT_NE(bare, nullptr) << r.stderrText;
    EXPECT_EQ(field(*bare, "severity"), "note") << "info maps to note (SARIF)";
    EXPECT_TRUE(bare->get("file") && bare->getString("file") == std::nullopt)
        << "file must be null, not a path";
    EXPECT_FALSE(bare->getInteger("line").has_value()) << "line must be null";
    EXPECT_FALSE(bare->getInteger("column").has_value()) << "column must be null";
}

// 4.1.4 — warn becomes a warning; an info log becomes a note.
TEST(PluginRuntimeTests, jsonModeMapsWarnToWarningAndLogToNote) {
    JsonModeForTest json;
    auto r = runPlugin("jsonlevels", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    bool sawWarning = false, sawNote = false;
    for (const auto& o : records(r.stderrText)) {
        if (field(o, "kind") != "diagnostic") continue;
        if (field(o, "message") == "config is odd" &&
            field(o, "severity") == "warning") sawWarning = true;
        if (field(o, "message") == "progress" &&
            field(o, "severity") == "note") sawNote = true;
    }
    EXPECT_TRUE(sawWarning) << r.stderrText;
    EXPECT_TRUE(sawNote) << r.stderrText;
}

// 4.1.5 — output, result and write stay their OWN kinds. Flattening them into
// messages would leave a consumer parsing prose to recover data it was handed
// structurally.
TEST(PluginRuntimeTests, jsonModeKeepsOutputResultAndWriteStructural) {
    JsonModeForTest json;
    auto r = runPlugin("jsonstructural", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    // Bound to a named local: `records()` returns by value, and pointers into
    // the temporary a range-for iterates would dangle the moment the loop ends.
    const auto recs = records(r.stderrText);
    const llvm::json::Object* output = nullptr;
    const llvm::json::Object* write = nullptr;
    const llvm::json::Object* result = nullptr;
    for (const auto& o : recs) {
        const auto k = field(o, "kind");
        if (k == "output") output = &o;
        if (k == "write")  write  = &o;
        if (k == "result") result = &o;
    }
    ASSERT_NE(output, nullptr) << r.stderrText;
    EXPECT_EQ(field(*output, "key"), "path");
    EXPECT_EQ(field(*output, "value"), "build/report.html");

    ASSERT_NE(write, nullptr) << r.stderrText;
    EXPECT_EQ(field(*write, "text"), "78.2% covered");

    ASSERT_NE(result, nullptr) << r.stderrText;
    EXPECT_EQ(field(*result, "status"), "ok");

    // And the output still reaches the invoking task, which is the other half
    // of "structural": a stream consumer AND ${id.key} both get it.
    ASSERT_EQ(r.result.outputs.count("path"), 1u);
    EXPECT_EQ(r.result.outputs["path"], "build/report.html");
}

// 4.1.7 — every record the plugin caused carries the plugin's Olla key: the
// same string as its `plugins` entry in cajeta.json, so a reader goes from a
// diagnostic straight to the entry that declared it, with no translation.
TEST(PluginRuntimeTests, everyRecordCarriesTheEmittingPluginsKey) {
    JsonModeForTest json;
    auto r = runPlugin("jsonprov", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    const auto recs = records(r.stderrText);
    ASSERT_FALSE(recs.empty()) << r.stderrText;
    for (const auto& o : recs) {
        EXPECT_EQ(field(o, "source"), "acme.jsonprov")
            << "record without the plugin's key: " << field(o, "kind");
        EXPECT_EQ(field(o, "sourceVersion"), "1.0.0");
    }
}

// 4.1.8 / 4.1.9 / 4.3.3 — a plugin's CLAIM about its own origin is discarded.
// Provenance is stamped by the build tool from the plugin it chose to invoke,
// so a plugin cannot launder a diagnostic into looking like the compiler's, or
// like another plugin's. The claim is not rejected, it is simply never read.
TEST(PluginRuntimeTests, aPluginCannotForgeItsOwnProvenance) {
    JsonModeForTest json;
    auto r = runPlugin("liar", R"(
printf '{"kind":"warn","message":"I am the compiler","source":"cajeta","sourceVersion":"99.0.0"}\n'
printf '{"kind":"warn","message":"I am someone else","source":"dev.cajeta.coverage"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    ASSERT_TRUE(r.ok) << r.error;

    const auto recs = records(r.stderrText);
    ASSERT_FALSE(recs.empty()) << r.stderrText;
    for (const auto& o : recs) {
        EXPECT_EQ(field(o, "source"), "acme.liar")
            << "a plugin's claimed origin was recorded: " << r.stderrText;
        EXPECT_NE(field(o, "sourceVersion"), "99.0.0");
    }
}

// 4.3.2 — the stream filters by provenance ALONE. No plugin-specific parsing,
// no kind allowlist: a consumer that wants compiler-only, or one plugin's,
// reads one field.
TEST(PluginRuntimeTests, theStreamFiltersByProvenanceAlone) {
    JsonModeForTest json;
    auto r = runPlugin("jsonfilter", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    int fromPlugin = 0, fromCompiler = 0;
    for (const auto& o : records(r.stderrText)) {
        if (field(o, "source") == "acme.jsonfilter") ++fromPlugin;
        else if (field(o, "source") == "cajeta") ++fromCompiler;
        else ADD_FAILURE() << "record with no usable source: " << field(o, "kind");
    }
    EXPECT_GT(fromPlugin, 0);
    EXPECT_EQ(fromCompiler, 0) << "nothing here was the compiler's";
}

// 4.3.1 — the findings parse with the compiler's OWN diagnostic reader. The
// point of joining the stream is that a consumer needs no plugin-specific
// code, so this asserts the shape a diagnostic consumer requires and nothing
// plugin-shaped at all.
TEST(PluginRuntimeTests, pluginFindingsParseAsOrdinaryDiagnostics) {
    JsonModeForTest json;
    auto r = runPlugin("jsonreader", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    int diagnostics = 0;
    for (const auto& o : records(r.stderrText)) {
        if (field(o, "kind") != "diagnostic") continue;
        ++diagnostics;
        const auto sev = field(o, "severity");
        EXPECT_TRUE(sev == "error" || sev == "warning" || sev == "note")
            << "severity outside the diagnostic vocabulary: " << sev;
        EXPECT_TRUE(o.get("message") != nullptr) << "diagnostic with no message";
    }
    EXPECT_GE(diagnostics, 4) << "log, warn and two findings: " << r.stderrText;
}

// 4.1.6 / 4.3.4 — text mode is untouched by all of the above. Pinned as exact
// bytes rather than "contains", because the guarantee is byte-identical output
// and a `contains` assertion would pass while the format drifted around it.
TEST(PluginRuntimeTests, textModeOutputIsUnchangedByJsonMode) {
    // No JsonModeForTest — this is the default path.
    auto r = runPlugin("textmode", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    EXPECT_EQ(r.stdoutText, "[plugin] progress\n78.2% covered");
    // §5 added the two finding lines to stderr. Everything else here is
    // byte-identical, which is the guarantee that mattered.
    EXPECT_EQ(r.stderrText,
              "warning: config is odd\n"
              "acme.textmode: src/A.cajeta:12:5: warning: uncovered [cov]\n"
              "acme.textmode: info: no position\n");

    // Not one structured record escaped into text mode.
    EXPECT_EQ(r.stdoutText.find("\"kind\":"), std::string::npos);
    EXPECT_EQ(r.stderrText.find("\"kind\":"), std::string::npos);

    // And the structured data still reached the task.
    ASSERT_EQ(r.result.outputs.count("path"), 1u);
    EXPECT_EQ(r.result.findings.size(), 2u);
}

// The control for JsonModeForTest. If the switch did not actually take, every
// §4 assertion above would be reading an empty stream and passing on an empty
// loop — the failure mode a mode-scoped test is least able to see.
TEST(PluginRuntimeTests, theJsonModeSwitchActuallyTakes) {
    std::string withJson, withoutJson;
    {
        JsonModeForTest json;
        withJson = runPlugin("switchon", kEveryKindScript).stderrText;
    }
    withoutJson = runPlugin("switchoff", kEveryKindScript).stderrText;

    EXPECT_FALSE(records(withJson).empty()) << "json mode emitted no records";
    EXPECT_TRUE(records(withoutJson).empty())
        << "text mode emitted records: " << withoutJson;
}

// ---- §5 — text mode: findings look like diagnostics ------------------------
//
// A finding is not progress and stops being rendered as such. It prints in the
// compiler's own line grammar so the IDE Build window makes it clickable, and
// it NAMES the plugin so it is never mistaken for compiler output.

// 5.3.1 — asserted against the GRAMMAR, not a hand-written expected string.
// The point is that a plugin finding and a compiler diagnostic are the same
// shape to a reader; pinning a literal would prove only that this code emits
// what this test expects.
namespace {

    // <producer>: [<file>:<line>:<col>: ]<tag>: <message>[ [rule]]
    //
    // Derived from DiagnosticEngine::emit, which writes
    //   cajeta: src/A.cajeta:12:5: CAJETA_ERROR_X: message
    //   cajeta: CAJETA_ERROR_X: message
    const std::regex& diagnosticLineGrammar() {
        static const std::regex re(
            R"(^([A-Za-z0-9_.-]+): (?:(.+):([0-9]+):([0-9]+): )?([A-Za-z_][A-Za-z0-9_]*): (.+)$)");
        return re;
    }

}  // namespace

// 5.1.1 / 5.1.2 / 5.3.1 — a located finding is a diagnostic-shaped, named line.
TEST(PluginRuntimeTests, textModeRendersALocatedFindingAsADiagnostic) {
    auto r = runPlugin("textlocated", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    const std::string line = "acme.textlocated: src/A.cajeta:12:5: warning: uncovered [cov]";
    EXPECT_NE(r.stderrText.find(line + "\n"), std::string::npos) << r.stderrText;

    std::smatch m;
    ASSERT_TRUE(std::regex_match(line, m, diagnosticLineGrammar()))
        << "the rendered finding is not in the compiler's line grammar";
    EXPECT_EQ(m[1].str(), "acme.textlocated") << "names the plugin";
    EXPECT_EQ(m[2].str(), "src/A.cajeta");
    EXPECT_EQ(m[3].str(), "12");
    EXPECT_EQ(m[4].str(), "5");
    EXPECT_EQ(m[5].str(), "warning");
}

// The other half of 5.3.1: the SAME grammar matches what the compiler emits,
// which is the whole claim — one shape, so the IDE's existing filter needs no
// plugin-specific case.
TEST(PluginRuntimeTests, theGrammarMatchesACompilerDiagnosticToo) {
    const std::string compilerLocated =
        "cajeta: src/A.cajeta:12:5: CAJETA_ERROR_UNRESOLVED_TYPE: unknown type";
    const std::string compilerBare =
        "cajeta: CAJETA_ERROR_NO_ENTRY: no entry point";

    std::smatch m;
    ASSERT_TRUE(std::regex_match(compilerLocated, m, diagnosticLineGrammar()));
    EXPECT_EQ(m[1].str(), "cajeta");
    EXPECT_EQ(m[2].str(), "src/A.cajeta");
    EXPECT_TRUE(std::regex_match(compilerBare, m, diagnosticLineGrammar()));

    // And the negative arm: the grammar has to be capable of REJECTING, or
    // matching it proves nothing about either line.
    EXPECT_FALSE(std::regex_match(std::string("[plugin] coco: [3/6] pass"),
                                  diagnosticLineGrammar()))
        << "a progress line must not read as a diagnostic";
    EXPECT_FALSE(std::regex_match(std::string("just some text"),
                                  diagnosticLineGrammar()));
}

// 5.1.3 — no location means NO location prefix, still attributed. A fabricated
// 0:0 would make the IDE navigate somewhere wrong.
TEST(PluginRuntimeTests, textModeRendersAnUnlocatedFindingWithNoLocation) {
    auto r = runPlugin("textbare", kEveryKindScript);
    ASSERT_TRUE(r.ok) << r.error;

    const std::string line = "acme.textbare: info: no position";
    EXPECT_NE(r.stderrText.find(line + "\n"), std::string::npos) << r.stderrText;
    EXPECT_EQ(r.stderrText.find("acme.textbare: :"), std::string::npos)
        << "an empty location slot was rendered";
    EXPECT_EQ(r.stderrText.find(":0:0:"), std::string::npos)
        << "a position was fabricated";

    std::smatch m;
    ASSERT_TRUE(std::regex_match(line, m, diagnosticLineGrammar()));
    EXPECT_EQ(m[1].str(), "acme.textbare") << "still attributed";
    EXPECT_TRUE(m[2].str().empty()) << "no file";
}

// 5.1.4 — two plugins in one task are told apart by the reader, because every
// line says who said it. Attribution that only works for one plugin is not
// attribution.
TEST(PluginRuntimeTests, twoPluginsFindingsAreEachAttributable) {
    const char* script = R"(
printf '{"kind":"finding","severity":"warning","file":"a.cajeta","line":1,"column":1,"message":"mine"}\n'
printf '{"kind":"result","status":"ok"}\n'
)";
    auto first  = runPlugin("alpha", script);
    auto second = runPlugin("beta", script);
    ASSERT_TRUE(first.ok) << first.error;
    ASSERT_TRUE(second.ok) << second.error;

    EXPECT_NE(first.stderrText.find("acme.alpha: a.cajeta:1:1: warning: mine"),
              std::string::npos) << first.stderrText;
    EXPECT_EQ(first.stderrText.find("acme.beta"), std::string::npos);

    EXPECT_NE(second.stderrText.find("acme.beta: a.cajeta:1:1: warning: mine"),
              std::string::npos) << second.stderrText;
    EXPECT_EQ(second.stderrText.find("acme.alpha"), std::string::npos);
}

// 5.1.5 / 5.3.3 — the progress lines are byte-identical to before §5. This is
// the half of text mode that must NOT move: the IDE's stream classifier keys
// on the `[plugin] ` prefix, and a plugin's progress output is read by people
// who have been reading it for months.
TEST(PluginRuntimeTests, progressLinesAreByteIdenticalAfterFindingsWereAdded) {
    auto r = runPlugin("progressbytes", R"(
printf '{"kind":"log","level":"info","message":"coco: [1/6] reference pass"}\n'
printf '{"kind":"log","level":"info","message":"coco: [3/6] instrumenting"}\n'
printf '{"kind":"finding","severity":"warning","file":"a.cajeta","line":2,"column":1,"message":"boom"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    ASSERT_TRUE(r.ok) << r.error;

    // The recorded baseline, unchanged: findings went to the OTHER stream, so
    // they cannot have perturbed these bytes.
    EXPECT_EQ(r.stdoutText,
              "[plugin] coco: [1/6] reference pass\n"
              "[plugin] coco: [3/6] instrumenting\n");
    EXPECT_EQ(r.stderrText, "acme.progressbytes: a.cajeta:2:1: warning: boom\n");
}

// A finding's message is plugin-controlled text on one console line. A raw
// newline in it would split the rendering, leaving an unattributed second line
// that reads as its own diagnostic — reached through a WELL-FORMED record, so
// no amount of validation catches it.
TEST(PluginRuntimeTests, aFindingMessageCannotSplitItsOwnRenderedLine) {
    auto r = runPlugin("splitline", R"(
printf '{"kind":"finding","severity":"warning","file":"a.cajeta","line":1,"column":1,"message":"first\\nacme.other: b.cajeta:9:9: error: forged"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    ASSERT_TRUE(r.ok) << r.error;

    EXPECT_EQ(countLinesStartingWith(r.stderrText, "acme.splitline: "), 1)
        << r.stderrText;
    // The forged text survives, inert, on the one real line.
    EXPECT_NE(r.stderrText.find("first acme.other:"), std::string::npos)
        << r.stderrText;
    // And exactly one line total: nothing escaped onto its own.
    EXPECT_EQ(std::count(r.stderrText.begin(), r.stderrText.end(), '\n'), 1)
        << r.stderrText;
}

// UTF-8 in a message is legitimate and must survive intact. Escaping it the
// way an untrusted LINE is escaped would render `café` as `caf\xc3\xa9`, which
// is a worse reading of a perfectly good message.
TEST(PluginRuntimeTests, aFindingMessageKeepsItsUtf8) {
    auto r = runPlugin("utf8msg", R"(
printf '{"kind":"finding","severity":"warning","message":"café — naïve"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.stderrText.find("café — naïve"), std::string::npos)
        << r.stderrText;
}

// ---- §6 / spec §7a — an `error` finding fails the task ---------------------
//
// Severity stops being decorative. A plugin that says `error` means the build
// is wrong, and coco's coverage floor becomes one instance of the general rule
// rather than a mechanism of its own.

// 6.1.1 / 6.1.4 / 6.3.3 — it fails, and the failure names the plugin by the
// Olla key that declared it. A failing build that does not say which plugin
// failed it sends the reader to the wrong repository.
TEST(PluginRuntimeTests, anErrorFindingFailsTheTaskAndNamesThePlugin) {
    auto r = runPlugin("gate", R"(
printf '{"kind":"finding","severity":"error","rule":"min","file":"src/A.cajeta","line":12,"column":5,"message":"coverage 73.5%% < min 80%%"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_FALSE(r.ok) << "an error finding must fail the task";
    EXPECT_NE(r.error.find("acme.gate"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("1 error finding"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("coverage 73.5% < min 80%"), std::string::npos)
        << "the failure should carry the finding: " << r.error;
}

// The case the rule exists for, stated on its own: the action finished
// CLEANLY — `result: ok` — and still fails, because a finding said error.
// coco's migrated gate is exactly this shape.
TEST(PluginRuntimeTests, anOkResultDoesNotRescueAnErrorFinding) {
    auto r = runPlugin("okbuterror", R"(
printf '{"kind":"finding","severity":"error","message":"floor breached"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_FALSE(r.ok) << "result:ok must not override an error finding";
}

// 6.1.2 / 6.3.2 — the negative arm. warning and info findings are REPORTED and
// the task succeeds; a rule that failed on any finding would make severity
// decorative in the other direction.
TEST(PluginRuntimeTests, warningAndInfoFindingsDoNotFailTheTask) {
    auto r = runPlugin("warnonly", R"(
printf '{"kind":"finding","severity":"warning","file":"a.cajeta","line":1,"column":1,"message":"a surviving mutant"}\n'
printf '{"kind":"finding","severity":"info","message":"dead code"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.result.findings.size(), 2u) << "still reported";
    EXPECT_NE(r.stderrText.find("warning: a surviving mutant"), std::string::npos)
        << r.stderrText;
    EXPECT_NE(r.stderrText.find("info: dead code"), std::string::npos)
        << r.stderrText;
}

// 6.1.3 / 6.3.1 — failing must not truncate the report that explains the
// failure. Asserted by COUNTING the rendered findings, not by the exit code:
// an implementation that failed on the first error and stopped reading would
// pass an exit-code-only assertion and lose four findings.
TEST(PluginRuntimeTests, failingDoesNotTruncateTheFindingReport) {
    auto r = runPlugin("manyfindings", R"(
printf '{"kind":"finding","severity":"warning","file":"a.cajeta","line":1,"column":1,"message":"one"}\n'
printf '{"kind":"finding","severity":"error","file":"b.cajeta","line":2,"column":1,"message":"two"}\n'
printf '{"kind":"finding","severity":"info","file":"c.cajeta","line":3,"column":1,"message":"three"}\n'
printf '{"kind":"finding","severity":"error","file":"d.cajeta","line":4,"column":1,"message":"four"}\n'
printf '{"kind":"finding","severity":"warning","file":"e.cajeta","line":5,"column":1,"message":"five"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(countLinesStartingWith(r.stderrText, "acme.manyfindings: "), 5)
        << "every finding must still be reported: " << r.stderrText;
    EXPECT_NE(r.error.find("2 error findings"), std::string::npos) << r.error;
    // The ones AFTER the first error are the ones a truncating implementation
    // would lose.
    EXPECT_NE(r.stderrText.find("info: three"), std::string::npos);
    EXPECT_NE(r.stderrText.find("warning: five"), std::string::npos);
}

// 6.1.5 — under json the failure is visible in the STREAM, not only in the
// exit code. A CI consumer reading the diagnostic stream must be able to see
// why the build failed without also capturing the process status.
TEST(PluginRuntimeTests, jsonModeMakesTheFailingFindingVisibleInTheStream) {
    JsonModeForTest json;
    auto r = runPlugin("jsongate", R"(
printf '{"kind":"finding","severity":"error","rule":"min","file":"src/A.cajeta","line":12,"column":5,"message":"coverage 73.5%% < min 80%%"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_FALSE(r.ok);

    const auto recs = records(r.stderrText);
    const llvm::json::Object* failing = nullptr;
    for (const auto& o : recs) {
        if (field(o, "kind") == "diagnostic" && field(o, "severity") == "error") {
            failing = &o;
        }
    }
    ASSERT_NE(failing, nullptr)
        << "the failure is invisible to a stream consumer: " << r.stderrText;
    EXPECT_EQ(field(*failing, "message"), "coverage 73.5% < min 80%");
    EXPECT_EQ(field(*failing, "source"), "acme.jsongate")
        << "and attributed, so a failing build says who failed it";
}

// The control for the whole section: a plugin with NO findings at all still
// succeeds. Without this, "an error finding fails the task" could hold because
// something else was failing every task.
TEST(PluginRuntimeTests, noFindingsMeansNoFindingFailure) {
    auto r = runPlugin("nofindings", R"(
printf '{"kind":"log","level":"info","message":"nothing to report"}\n'
printf '{"kind":"result","status":"ok"}\n'
)");
    EXPECT_TRUE(r.ok) << r.error;
}
