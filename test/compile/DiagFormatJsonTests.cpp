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

} // namespace

TEST(DiagFormatJson, SemanticErrorEmitsNdjson) {
    auto root = freshTempDir("sem");
    auto srcRoot = writeTest(root, "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = compileCapturingStderr(srcRoot, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "a semantic error must fail the compile";
    // Structured: a JSON object naming the error id + message, no plain text.
    EXPECT_TRUE(hasLineStartingWith(err, "{\"severity\":\"error\""))
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
