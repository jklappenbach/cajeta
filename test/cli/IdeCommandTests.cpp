// `cajeta ide <sub>` — the IDEA plugin installer surface (cli/IdeCommands.cpp).
//
// Drives the real built binary end-to-end with HOME/XDG_* pointed at temp
// dirs, so product detection, plugin-status listing, uninstall sweeps and the
// --plugins-dir override all run hermetically. The install/extract arms need
// a build with the plugin zip embedded; a plugin-less build refuses install
// with EXIT_NONE, and that refusal is pinned here too (the test adapts to
// either build flavor via `ide list`'s bundled-plugin line).

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

// One temp world per test: HOME, XDG_CONFIG_HOME, XDG_DATA_HOME all live
// under it, so detection sees exactly what the test creates.
struct IdeWorld {
    fs::path root;
    IdeWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_ide_" + std::to_string(rng()));
        fs::create_directories(configBase());
        fs::create_directories(dataBase());
    }
    ~IdeWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path configBase() const { return root / "config"; }
    fs::path dataBase() const { return root / "data"; }
    fs::path outLog() const { return root / "out.log"; }

    // A JetBrains product config dir (what detection scans).
    void addProduct(const std::string& name) const {
        fs::create_directories(configBase() / "JetBrains" / name);
    }
    // The plugins dir `ide` targets for that product on Linux.
    fs::path pluginsDir(const std::string& name) const {
        return dataBase() / "JetBrains" / name;
    }

    int run(const std::string& ideArgs) const {
        std::string cmd = "HOME=" + (root / "home").string()
            + " XDG_CONFIG_HOME=" + configBase().string()
            + " XDG_DATA_HOME=" + dataBase().string()
            + " " + compilerBinary() + " ide " + ideArgs
            + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }
    std::string output() const {
        std::ifstream in(outLog());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

} // namespace

TEST(IdeCommandTests, noSubcommandPrintsUsageAndFails) {
    IdeWorld w;
    EXPECT_EQ(w.run(""), 1);
    EXPECT_NE(w.output().find("Usage: cajeta ide"), std::string::npos)
        << w.output();
}

TEST(IdeCommandTests, unknownSubcommandPrintsUsageAndFails) {
    IdeWorld w;
    EXPECT_EQ(w.run("frobnicate"), 1);
    EXPECT_NE(w.output().find("unknown command 'frobnicate'"),
              std::string::npos) << w.output();
}

TEST(IdeCommandTests, helpFlagSucceeds) {
    IdeWorld w;
    EXPECT_EQ(w.run("--help"), 0);
    EXPECT_NE(w.output().find("Usage: cajeta ide"), std::string::npos)
        << w.output();
}

TEST(IdeCommandTests, listReportsNoDetectionInEmptyWorld) {
    IdeWorld w;
    EXPECT_EQ(w.run("list"), 0);
    EXPECT_NE(w.output().find("No IntelliJ IDEA installation detected"),
              std::string::npos) << w.output();
}

// Detection scans the JetBrains config base: IDEA products (Ultimate and
// Community naming) appear sorted; non-IDEA products are skipped. Status
// comes from the per-product plugins dir.
TEST(IdeCommandTests, listDetectsIdeaProductsAndPluginStatus) {
    IdeWorld w;
    w.addProduct("IntelliJIdea2025.1");
    w.addProduct("IdeaIC2024.3");
    w.addProduct("CLion2025.1");   // not an IDEA product — must not appear

    EXPECT_EQ(w.run("list"), 0);
    std::string out = w.output();
    EXPECT_NE(out.find("IntelliJIdea2025.1  [not installed]"),
              std::string::npos) << out;
    EXPECT_NE(out.find("IdeaIC2024.3  [not installed]"), std::string::npos)
        << out;
    EXPECT_EQ(out.find("CLion"), std::string::npos) << out;

    // Drop a plugin dir where the installer would put it: status flips.
    fs::create_directories(w.pluginsDir("IntelliJIdea2025.1") / "cajeta-idea");
    EXPECT_EQ(w.run("list"), 0);
    out = w.output();
    EXPECT_NE(out.find("IntelliJIdea2025.1  [installed]"), std::string::npos)
        << out;
    EXPECT_NE(out.find("IdeaIC2024.3  [not installed]"), std::string::npos)
        << out;
}

TEST(IdeCommandTests, listWithPluginsDirOverrideChecksThatDirOnly) {
    IdeWorld w;
    fs::path plug = w.root / "explicit-plugins";
    fs::create_directories(plug);

    EXPECT_EQ(w.run("list --plugins-dir=" + plug.string()), 0);
    EXPECT_NE(w.output().find("not installed"), std::string::npos)
        << w.output();

    fs::create_directories(plug / "cajeta-idea");
    EXPECT_EQ(w.run("list --plugins-dir=" + plug.string()), 0);
    std::string out = w.output();
    EXPECT_NE(out.find("installed"), std::string::npos) << out;
    EXPECT_EQ(out.find("not installed"), std::string::npos) << out;
}

// Install behaves per build flavor: with a bundled plugin it extracts into
// the override dir; a plugin-less build refuses with EXIT_NONE (2). `list`'s
// "Bundled plugin:" line says which world we're in.
TEST(IdeCommandTests, installIntoOverrideDirOrBundleRefusal) {
    IdeWorld w;
    ASSERT_EQ(w.run("list"), 0);
    const bool bundled =
        w.output().find("Bundled plugin: none") == std::string::npos;

    fs::path plug = w.root / "explicit-plugins";
    int rc = w.run("install --plugins-dir=" + plug.string());
    if (bundled) {
        EXPECT_EQ(rc, 0) << w.output();
        EXPECT_TRUE(fs::exists(plug / "cajeta-idea")) << w.output();
        // Idempotent: a second install replaces, still clean.
        EXPECT_EQ(w.run("install --plugins-dir=" + plug.string()), 0)
            << w.output();
    } else {
        EXPECT_EQ(rc, 2) << w.output();
        EXPECT_NE(w.output().find("no bundled plugin"), std::string::npos)
            << w.output();
    }
}

TEST(IdeCommandTests, uninstallWithNoDetectionReportsAndFails) {
    IdeWorld w;
    EXPECT_EQ(w.run("uninstall"), 2);
    EXPECT_NE(w.output().find("no IntelliJ IDEA installation detected"),
              std::string::npos) << w.output();
}

TEST(IdeCommandTests, uninstallSweepsDetectedProductsAndOverrideDir) {
    IdeWorld w;
    w.addProduct("IntelliJIdea2025.1");
    fs::path target = w.pluginsDir("IntelliJIdea2025.1") / "cajeta-idea";
    fs::create_directories(target);
    std::ofstream(target / "plugin.xml") << "<idea-plugin/>";

    EXPECT_EQ(w.run("uninstall"), 0);
    EXPECT_NE(w.output().find("Removed from 1 location(s)"), std::string::npos)
        << w.output();
    EXPECT_FALSE(fs::exists(target));

    // Nothing left: the sweep reports so, still exit 0.
    EXPECT_EQ(w.run("uninstall"), 0);
    EXPECT_NE(w.output().find("No bundled Cajeta plugin was installed"),
              std::string::npos) << w.output();

    // Override form removes exactly the named dir.
    fs::path plug = w.root / "explicit-plugins";
    fs::create_directories(plug / "cajeta-idea");
    EXPECT_EQ(w.run("uninstall --plugins-dir=" + plug.string()), 0);
    EXPECT_FALSE(fs::exists(plug / "cajeta-idea")) << w.output();
}
