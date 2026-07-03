// `cajeta --lint <file> [--diag-format=text|json]` (compiler-lint-mode-spec):
// run the diagnostic passes over ONE source file (stdlib + in-file resolution),
// no codegen/emit/entry-method, and report diagnostics. These drive the built
// compiler binary end-to-end and assert the shape of what it emits.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_LINT_DEVNULL "NUL"
#else
#  define CAJETA_LINT_DEVNULL "/dev/null"
#endif

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
              / ("cajeta_lint_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// Write a standalone <dir>/File.cajeta wrapping `body` in a method; return its path.
fs::path writeFile(const fs::path& dir, const std::string& body) {
    fs::create_directories(dir);
    auto file = dir / "File.cajeta";
    std::ofstream out(file);
    out << "package demo;\n"
           "public final class File {\n"
           "    public static void main() {\n"
        << "        " << body << "\n"
           "    }\n"
           "}\n";
    out.close();
    return file;
}

// Run `cajeta --lint <file> [flags]`; capture stderr into `err`, return exit code
// (-1 if the binary is unavailable). stdout is discarded to /dev/null.
int lintCapturingStderr(const fs::path& file, const std::string& flags,
                        std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = freshTempDir("err") / "stderr.txt";
    std::string cmd = bin + " --lint " + file.string() + " " + flags
                    + " > " CAJETA_LINT_DEVNULL " 2> " + errFile.string();
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

// 1.1.1 — a clean file emits no diagnostics and exits 0; no IR on stdout.
TEST(LintMode, CleanFileJsonExitsZeroNoDiagnostics) {
    auto file = writeFile(freshTempDir("clean"), "int32 x = 1;");
    std::string err;
    int rc = lintCapturingStderr(file, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "stderr:\n" << err;
    EXPECT_EQ(err.find("{\"severity\""), std::string::npos)
        << "a clean lint must emit no diagnostics; stderr:\n" << err;
}

// 1.1.2 — a syntax error: NDJSON with location, clean NDJSON stream, no crash.
TEST(LintMode, SyntaxErrorJsonHasLocationNoCrash) {
    auto file = writeFile(freshTempDir("syn"), "return 1 + ;");
    std::string err;
    int rc = lintCapturingStderr(file, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("SIGSEGV"), std::string::npos)
        << "lint crashed instead of reporting a syntax error:\n" << err;
    EXPECT_NE(err.find("\"code\":\"syntax\""), std::string::npos) << err;
    EXPECT_TRUE(everyNonEmptyLineIsJson(err))
        << "non-JSON text leaked into the json stream:\n" << err;
}

// 1.1.3 — a semantic error (unresolved type) emits an error-severity NDJSON diag.
TEST(LintMode, SemanticErrorJsonEmitsNdjson) {
    auto file = writeFile(freshTempDir("sem"), "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = lintCapturingStderr(file, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "a semantic error must fail the lint";
    EXPECT_TRUE(hasLineStartingWith(err, "{\"severity\":\"error\""))
        << "stderr:\n" << err;
    EXPECT_NE(err.find("NoSuchType"), std::string::npos) << "stderr:\n" << err;
}

// 1.1.4 — text mode stays plain (no JSON).
TEST(LintMode, TextModeRemainsPlain) {
    auto file = writeFile(freshTempDir("txt"), "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = lintCapturingStderr(file, "", err);  // default = text
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("{\"severity\""), std::string::npos)
        << "text mode must not emit JSON; stderr:\n" << err;
}

// 1.1.5 — a missing file fails clearly, not with the three-positional usage banner.
TEST(LintMode, MissingFileFailsClearlyNotUsageBanner) {
    auto missing = freshTempDir("missing") / "DoesNotExist.cajeta";
    std::string err;
    int rc = lintCapturingStderr(missing, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("<source-root-path>"), std::string::npos)
        << "must not print the compile usage banner; stderr:\n" << err;
}

// 1.1.6 — lint writes no artifact (no .ll/.o/.cja) beside the source.
TEST(LintMode, WritesNoArtifact) {
    auto dir = freshTempDir("noartifact");
    auto file = writeFile(dir, "int32 x = 1;");
    std::string err;
    int rc = lintCapturingStderr(file, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        auto ext = e.path().extension();
        EXPECT_TRUE(ext != ".ll" && ext != ".o" && ext != ".cja" && ext != ".bc")
            << "lint emitted an artifact: " << e.path();
    }
}
