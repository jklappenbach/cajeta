// The buildtool command surface of `cajeta` (buildtool/BuildToolCommands.cpp)
// beyond `build`/`run`: init archetypes, manifest introspection (info /
// tasks / task --show), dependency edits (add / remove), coverage-ignore
// bookkeeping, the trust store, toolchain listing, and sandbox-info. The
// build/run spine is covered by the IncrementalBuild/RunCommand suites;
// these arms had no consumer at all.
//
// Every test runs the real built binary in a temp world: HOME and the XDG
// dirs point inside it (trust store, toolchains), and project commands run
// in a `cajeta init`-created project dir.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

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

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

struct ToolWorld {
    fs::path root;
    ToolWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_btool_" + std::to_string(rng()));
        fs::create_directories(root / "home");
    }
    ~ToolWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path outLog() const { return root / "out.log"; }

    // Run `cajeta <args>` with cwd `dir` and the world's fake HOME/XDG.
    int runIn(const fs::path& dir, const std::string& args) {
        std::string cmd = "cd " + dir.string()
            + " && HOME=" + (root / "home").string()
            + " XDG_CONFIG_HOME=" + (root / "home" / ".config").string()
            + " XDG_DATA_HOME=" + (root / "home" / ".local").string()
            + " " + compilerBinary() + " " + args
            + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }
    int run(const std::string& args) { return runIn(root, args); }

    std::string output() const {
        std::ifstream in(outLog());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // `cajeta init basic <dir>` — the fixture project for manifest commands.
    fs::path initProject(const std::string& name = "proj") {
        fs::path dir = root / name;
        fs::create_directories(dir);
        EXPECT_EQ(run("init basic " + dir.string()), 0) << output();
        EXPECT_TRUE(fs::exists(dir / "cajeta.json")) << output();
        return dir;
    }

    std::string manifestOf(const fs::path& dir) const {
        std::ifstream in(dir / "cajeta.json");
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

} // namespace

TEST(BuildToolCommandTests, initListsArchetypes) {
    ToolWorld w;
    EXPECT_EQ(w.run("init --list"), 0);
    EXPECT_NE(w.output().find("basic"), std::string::npos) << w.output();
}

TEST(BuildToolCommandTests, initWritesArchetypeAndRefusesSecondPass) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_NE(w.output().find("Initialized 'basic' archetype"),
              std::string::npos)
        << w.output();

    // Without --force an existing tree is not overwritten.
    EXPECT_NE(w.run("init basic " + dir.string()), 0);
    EXPECT_EQ(w.run("init basic " + dir.string() + " --force"), 0)
        << w.output();
}

TEST(BuildToolCommandTests, infoPrintsManifestDetails) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "info"), 0);
    // The basic archetype's details block round-trips to the console.
    EXPECT_NE(w.output().find("name"), std::string::npos) << w.output();
    EXPECT_NE(w.output().find("version"), std::string::npos) << w.output();
}

TEST(BuildToolCommandTests, tasksListsManifestTasksTextAndJson) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "tasks"), 0);
    EXPECT_NE(w.output().find("build"), std::string::npos) << w.output();

    EXPECT_EQ(w.runIn(dir, "tasks --json"), 0);
    EXPECT_NE(w.output().find("\"build\""), std::string::npos) << w.output();
}

TEST(BuildToolCommandTests, taskShowInspectsOneTask) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "task build --show"), 0);
    EXPECT_NE(w.output().find("build"), std::string::npos) << w.output();
}

TEST(BuildToolCommandTests, addAndRemoveDependencyEditTheManifest) {
    ToolWorld w;
    fs::path dir = w.initProject();

    EXPECT_EQ(w.runIn(dir, "add dev.cajeta.codec@1.2.0"), 0) << w.output();
    std::string m = w.manifestOf(dir);
    EXPECT_NE(m.find("dev.cajeta.codec"), std::string::npos) << m;
    EXPECT_NE(m.find("1.2.0"), std::string::npos) << m;

    // Version omitted → wildcard constraint.
    EXPECT_EQ(w.runIn(dir, "add com.example.util"), 0) << w.output();
    m = w.manifestOf(dir);
    EXPECT_NE(m.find("com.example.util"), std::string::npos) << m;

    EXPECT_EQ(w.runIn(dir, "remove dev.cajeta.codec"), 0) << w.output();
    m = w.manifestOf(dir);
    EXPECT_EQ(m.find("dev.cajeta.codec"), std::string::npos) << m;

    // Missing name is a usage error.
    EXPECT_NE(w.runIn(dir, "add"), 0);
}

TEST(BuildToolCommandTests, coverageIgnoreListRemoveRoundTrip) {
    ToolWorld w;
    fs::path dir = w.initProject();

    EXPECT_EQ(w.runIn(dir,
        "coverage ignore --kind=file --pattern=src/Gen.cajeta "
        "--reason=generated"), 0) << w.output();
    std::string m = w.manifestOf(dir);
    EXPECT_NE(m.find("src/Gen.cajeta"), std::string::npos) << m;

    EXPECT_EQ(w.runIn(dir, "coverage list"), 0);
    EXPECT_NE(w.output().find("src/Gen.cajeta"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.runIn(dir,
        "coverage remove --pattern=src/Gen.cajeta --yes"), 0)
        << w.output();
    m = w.manifestOf(dir);
    EXPECT_EQ(m.find("src/Gen.cajeta"), std::string::npos) << m;

    // The ignore path upgraded the archetype's string-shorthand plugin
    // declaration to object form; the version constraint must survive.
    EXPECT_NE(m.find("cajeta.coverage"), std::string::npos) << m;
    EXPECT_NE(m.find("1.0.*"), std::string::npos) << m;
}

TEST(BuildToolCommandTests, trustStoreAddShowListRemove) {
    ToolWorld w;
    fs::path key = w.root / "key.pem";
    fs::path pub = w.root / "pub.pem";
    if (std::system(("openssl genpkey -algorithm ed25519 -out "
                     + key.string() + " 2>/dev/null").c_str()) != 0) {
        GTEST_SKIP() << "openssl unavailable";
    }
    ASSERT_EQ(std::system(("openssl pkey -in " + key.string()
                           + " -pubout -out " + pub.string()
                           + " 2>/dev/null").c_str()), 0);

    EXPECT_EQ(w.run("trust add relkey " + pub.string()), 0) << w.output();
    EXPECT_NE(w.output().find("added 'relkey'"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("trust show relkey"), 0);
    EXPECT_NE(w.output().find("fingerprint: sha256:"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("trust list"), 0);
    EXPECT_NE(w.output().find("relkey"), std::string::npos) << w.output();

    EXPECT_EQ(w.run("trust remove relkey"), 0) << w.output();
    EXPECT_NE(w.run("trust show relkey"), 0);
    EXPECT_NE(w.output().find("not found"), std::string::npos) << w.output();
}

TEST(BuildToolCommandTests, toolchainListRunsOnFreshHome) {
    ToolWorld w;
    EXPECT_EQ(w.run("toolchain list"), 0) << w.output();
}

TEST(BuildToolCommandTests, sandboxInfoDumpsDiagnostics) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "sandbox-info"), 0) << w.output();
}
