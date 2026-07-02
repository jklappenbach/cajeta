// json-diagnostics U1: the build tool forwards `--diag-format=json` to the
// compiler subprocesses its build actions spawn, and the compiler's NDJSON
// passes through to the build tool's stderr (json-diagnostics-spec §2). Drives
// the built binary end-to-end: `cajeta build --diag-format=json` on a project
// with a broken source must surface machine-readable diagnostics, and the
// default (text) must not.

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
#  define CAJETA_BT_DEVNULL "NUL"
#else
#  define CAJETA_BT_DEVNULL "/dev/null"
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
              / ("cajeta_bt_diagjson_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// Write a minimal single-package project (manifest + one source) whose `build`
// task runs the native `build` action. `body` goes inside Main::main.
fs::path writeProject(const fs::path& root, const std::string& body) {
    std::ofstream manifest(root / "cajeta.json");
    manifest <<
        "{\n"
        "  \"details\": { \"name\": \"com.example.t\", \"version\": \"0.1.0\",\n"
        "                 \"cajeta-lang-version\": \"1.0\" },\n"
        "  \"settings\": { \"build\": { \"target\": \"host\",\n"
        "                 \"entry-method\": \"com.example.t.Main::main\" } },\n"
        "  \"tasks\": { \"build\": { \"actions\": [\n"
        "      { \"action\": \"build\", \"flavor\": \"debug\", \"id\": \"art\" } ] } }\n"
        "}\n";
    manifest.close();

    auto dir = root / "src" / "main" / "cajeta" / "com" / "example" / "t";
    fs::create_directories(dir);
    std::ofstream src(dir / "Main.cajeta");
    src << "package com.example.t;\n"
           "public final class Main {\n"
           "    public static void main() {\n"
        << "        " << body << "\n"
           "    }\n"
           "}\n";
    src.close();
    return root;
}

int runBuildCapturingStderr(const fs::path& proj, const std::string& flags,
                            std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = proj / "stderr.txt";
    std::string cmd = "cd " + proj.string() + " && " + bin +
                      " build " + flags +
                      " > " CAJETA_BT_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

bool hasLineStartingWith(const std::string& text, const std::string& prefix) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
        if (line.rfind(prefix, 0) == 0) return true;
    return false;
}

} // namespace

TEST(BuildToolDiagFormatJson, BuildForwardsJsonAndPassesNdjsonThrough) {
    auto proj = writeProject(freshTempDir("err"),
                             "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = runBuildCapturingStderr(proj, "--diag-format=json", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "a broken build must fail";
    EXPECT_TRUE(hasLineStartingWith(err, "{\"severity\":\"error\""))
        << "the compiler's NDJSON must pass through the build tool; stderr:\n" << err;
    EXPECT_NE(err.find("NoSuchType"), std::string::npos) << "stderr:\n" << err;
}

TEST(BuildToolDiagFormatJson, DefaultTextModeEmitsNoJson) {
    auto proj = writeProject(freshTempDir("txt"),
                             "NoSuchType z = NoSuchType.create();");
    std::string err;
    int rc = runBuildCapturingStderr(proj, "", err);  // default = text
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0);
    EXPECT_EQ(err.find("{\"severity\""), std::string::npos)
        << "text mode must not emit JSON; stderr:\n" << err;
}

TEST(BuildToolDiagFormatJson, BadDiagFormatValueIsRejected) {
    auto proj = writeProject(freshTempDir("bad"), "");
    std::string err;
    int rc = runBuildCapturingStderr(proj, "--diag-format=nonsense", err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";
    EXPECT_NE(rc, 0) << "an invalid --diag-format value must be rejected";
}
