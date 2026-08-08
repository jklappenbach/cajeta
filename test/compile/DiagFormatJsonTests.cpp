// --diag-format=json (docs/CompilerModes.md § --diag-format): the compiler
// emits one machine-readable NDJSON diagnostic per line on stderr instead of
// free text, so the IntelliJ plugin / build tool can consume structured errors
// (id, message, file, line, column) rather than regex-scraping. These drive the
// built compiler binary end-to-end and assert the shape of what it emits.

#include <gtest/gtest.h>

#include <algorithm>   // std::replace, used ONLY in the _WIN32 branch below.
                       // Non-Windows builds preprocess that branch away, so they
                       // can never catch a missing include for it — only the
                       // release matrix's mingw target does.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_DIAG_DEVNULL "NUL"
#else
#  define CAJETA_DIAG_DEVNULL "/dev/null"
#endif

// Resolve the in-tree compiler binary (mirrors ReproducibleIrTests' helper).
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
              / ("cajeta_diagjson_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// Write <root>/src/cajeta/Test.cajeta with the given method body, return srcRoot.
fs::path writeTest(const fs::path& root, const std::string& body) {
    auto dir = root / "src" / "cajeta";
    fs::create_directories(dir);
    std::ofstream out(dir / "Test.cajeta");
    out << "package cajeta;\n"
           "public final class Test {\n"
           "    public static void main() {\n"
        << "        " << body << "\n"
           "    }\n"
           "}\n";
    out.close();
    return root / "src";
}

// Compile Test under `srcRoot` with the given extra flags; capture stderr into
// `err` and return the process exit code (-1 if the binary is unavailable).
int compileCapturingStderr(const fs::path& srcRoot, const std::string& flags,
                           std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto build = freshTempDir("out");
    auto errFile = build / "stderr.txt";
    std::string cmd = bin + " --emit=ir " + flags + " cajeta.Test.main "
                    + srcRoot.string() + " " + build.string()
                    + " > " CAJETA_DIAG_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

bool hasLineStartingWith(const std::string& text, const std::string& prefix) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// True if every non-empty line of `text` begins with '{' (a clean NDJSON stream
// with no leaked free-text diagnostics).
bool everyNonEmptyLineIsJson(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    bool sawOne = false;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        sawOne = true;
        if (line[0] != '{') return false;
    }
    return sawOne;
}

// True when `phrase` never appears as RAW console text — i.e. it occurs only
// on lines that are structured records. A record legitimately quotes the
// underlying tool's wording inside a field (ANTLR's message becomes the
// diagnostic's `message`), so a whole-stream substring search cannot tell a
// leak from the structured report of the same error; the line's SHAPE can.
bool phraseOnlyInsideRecords(const std::string& text, const std::string& phrase) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '{') continue;   // structured record
        if (line.find(phrase) != std::string::npos) return false;
    }
    return true;
}

// The first non-empty line, or "" when there is none.
std::string firstLine(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        if (!line.empty()) return line;
    return "";
}

// Every non-empty line that looks like a record carries a "kind" field
// (compiler-jsonl 2.1.1). Returns the first offender, or "" when all comply.
std::string lineWithoutKind(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] != '{') continue;
        if (line.find("\"kind\"") == std::string::npos) return line;
    }
    return "";
}

// The `kind` of the last record on the stream, or "" when there is none.
std::string lastRecordKind(const std::string& text) {
    std::istringstream ss(text);
    std::string line, last;
    while (std::getline(ss, line))
        if (!line.empty() && line[0] == '{') last = line;
    const std::string key = "\"kind\":\"";
    auto at = last.find(key);
    if (at == std::string::npos) return "";
    at += key.size();
    auto end = last.find('"', at);
    return end == std::string::npos ? "" : last.substr(at, end - at);
}

// Run `cajeta jit-run` on a one-file project, capturing stderr. Separate from
// compileCapturingStderr because jit-run takes <root> <entry>, not the
// three-positional compile form.
int jitRunCapturingStderr(const fs::path& srcRoot, const std::string& flags,
                          std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = freshTempDir("jitout") / "stderr.txt";
    std::string cmd = bin + " jit-run " + srcRoot.string()
                    + " cajeta.Test.main " + flags
                    + " > " CAJETA_DIAG_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

// Lint one file, capturing stderr.
int lintCapturingStderrHere(const fs::path& file, const std::string& flags,
                            std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = freshTempDir("lintout") / "stderr.txt";
    std::string cmd = bin + " --lint " + file.string() + " " + flags
                    + " > " CAJETA_DIAG_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

} // namespace

// --- compiler-jsonl Unit 1: the envelope ---------------------------------
// Every record self-describes, and the stream announces its schema before
// saying anything else. Until now `progress`/`cache`/`xref` carried `kind`
// and diagnostics did not — consumers recognised a diagnostic by its HAVING
// a `severity` field, so the IDE console sniffed lines instead of dispatching.

// 1.1.1 — the stream opens with its version record, before any other record.
TEST(DiagFormatJson, StreamRecordComesFirstAndCarriesVersion) {
    auto root = freshTempDir("streamrec");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    const std::string first = firstLine(err);
    EXPECT_NE(first.find("\"kind\":\"stream\""), std::string::npos)
        << "the stream must announce itself before any other record; got:\n"
        << first << "\nfull stderr:\n" << err;
    EXPECT_NE(first.find("\"major\""), std::string::npos) << first;
    EXPECT_NE(first.find("\"minor\""), std::string::npos) << first;
    EXPECT_NE(first.find("\"producer\""), std::string::npos) << first;
}

// 1.1.2 — even a run with nothing to report announces itself, so "clean" is
// distinguishable from "the process died before saying anything" (spec 2.2.4).
TEST(DiagFormatJson, CleanCompileStillEmitsTheStreamRecord) {
    auto root = freshTempDir("streamclean");
    auto srcRoot = writeTest(root, "");   // valid
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "stderr:\n" << err;
    EXPECT_NE(err.find("\"kind\":\"stream\""), std::string::npos)
        << "a clean run said nothing at all; stderr:\n" << err;
}

// 1.1.3 — diagnostics gain `kind` and keep EVERY field they carry today. The
// additive rule (spec 1.4.2): the plugin ships separately from the compiler.
TEST(DiagFormatJson, DiagnosticCarriesKindAndKeepsEveryExistingField) {
    auto root = freshTempDir("diagkind");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(err.find("\"kind\":\"diagnostic\""), std::string::npos)
        << "stderr:\n" << err;
    for (const char* field : {"\"severity\":", "\"code\":", "\"message\":",
                              "\"file\":", "\"line\":", "\"column\":"}) {
        EXPECT_NE(err.find(field), std::string::npos)
            << "diagnostic lost the field " << field << "; stderr:\n" << err;
    }
    EXPECT_NE(err.find("CAJETA_ERROR_UNRESOLVED_TYPE"), std::string::npos);
}

// 1.3.1 / spec 8.1 — no record escapes without a kind, on a real compile that
// produces progress AND diagnostics.
TEST(DiagFormatJson, EveryEmittedRecordCarriesAKind) {
    auto root = freshTempDir("allkind");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(lineWithoutKind(err), "")
        << "a record without `kind` forces consumers to sniff; stderr:\n" << err;
}

// 1.1.4 — text mode gains nothing. The durable half of the byte-identity
// check: no record of any kind leaks into the default output (spec 1.4.1).
TEST(DiagFormatJson, TextModeEmitsNoRecordsAtAll) {
    auto root = freshTempDir("txtnorec");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "", err);   // default = text
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("\"kind\":"), std::string::npos)
        << "text mode leaked a structured record:\n" << err;
    EXPECT_TRUE(hasLineStartingWith(err, "cajeta:")) << "stderr:\n" << err;
}

// --- compiler-jsonl Unit 3: narration + terminal result -------------------
// Output that had no structured form reached the IDE console as opaque text a
// structured console cannot filter, tint, or navigate from. Julian, 2026-07-31:
// a run "never hit a breakpoint, and didn't show compilation or any other
// reason". These give the reason a level and a kind.

// 3.1.1 — an unreadable classpath archive reports as a levelled record under
// the flag. The same failure in text mode keeps its exact wording.
TEST(DiagFormatJson, ClasspathReadFailureIsALevelledLogRecord) {
    auto root = freshTempDir("cplog");
    auto srcRoot = writeTest(root, "");
    // A file that is not an archive at all: ingestClasspath must report, not
    // silently continue with a dependency it never read.
    auto bogus = root / "not-an-archive.cja";
    { std::ofstream o(bogus); o << "this is not a cja archive\n"; }

    std::string jsonErr, textErr;
    int jrc = compileCapturingStderr(
        srcRoot, "--diag-format=json --classpath=" + bogus.string(), jsonErr);
    if (jrc == -1) GTEST_SKIP() << "compiler binary unavailable";
    compileCapturingStderr(srcRoot, "--classpath=" + bogus.string(), textErr);

    EXPECT_NE(jsonErr.find("\"kind\":\"log\""), std::string::npos)
        << "the failure had no structured form; stderr:\n" << jsonErr;
    EXPECT_NE(jsonErr.find("\"level\""), std::string::npos)
        << "a log record must carry a level so the console can filter it:\n"
        << jsonErr;
    EXPECT_TRUE(everyNonEmptyLineIsJson(jsonErr))
        << "free text leaked into the json stream:\n" << jsonErr;
    // Text mode keeps the wording it has always had.
    EXPECT_NE(textErr.find("--classpath read failed"), std::string::npos)
        << "text mode wording changed:\n" << textErr;
    EXPECT_EQ(textErr.find("\"kind\":"), std::string::npos)
        << "text mode leaked a record:\n" << textErr;
}

// 3.1.4 — one terminal `result`, last, so "did it work" is answerable from the
// stream alone rather than inferred from an exit code plus silence (spec 9.4).
TEST(DiagFormatJson, SuccessfulCompileEndsWithAnOkResultRecord) {
    auto root = freshTempDir("resok");
    auto srcRoot = writeTest(root, "");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "stderr:\n" << err;
    EXPECT_NE(err.find("\"kind\":\"result\""), std::string::npos)
        << "no terminal record; stderr:\n" << err;
    EXPECT_NE(err.find("\"status\":\"ok\""), std::string::npos) << err;
    EXPECT_NE(lastRecordKind(err), "") << err;
    EXPECT_EQ(lastRecordKind(err), "result")
        << "the result must come LAST; stderr:\n" << err;
}

TEST(DiagFormatJson, FailingCompileEndsWithAnErrorResultRecord) {
    auto root = freshTempDir("reserr");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_NE(err.find("\"status\":\"error\""), std::string::npos)
        << "a failed compile must say so structurally; stderr:\n" << err;
    EXPECT_EQ(lastRecordKind(err), "result")
        << "the result must come LAST; stderr:\n" << err;
    // Exactly one — a second would read as a second run ending.
    size_t n = 0, at = 0;
    while ((at = err.find("\"kind\":\"result\"", at)) != std::string::npos) { ++n; ++at; }
    EXPECT_EQ(n, 1u) << "expected exactly one terminal record; stderr:\n" << err;
}

// --- compiler-jsonl Unit 4: the flag means one thing --------------------
// `--diag-format=json` used to mean something different per verb: a compile
// got the full stream, `jit-run` accepted the flag but only exported
// CAJETA_DIAG_FORMAT for the runtime's uncaught-throw emitter, and `--lint`
// got diagnostics with no envelope around them. A tool author had to learn
// the quirks. These pin that they are gone (spec 5.1.2 / 5.2.2).

// 4.1.1 — jit-run gets the envelope it previously had no part of.
TEST(DiagFormatJson, JitRunHonoursTheFlagAndEmitsTheEnvelope) {
    auto root = freshTempDir("jitenv");
    auto srcRoot = writeTest(root, "");
    std::string err;
    int rc = jitRunCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(err.find("\"kind\":\"stream\""), std::string::npos)
        << "jit-run took the flag and announced nothing; stderr:\n" << err;
    EXPECT_EQ(lastRecordKind(err), "result")
        << "jit-run must end with a terminal record; stderr:\n" << err;
    EXPECT_EQ(lineWithoutKind(err), "")
        << "a jit-run record escaped without a kind; stderr:\n" << err;
    // The whole point of honouring the flag: the stream is machine-readable
    // end to end, not records with narration interleaved between them.
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "jit-run leaked free text into the json stream:\n" << err;
    // 3.1.3 — the `[jit-run]`/`[jit]` narration arrives as debug-level log
    // records carrying its text verbatim (spec 9.2), so the console can filter
    // it by level instead of showing it as untyped noise.
    EXPECT_NE(err.find("\"kind\":\"log\""), std::string::npos)
        << "narration lost its structured form; stderr:\n" << err;
    EXPECT_NE(err.find("\"level\":\"debug\""), std::string::npos)
        << "narration is not debug-levelled; stderr:\n" << err;
    EXPECT_NE(err.find("[jit"), std::string::npos)
        << "narration text was not carried verbatim; stderr:\n" << err;
}

// jit-run in TEXT mode keeps its narration exactly as it was.
// A syntax error under jit-run must arrive as a LOCATED diagnostic record,
// not as ANTLR's raw "line L:col ..." console text. Julian, 2026-07-31: a
// debug launch of a file with a stray token showed
// `line 37:22 no viable alternative at input 'asdf...'` twice, with no file
// name, no structured record, and a launch failure reading "request failed".
TEST(DiagFormatJson, JitRunSyntaxErrorIsALocatedDiagnosticRecord) {
    auto root = freshTempDir("jitsyn");
    auto srcRoot = writeTest(root, "int x = ;");
    std::string err;
    int rc = jitRunCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_NE(err.find("\"code\":\"syntax\""), std::string::npos)
        << "syntax error never became a diagnostic record; stderr:\n" << err;
    EXPECT_NE(err.find("\"line\":4"), std::string::npos)
        << "the diagnostic carries no location; stderr:\n" << err;
    EXPECT_NE(err.find("Test.cajeta"), std::string::npos)
        << "the diagnostic names no file; stderr:\n" << err;
    // ANTLR's console listener must be off: its raw text is what the record
    // replaces, and leaving both means the stream is not machine-readable.
    // Scoped to NON-record lines: JsonSyntaxErrorListener passes ANTLR's own
    // message through as the diagnostic's `message` (since 56e7d646), so the
    // phrase appears inside the record BY DESIGN — the old whole-stream
    // search read that as a leak and failed a healthy stream.
    EXPECT_TRUE(phraseOnlyInsideRecords(err, "no viable alternative"))
        << "raw ANTLR console text leaked; stderr:\n" << err;
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "free text in the json stream:\n" << err;
}

// 3.1.2 — a dependency graph that cannot be resolved reports as a levelled
// record too, not as a bare line the console cannot classify.
TEST(DiagFormatJson, JitDependencyResolutionFailureIsALevelledLogRecord) {
    auto root = freshTempDir("jitdep");
    auto dir = root / "src" / "cajeta";
    fs::create_directories(dir);
    {
        // @Inject of an interface no @Component provides: resolveDependencyGraph
        // fails, and the JIT host reports it.
        std::ofstream out(dir / "Test.cajeta");
        out << "package cajeta;\n"
               "public interface Missing { public void go(); }\n"
               "@Component\n"
               "public final class Needy {\n"
               "    @Inject Missing dep;\n"
               "    public Needy() { }\n"
               "}\n"
               "public final class Test {\n"
               "    public static void main() { }\n"
               "}\n";
    }
    std::string err;
    int rc = jitRunCapturingStderr(root / "src", "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    // Whether THIS fixture trips resolution is a language detail; what the
    // envelope guarantees is that whatever the JIT host says, it says
    // structurally. A run that fails must never fall back to bare prose.
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "jit-run leaked free text into the json stream:\n" << err;
    EXPECT_EQ(lastRecordKind(err), "result")
        << "no terminal record; stderr:\n" << err;
}

TEST(DiagFormatJson, JitRunTextModeKeepsItsNarration) {
    auto root = freshTempDir("jittext");
    auto srcRoot = writeTest(root, "");
    std::string err;
    int rc = jitRunCapturingStderr(srcRoot, "", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(err.find("[jit-run] entry"), std::string::npos)
        << "text-mode narration changed; stderr:\n" << err;
    EXPECT_EQ(err.find("\"kind\":"), std::string::npos)
        << "text mode leaked a record; stderr:\n" << err;
}

// 4.1.2 — lint gets the same envelope. Its payload is what the warm server
// replays byte-for-byte, so this is also what keeps one-shot and warm honest
// (lint-server-spec 1.4.1) — the envelope has to be emitted by the shared
// DRIVER, not once per process, or the two diverge.
TEST(DiagFormatJson, LintHonoursTheFlagAndEmitsTheEnvelope) {
    auto root = freshTempDir("lintenv");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    auto file = srcRoot / "cajeta" / "Test.cajeta";
    std::string err;
    int rc = lintCapturingStderrHere(file, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(err.find("\"kind\":\"stream\""), std::string::npos)
        << "lint emitted diagnostics with no envelope; stderr:\n" << err;
    EXPECT_NE(err.find("\"kind\":\"diagnostic\""), std::string::npos) << err;
    EXPECT_EQ(lastRecordKind(err), "result")
        << "lint must end with a terminal record; stderr:\n" << err;
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "free text leaked into the lint stream:\n" << err;
}

TEST(DiagFormatJson, LintTextModeStillEmitsNoRecords) {
    auto root = freshTempDir("linttext");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    auto file = srcRoot / "cajeta" / "Test.cajeta";
    std::string err;
    int rc = lintCapturingStderrHere(file, "", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(err.find("\"kind\":"), std::string::npos)
        << "text-mode lint leaked a record; stderr:\n" << err;
}

TEST(DiagFormatJson, SemanticErrorEmitsNdjson) {
    auto root = freshTempDir("sem");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "a semantic error must fail the compile";
    // Structured: a JSON object naming the error id + message, no plain text.
    // `kind` now leads the object (compiler-jsonl 2.1.1), so this asserts the
    // FIELDS rather than a byte prefix — JSON key order was never contractual,
    // and the plugin's parser reads the DOM, not the leading characters.
    EXPECT_NE(err.find("\"kind\":\"diagnostic\""), std::string::npos)
        << "stderr:\n" << err;
    EXPECT_NE(err.find("\"severity\":\"error\""), std::string::npos)
        << "stderr:\n" << err;
    EXPECT_NE(err.find("CAJETA_ERROR_UNRESOLVED_TYPE"), std::string::npos);
    EXPECT_NE(err.find("NoSuchType"), std::string::npos);
    EXPECT_FALSE(hasLineStartingWith(err, "cajeta:"))
        << "json mode must not emit the plain-text 'cajeta:' line";
}

TEST(DiagFormatJson, SyntaxErrorEmitsNdjsonWithLocationAndNoLeakedText) {
    auto root = freshTempDir("syn");
    auto srcRoot = writeTest(root, "int x = ;");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    // A syntax diagnostic with a concrete 1-based location.
    EXPECT_NE(err.find("\"code\":\"syntax\""), std::string::npos)
        << "stderr:\n" << err;
    EXPECT_NE(err.find("\"line\":4"), std::string::npos) << "stderr:\n" << err;
    // The prescan pass must not leak ANTLR's "line L:col ..." console text —
    // the whole stream stays machine-parseable.
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "leaked non-JSON text in json mode:\n" << err;
}

TEST(DiagFormatJson, SyntaxErrorInExpressionDoesNotCrashCompiler) {
    // Regression: a dangling binary operator makes ANTLR's error-recovery tree
    // malformed; the compiler used to hand it to the semantic visitor and
    // segfault. It must report the syntax diagnostic and fail cleanly instead.
    auto root = freshTempDir("syncrash");
    auto srcRoot = writeTest(root, "return 1 + ;");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("SIGSEGV"), std::string::npos)
        << "compiler crashed instead of reporting a syntax error:\n" << err;
    EXPECT_NE(err.find("\"code\":\"syntax\""), std::string::npos) << err;
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "crash backtrace or free text leaked into the json stream:\n" << err;
}

TEST(DiagFormatJson, TextModeRemainsPlain) {
    auto root = freshTempDir("txt");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "", err);  // default = text
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_TRUE(hasLineStartingWith(err, "cajeta:")) << "stderr:\n" << err;
    EXPECT_EQ(err.find("{\"severity\""), std::string::npos)
        << "text mode must not emit JSON";
}

// collect-continue-compile 1.1 — full compile reports ALL pre-codegen errors
// (not just the first) and writes no artifact when errors exist.
TEST(DiagFormatJson, FullCompileCollectsMultipleErrorsNoArtifact) {
    auto root = freshTempDir("fcmulti");
    auto srcRoot = writeTest(root,
        "NoSuchType1 z = NoSuchType1.create(); NoSuchType2 y = NoSuchType2.create();");
    auto bin = compilerBinary();
    if (!fs::exists(bin)) GTEST_SKIP() << "compiler binary unavailable";
    auto out = freshTempDir("fcout");
    auto errFile = out / "err.txt";
    std::string cmd = bin + " --emit=ir --diag-format=json cajeta.Test.main "
                    + srcRoot.string() + " " + out.string()
                    + " > " CAJETA_DIAG_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    std::string err = ss.str();

    EXPECT_NE(rc, 0);
    size_t n = 0, p = 0;
    while ((p = err.find("\"severity\":\"error\"", p)) != std::string::npos) { n++; p++; }
    EXPECT_EQ(n, 2u) << "both unresolved types must report; stderr:\n" << err;
    EXPECT_TRUE(everyNonEmptyLineIsJson(err)) << "pure NDJSON:\n" << err;

    int artifacts = 0;
    for (const auto& e : fs::recursive_directory_iterator(out))
        if (e.path().extension() == ".ll") artifacts++;
    EXPECT_EQ(artifacts, 0) << "no artifact may be emitted when errors exist";
}

TEST(DiagFormatJson, ValidCompileEmitsNoDiagnostics) {
    auto root = freshTempDir("ok");
    auto srcRoot = writeTest(root, "");  // empty body, valid
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "stderr:\n" << err;
    EXPECT_EQ(err.find("{\"severity\""), std::string::npos)
        << "a clean compile must emit no diagnostics; stderr:\n" << err;
}
