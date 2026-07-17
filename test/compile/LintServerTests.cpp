// lint-server plan Unit 2 (specs/lint-server-spec.md §2): the server loop
// and protocol. `cajeta --lint-server` reads NDJSON requests on stdin and
// writes NDJSON responses on stdout: a ready record (with proto version)
// after the prime, then per request the one-shot lint's diagnostic/xref
// lines VERBATIM bracketed by a done marker. stderr stays uncontrolled.
//
// These drive the built binary end-to-end with requests pre-written to a
// file (stdin EOF after the last line doubles as the 2.1.5 clean-exit
// path) and assert on the captured stdout stream.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define CAJETA_LSRV_DEVNULL "NUL"
#else
#  include <sys/wait.h>
#  define CAJETA_LSRV_DEVNULL "/dev/null"
#endif

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) {
        r = envRoot;
    } else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_lintsrv_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

fs::path writeUnit(const fs::path& root, const std::string& name,
                   const std::string& classBody) {
    auto dir = root / "demo";
    fs::create_directories(dir);
    auto file = dir / (name + ".cajeta");
    std::ofstream out(file);
    out << "package demo;\n" << classBody << "\n";
    out.close();
    return file;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Run `cajeta --lint-server <flags>` feeding `requests` on stdin; capture
// stdout and stderr. Returns the exit code (-1 = binary unavailable).
struct ServerRun {
    int rc = -1;
    std::string out;
    std::string err;
};

ServerRun runServer(const std::string& flags, const std::string& requests) {
    ServerRun r;
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return r;
    auto dir = freshTempDir("io");
    auto reqFile = dir / "requests.ndjson";
    { std::ofstream q(reqFile); q << requests; }
    auto outFile = dir / "stdout.ndjson";
    auto errFile = dir / "stderr.txt";
    std::string cmd = bin + " --lint-server " + flags
                    + " < " + reqFile.string()
                    + " > " + outFile.string()
                    + " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
#ifdef _WIN32
    r.rc = rc;
#else
    r.rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : -2;
#endif
    r.out = readFile(outFile);
    r.err = readFile(errFile);
    return r;
}

// One-shot oracle stderr for the same input.
std::string oneShotStderr(const fs::path& file, const std::string& flags) {
    auto bin = compilerBinary();
    auto errFile = freshTempDir("oracle") / "stderr.txt";
    std::string cmd = bin + " --lint " + file.string() + " " + flags
                    + " > " CAJETA_LSRV_DEVNULL " 2> " + errFile.string();
    std::system(cmd.c_str());
    return readFile(errFile);
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l); ) out.push_back(l);
    return out;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The payload slice for request `id`: the raw bytes between the previous
// marker line (ready, or the previous done/error) and this id's done
// marker. Returns false if the done marker is missing.
bool payloadSlice(const std::string& out, int id, std::string& slice) {
    const std::string done = "{\"kind\":\"done\",\"id\":" + std::to_string(id) + "}";
    auto ls = lines(out);
    size_t end = ls.size();
    for (size_t i = 0; i < ls.size(); ++i)
        if (ls[i] == done) { end = i; break; }
    if (end == ls.size()) return false;
    // Walk back to the previous marker (server/done/error record).
    size_t start = 0;
    for (size_t i = end; i-- > 0; ) {
        if (ls[i].rfind("{\"kind\":\"server\"", 0) == 0
                || ls[i].rfind("{\"kind\":\"done\"", 0) == 0
                || ls[i].rfind("{\"kind\":\"error\"", 0) == 0) {
            start = i + 1;
            break;
        }
    }
    slice.clear();
    for (size_t i = start; i < end; ++i) slice += ls[i] + "\n";
    return true;
}

std::string lintRequest(int id, const fs::path& file, bool emitXref = false,
                        const std::string& shadow = "") {
    std::string r = "{\"kind\":\"lint\",\"id\":" + std::to_string(id)
                  + ",\"file\":\"" + file.string() + "\"";
    if (!shadow.empty()) r += ",\"shadow\":\"" + shadow + "\"";
    if (emitXref) r += ",\"emitXref\":true";
    r += "}\n";
    return r;
}

const char* HEALTHY =
    "public final class Alpha {\n"
    "    public static void main() { int32 x = 1; }\n"
    "}";

const char* SEMANTIC_ERROR =
    "public final class Bad {\n"
    "    public static void main() { NoSuchType z = NoSuchType.create(); }\n"
    "}";

#define SKIP_WITHOUT_BINARY() \
    if (!fs::exists(compilerBinary())) \
        GTEST_SKIP() << "compiler binary unavailable"

} // namespace

// 2.1.1 — the spawned server emits the ready record (with proto version)
// after the prime, then answers a lint request with payload + done marker.
TEST(LintServer, ReadyRecordThenLintResponse) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("ready") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);

    auto r = runServer("--diag-format=json", lintRequest(1, file));
    ASSERT_NE(r.rc, -1);
    EXPECT_EQ(r.rc, 0) << "stderr:\n" << r.err;

    auto ls = lines(r.out);
    ASSERT_FALSE(ls.empty()) << "no stdout at all; stderr:\n" << r.err;
    EXPECT_EQ(ls[0],
        "{\"kind\":\"server\",\"proto\":{\"major\":1,\"minor\":0},\"state\":\"ready\"}")
        << "first record must be the ready record";

    std::string slice;
    ASSERT_TRUE(payloadSlice(r.out, 1, slice)) << "no done marker for id 1:\n" << r.out;
    EXPECT_EQ(slice.find("\"severity\""), std::string::npos)
        << "clean file produced diagnostics:\n" << slice;
}

// 2.1.2 — the payload slice between markers is byte-identical to the
// one-shot `--lint` stderr for the same input.
TEST(LintServer, PayloadSliceMatchesOneShotStderr) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("parity") / "src";
    auto file = writeUnit(root, "Bad", SEMANTIC_ERROR);

    std::string oracle = oneShotStderr(file, "--diag-format=json");
    auto r = runServer("--diag-format=json", lintRequest(7, file));
    ASSERT_EQ(r.rc, 0) << r.err;

    std::string slice;
    ASSERT_TRUE(payloadSlice(r.out, 7, slice)) << r.out;
    EXPECT_EQ(slice, oracle)
        << "server payload diverges from one-shot stderr";
    EXPECT_NE(slice.find("NoSuchType"), std::string::npos);
}

// 2.1.3 — two sequential requests: responses contiguous, correctly
// bracketed, ids echoed in arrival order.
TEST(LintServer, TwoSequentialRequestsBracketAndEcho) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("seq") / "src";
    auto clean = writeUnit(root, "Alpha", HEALTHY);
    auto bad = writeUnit(root, "Bad", SEMANTIC_ERROR);

    auto r = runServer("--diag-format=json",
                       lintRequest(1, clean) + lintRequest(2, bad));
    ASSERT_EQ(r.rc, 0) << r.err;

    auto ls = lines(r.out);
    // Expected shape: ready, [payload 1...], done 1, [payload 2...], done 2.
    std::vector<size_t> markers;
    for (size_t i = 0; i < ls.size(); ++i)
        if (ls[i].rfind("{\"kind\":\"done\"", 0) == 0) markers.push_back(i);
    ASSERT_EQ(markers.size(), 2u) << r.out;
    EXPECT_EQ(ls[markers[0]], "{\"kind\":\"done\",\"id\":1}");
    EXPECT_EQ(ls[markers[1]], "{\"kind\":\"done\",\"id\":2}");

    std::string slice1, slice2;
    ASSERT_TRUE(payloadSlice(r.out, 1, slice1));
    ASSERT_TRUE(payloadSlice(r.out, 2, slice2));
    EXPECT_EQ(slice1.find("\"severity\""), std::string::npos)
        << "clean request 1 got diagnostics:\n" << slice1;
    EXPECT_NE(slice2.find("NoSuchType"), std::string::npos)
        << "request 2's diagnostics missing:\n" << slice2;
}

// 2.1.4 — a malformed request line yields an error record and the server
// keeps serving; it never crashes on input.
TEST(LintServer, MalformedLineYieldsErrorAndKeepsServing) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("malformed") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);

    std::string requests =
        "this is not json\n"
        "{\"kind\":\"lint\",\"id\":3}\n"              // lint without a file
        "{\"kind\":\"frobnicate\",\"id\":4}\n"        // unknown kind
        + lintRequest(5, file);
    auto r = runServer("--diag-format=json", requests);
    ASSERT_EQ(r.rc, 0) << "server must survive malformed input; stderr:\n" << r.err;

    auto ls = lines(r.out);
    int errorRecords = 0;
    for (auto& l : ls)
        if (l.rfind("{\"kind\":\"error\"", 0) == 0) ++errorRecords;
    EXPECT_EQ(errorRecords, 3) << r.out;
    // The id is echoed on the error when the line carried one.
    EXPECT_TRUE(has(r.out, "\"id\":3")) << r.out;
    EXPECT_TRUE(has(r.out, "\"id\":4")) << r.out;
    // And the well-formed request after the garbage is still served.
    std::string slice;
    EXPECT_TRUE(payloadSlice(r.out, 5, slice))
        << "server stopped serving after malformed input:\n" << r.out;
}

// 2.1.5 — stdin EOF exits 0 (every test above already ends by EOF); the
// shutdown record likewise, and nothing after it is processed.
TEST(LintServer, ShutdownRecordExitsCleanlyAndStopsServing) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("shutdown") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);

    std::string requests = lintRequest(1, file)
                         + "{\"kind\":\"shutdown\"}\n"
                         + lintRequest(2, file);   // must never be answered
    auto r = runServer("--diag-format=json", requests);
    ASSERT_EQ(r.rc, 0) << r.err;

    std::string slice;
    EXPECT_TRUE(payloadSlice(r.out, 1, slice));
    EXPECT_FALSE(has(r.out, "{\"kind\":\"done\",\"id\":2}"))
        << "a request after shutdown was processed:\n" << r.out;
}

// 2.1.6 — `emitXref` toggles the xref stream per request.
TEST(LintServer, EmitXrefTogglesPerRequest) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("xreftoggle") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);

    auto r = runServer("--diag-format=json --source-root " + root.string(),
                       lintRequest(1, file, /*emitXref=*/true)
                     + lintRequest(2, file, /*emitXref=*/false)
                     + lintRequest(3, file, /*emitXref=*/true));
    ASSERT_EQ(r.rc, 0) << r.err;

    std::string s1, s2, s3;
    ASSERT_TRUE(payloadSlice(r.out, 1, s1));
    ASSERT_TRUE(payloadSlice(r.out, 2, s2));
    ASSERT_TRUE(payloadSlice(r.out, 3, s3));
    EXPECT_TRUE(has(s1, "\"kind\":\"xref\"")) << "request 1 asked for xref:\n" << s1;
    EXPECT_FALSE(has(s2, "\"kind\":\"xref\""))
        << "request 2 did not ask for xref:\n" << s2;
    EXPECT_TRUE(has(s3, "\"kind\":\"xref\"")) << "request 3 asked for xref:\n" << s3;
}

// 2.1.2 (xref form) — with emitXref, the slice matches the one-shot's
// stderr for `--lint <file> --emit-xref` byte-for-byte.
TEST(LintServer, XrefPayloadSliceMatchesOneShot) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("xrefparity") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);

    std::string oracle = oneShotStderr(
        file, "--diag-format=json --emit-xref --source-root " + root.string());
    auto r = runServer("--diag-format=json --source-root " + root.string(),
                       lintRequest(9, file, /*emitXref=*/true));
    ASSERT_EQ(r.rc, 0) << r.err;

    std::string slice;
    ASSERT_TRUE(payloadSlice(r.out, 9, slice)) << r.out;
    EXPECT_EQ(slice, oracle) << "xref payload diverges from one-shot stderr";
}

// 2.2.1 — nonsensical flag combos are refused before the server starts:
// --emit-xref belongs to requests (per-request toggle), not the CLI; a
// positional arg has no meaning in server mode.
TEST(LintServer, RefusesNonsensicalFlagCombos) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("refuse") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);

    auto withXrefPath = runServer("--diag-format=json --emit-xref=/tmp/x.ndjson", "");
    EXPECT_NE(withXrefPath.rc, 0);
    EXPECT_FALSE(has(withXrefPath.out, "\"state\":\"ready\""))
        << "server started despite --emit-xref=<path>";

    auto withBareXref = runServer("--diag-format=json --emit-xref", "");
    EXPECT_NE(withBareXref.rc, 0);
    EXPECT_FALSE(has(withBareXref.out, "\"state\":\"ready\""))
        << "server started despite bare --emit-xref";

    auto withPositional = runServer(
        "--diag-format=json " + file.string(), "");
    EXPECT_NE(withPositional.rc, 0);
    EXPECT_FALSE(has(withPositional.out, "\"state\":\"ready\""))
        << "server started despite a positional argument";
}

// A request naming a missing file gets an error record (the one-shot
// refuses before linting too) and the server keeps serving.
TEST(LintServer, MissingFileYieldsErrorRecordAndKeepsServing) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("missing") / "src";
    auto file = writeUnit(root, "Alpha", HEALTHY);
    auto ghost = root / "demo" / "DoesNotExist.cajeta";

    auto r = runServer("--diag-format=json",
                       lintRequest(1, ghost) + lintRequest(2, file));
    ASSERT_EQ(r.rc, 0) << r.err;

    EXPECT_TRUE(has(r.out, "{\"kind\":\"error\",\"id\":1")) << r.out;
    std::string slice;
    EXPECT_TRUE(payloadSlice(r.out, 2, slice))
        << "server stopped after a missing-file request:\n" << r.out;
}
