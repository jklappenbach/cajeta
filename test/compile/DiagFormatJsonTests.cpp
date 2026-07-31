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
