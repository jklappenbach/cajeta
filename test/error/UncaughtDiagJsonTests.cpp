// diagnostic-exceptions Unit 1 (1.1.5 / 1.2.3): under --diag-format=json an
// UNCAUGHT throw emits one NDJSON diagnostic line (severity/code/message/frames)
// instead of the free-text "cajeta: uncaught exception: ..." line. Driven
// end-to-end through the built binary via `cajeta jit-run`, since the uncaught
// path terminates the process and can't be exercised in-process.

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
#  define CAJETA_UNCAUGHT_DEVNULL "NUL"
#else
#  define CAJETA_UNCAUGHT_DEVNULL "/dev/null"
#endif

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_uncaught_diagjson_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// A single-source project whose entry `test.App.run` throws `body` uncaught.
fs::path writeProject(const fs::path& root, const std::string& body) {
    auto dir = root / "test";
    fs::create_directories(dir);
    std::ofstream src(dir / "App.cajeta");
    src << "package test;\n"
           "import cajeta.error.Exception;\n"
           "public final class App {\n"
           "    public static void run() {\n"
        << "        " << body << "\n"
           "    }\n"
           "}\n";
    src.close();
    return root;
}

int runJitCapturingStderr(const fs::path& proj, const std::string& flags,
                          std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = proj / "stderr.txt";
    std::string cmd = bin + " jit-run " + proj.string() + " test.App.run " + flags +
                      " > " CAJETA_UNCAUGHT_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

// True iff some line of `text` starts with `prefix`.
bool hasLineStartingWith(const std::string& text, const std::string& prefix) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        if (line.rfind(prefix, 0) == 0) return true;
    return false;
}

} // namespace

// 1.1.5: uncaught throw under --diag-format=json emits an NDJSON diagnostic line.
TEST(UncaughtDiagJson, JsonModeEmitsNdjsonDiagnostic) {
    auto proj = writeProject(freshTempDir("json"),
                             "throw heap Exception(\"disk full\");");
    std::string err;
    int rc = runJitCapturingStderr(proj, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    EXPECT_TRUE(hasLineStartingWith(err, "{\"severity\":\"error\""))
        << "uncaught throw must emit NDJSON under --diag-format=json; stderr:\n" << err;
    EXPECT_NE(err.find("\"code\":\"cajeta.error.Exception\""), std::string::npos)
        << "stderr:\n" << err;
    EXPECT_NE(err.find("disk full"), std::string::npos) << "stderr:\n" << err;
}

// 1.2.3: default (text) mode keeps the free-text uncaught line, no JSON.
TEST(UncaughtDiagJson, TextModeEmitsNoJson) {
    auto proj = writeProject(freshTempDir("text"),
                             "throw heap Exception(\"disk full\");");
    std::string err;
    int rc = runJitCapturingStderr(proj, "", err);  // default = text
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("{\"severity\":\"error\""), std::string::npos)
        << "text mode must not emit JSON; stderr:\n" << err;
    EXPECT_NE(err.find("disk full"), std::string::npos)
        << "text mode still surfaces the message; stderr:\n" << err;
}
