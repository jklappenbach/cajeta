// The `cajeta` buildtool verbs beyond build/run/init: workspace, toolchain,
// install, upgrade, publish, verify-reproducible, and the coverage
// dispatcher's own arms. BuildToolCommands.cpp is the battery's single
// largest coverage gap (1,390 uncovered lines) because these commands had
// no consumer at all.
//
// Everything runs hermetically in a temp world: HOME, XDG_*, OLLA_HOME and
// CAJETA_TOOLCHAIN_HOME all point inside it, so the toolchain store, the
// Olla store and the trust store are per-test and disposable. No network:
// `toolchain install` is layout-only by design (v1), `install <archive>`
// reads a local .cja, and `publish` is driven only to its argument-
// validation arms.

#include <gtest/gtest.h>
#include "cajeta/compile/CajetaArchive.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using cajeta::CajetaArchive;
using cajeta::CajetaArchiveEntry;

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

struct ArmWorld {
    fs::path root;
    ArmWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_arms_" + std::to_string(rng()));
        fs::create_directories(root / "home");
        fs::create_directories(root / "work");
    }
    ~ArmWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path outLog() const { return root / "out.log"; }
    fs::path ollaHome() const { return root / "olla"; }
    fs::path toolchainHome() const { return root / "toolchains"; }
    fs::path workDir() const { return root / "work"; }

    int runIn(const fs::path& dir, const std::string& args) {
        std::string cmd = "cd " + dir.string()
            + " && HOME=" + (root / "home").string()
            + " XDG_CONFIG_HOME=" + (root / "home" / ".config").string()
            + " XDG_DATA_HOME=" + (root / "home" / ".local").string()
            + " OLLA_HOME=" + ollaHome().string()
            + " CAJETA_TOOLCHAIN_HOME=" + toolchainHome().string()
            + " " + compilerBinary() + " " + args
            + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }
    int run(const std::string& args) { return runIn(workDir(), args); }

    std::string output() const {
        std::ifstream in(outLog());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // `cajeta init <template> <dir>` — the fixture project generator.
    fs::path initProject(const std::string& tmpl, const std::string& name) {
        fs::path dir = root / name;
        fs::create_directories(dir);
        EXPECT_EQ(run("init " + tmpl + " " + dir.string()), 0) << output();
        return dir;
    }

    std::string manifestOf(const fs::path& dir) const {
        std::ifstream in(dir / "cajeta.json");
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // `cajeta coverage ignore` edits `plugins.cajeta.coverage`, so the plugin
    // must be DECLARED first. The basic archetype no longer ships that
    // declaration — declaring an unpublished plugin made `cajeta init basic
    // && cajeta build` fail — so a coverage test states its own precondition.
    void declareCoveragePlugin(const fs::path& dir) const {
        std::string m = manifestOf(dir);
        size_t brace = m.find('{');
        ASSERT_NE(brace, std::string::npos) << "manifest has no object";
        m.insert(brace + 1,
                 "\n    \"plugins\": { \"cajeta.coverage\": \"1.0.*\" },\n");
        std::ofstream(dir / "cajeta.json") << m;
    }

    // A minimal library .cja (no entry-method) for `cajeta install`.
    fs::path writeLibraryArchive(const std::string& file,
                                 const std::string& name = "com.example.lib",
                                 const std::string& version = "1.0.0") {
        CajetaArchive arc(name, version, CajetaArchive::Kind::Cja);
        CajetaArchiveEntry src;
        src.name = "com/example/L.cajeta";
        src.kindTag = CajetaArchive::EntryKind::ClassSource;
        std::string body = "package com.example;\npublic class L {}\n";
        src.data.assign(body.begin(), body.end());
        arc.addEntry(std::move(src));
        fs::path p = root / file;
        arc.writeTo(p.string());
        return p;
    }
};

} // namespace

// ─── workspace ─────────────────────────────────────────────────────

TEST(BuildToolArmsTests, workspaceUsageHelpAndUnknownSubcommand) {
    ArmWorld w;
    EXPECT_EQ(w.run("workspace"), 1);
    EXPECT_NE(w.output().find("Usage: cajeta workspace"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("workspace --help"), 0);
    EXPECT_NE(w.output().find("members"), std::string::npos) << w.output();

    EXPECT_EQ(w.run("workspace frobnicate"), 1);
    EXPECT_NE(w.output().find("unknown subcommand 'frobnicate'"),
              std::string::npos) << w.output();
}

TEST(BuildToolArmsTests, workspaceMembersListsTheWorkspace) {
    ArmWorld w;
    fs::path ws = w.initProject("workspace", "ws");

    // From the workspace root: discovery walks ancestors from cwd.
    EXPECT_EQ(w.runIn(ws, "workspace members"), 0) << w.output();
    std::string out = w.output();
    EXPECT_NE(out.find("Workspace root:"), std::string::npos) << out;

    // Explicit --manifest= skips discovery and names the same root.
    EXPECT_EQ(w.runIn(w.workDir(),
        "workspace members --manifest=" + (ws / "cajeta.json").string()), 0)
        << w.output();
    EXPECT_NE(w.output().find("Workspace root:"), std::string::npos)
        << w.output();
}

TEST(BuildToolArmsTests, workspaceMembersOutsideAWorkspaceFails) {
    ArmWorld w;
    EXPECT_EQ(w.run("workspace members"), 1);
    EXPECT_NE(w.output().find("no workspace root found"), std::string::npos)
        << w.output();
}

TEST(BuildToolArmsTests, workspaceTaskArgumentArms) {
    ArmWorld w;
    // Unknown argument is refused before any workspace resolution.
    EXPECT_EQ(w.run("workspace build --bogus"), 1);
    EXPECT_NE(w.output().find("unknown argument"), std::string::npos)
        << w.output();

    // A bad --diag-format is its own refusal.
    EXPECT_EQ(w.run("workspace build --diag-format=xml"), 1);
    EXPECT_NE(w.output().find("diag-format"), std::string::npos)
        << w.output();

    // No workspace in scope: build/publish/test all report it.
    EXPECT_EQ(w.run("workspace publish"), 1);
    EXPECT_NE(w.output().find("no workspace root found"), std::string::npos)
        << w.output();
    EXPECT_EQ(w.run("workspace test"), 1);
    EXPECT_NE(w.output().find("no workspace root found"), std::string::npos)
        << w.output();
}

// ─── toolchain ─────────────────────────────────────────────────────

TEST(BuildToolArmsTests, toolchainUsageHelpAndUnknownSubcommand) {
    ArmWorld w;
    EXPECT_EQ(w.run("toolchain"), 1);
    EXPECT_NE(w.output().find("Usage: cajeta toolchain"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("toolchain --help"), 0);
    EXPECT_NE(w.output().find("install <dist>:<ver>"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("toolchain frobnicate"), 1);
    EXPECT_NE(w.output().find("unknown subcommand"), std::string::npos)
        << w.output();
}

// v1 install is layout-only: it claims the install root and lays the
// bin/lib/share skeleton the dispatcher consults. `list` then sees it.
TEST(BuildToolArmsTests, toolchainInstallLaysSkeletonAndListSeesIt) {
    ArmWorld w;
    EXPECT_EQ(w.run("toolchain install acme:1.2.3"), 0) << w.output();
    EXPECT_NE(w.output().find("Installed (skeleton) acme:1.2.3"),
              std::string::npos) << w.output();

    fs::path installRoot = w.toolchainHome() / "acme" / "1.2.3";
    bool laid = fs::exists(installRoot / "bin")
             && fs::exists(installRoot / "lib")
             && fs::exists(installRoot / "share");
    EXPECT_TRUE(laid) << w.output();

    EXPECT_EQ(w.run("toolchain list"), 0) << w.output();
    EXPECT_NE(w.output().find("acme"), std::string::npos) << w.output();
    EXPECT_NE(w.output().find("1.2.3"), std::string::npos) << w.output();
}

TEST(BuildToolArmsTests, toolchainInstallRejectsMalformedArgument) {
    ArmWorld w;
    EXPECT_EQ(w.run("toolchain install"), 1);
    EXPECT_NE(w.output().find("Usage: cajeta toolchain install"),
              std::string::npos) << w.output();

    EXPECT_EQ(w.run("toolchain install acme"), 1);          // no colon
    EXPECT_NE(w.output().find("<distribution>:<version>"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("toolchain install :1.0"), 1);          // empty dist
    EXPECT_EQ(w.run("toolchain install acme:"), 1);         // empty version
}

TEST(BuildToolArmsTests, toolchainRemoveInstalledAndMissing) {
    ArmWorld w;
    ASSERT_EQ(w.run("toolchain install acme:2.0.0"), 0) << w.output();

    EXPECT_EQ(w.run("toolchain remove acme:2.0.0"), 0) << w.output();
    EXPECT_NE(w.output().find("Removed acme:2.0.0"), std::string::npos)
        << w.output();
    EXPECT_FALSE(fs::exists(w.toolchainHome() / "acme" / "2.0.0"));

    // Second removal: not installed.
    EXPECT_EQ(w.run("toolchain remove acme:2.0.0"), 1);
    EXPECT_NE(w.output().find("is not installed"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("toolchain remove nonsense"), 1);       // malformed
}

TEST(BuildToolArmsTests, toolchainDefaultRequiresAnInstallThenSymlinks) {
    ArmWorld w;
    // Not installed → refusal that names the install command.
    EXPECT_EQ(w.run("toolchain default acme:3.0.0"), 1);
    EXPECT_NE(w.output().find("is not installed"), std::string::npos)
        << w.output();

    ASSERT_EQ(w.run("toolchain install acme:3.0.0"), 0) << w.output();
    EXPECT_EQ(w.run("toolchain default acme:3.0.0"), 0) << w.output();
    EXPECT_NE(w.output().find("Default is now acme:3.0.0"),
              std::string::npos) << w.output();

    // Re-pointing the default is idempotent (remove + recreate).
    ASSERT_EQ(w.run("toolchain install acme:3.1.0"), 0) << w.output();
    EXPECT_EQ(w.run("toolchain default acme:3.1.0"), 0) << w.output();

    EXPECT_EQ(w.run("toolchain default"), 1);               // usage
    EXPECT_EQ(w.run("toolchain default garbage"), 1);       // malformed
}

TEST(BuildToolArmsTests, toolchainPinWritesSettingsToolchain) {
    ArmWorld w;
    fs::path proj = w.initProject("basic", "pinproj");

    EXPECT_EQ(w.runIn(proj, "toolchain pin 0.19.0"), 0) << w.output();
    std::string m = w.manifestOf(proj);
    EXPECT_NE(m.find("toolchain"), std::string::npos) << m;
    EXPECT_NE(m.find("0.19.0"), std::string::npos) << m;

    // Re-pinning overwrites rather than appending a second block.
    EXPECT_EQ(w.runIn(proj, "toolchain pin 0.19.1"), 0) << w.output();
    m = w.manifestOf(proj);
    EXPECT_NE(m.find("0.19.1"), std::string::npos) << m;

    // Explicit --manifest= form, and the missing-manifest refusal.
    EXPECT_EQ(w.runIn(w.workDir(),
        "toolchain pin 0.19.2 --manifest=" + (proj / "cajeta.json").string()),
        0) << w.output();
    EXPECT_EQ(w.run("toolchain pin 0.19.0"), 1);            // no manifest here
    EXPECT_NE(w.output().find("cannot read"), std::string::npos)
        << w.output();
    EXPECT_EQ(w.run("toolchain pin"), 1);                   // usage
}

TEST(BuildToolArmsTests, toolchainWhichAndShowReport) {
    ArmWorld w;
    EXPECT_EQ(w.run("toolchain which"), 0) << w.output();
    EXPECT_FALSE(w.output().empty()) << "which printed nothing";

    EXPECT_EQ(w.run("toolchain show"), 0) << w.output();
    EXPECT_NE(w.output().find("Dispatch"), std::string::npos) << w.output();
}

// ─── install ───────────────────────────────────────────────────────




// Project mode (`cajeta install` with no archive) installs the cwd LIBRARY
// into ~/.olla — a project that declares an entry-method builds an
// executable and is refused before anything is built.
TEST(BuildToolArmsTests, installProjectModeRefusesAnExecutableProject) {
    ArmWorld w;
    fs::path proj = w.initProject("basic", "exeproj");
    EXPECT_EQ(w.runIn(proj, "install"), 1);
    EXPECT_NE(w.output().find("only libraries can be installed"),
              std::string::npos) << w.output();
}

TEST(BuildToolArmsTests, installProjectModeNeedsAManifest) {
    ArmWorld w;
    EXPECT_EQ(w.run("install"), 1);
    EXPECT_FALSE(w.output().empty()) << "expected a diagnostic";
}

// ─── upgrade ───────────────────────────────────────────────────────

TEST(BuildToolArmsTests, upgradeHelpAndArgumentArms) {
    ArmWorld w;
    EXPECT_EQ(w.run("upgrade --help"), 0);
    EXPECT_NE(w.output().find("Usage: cajeta upgrade"), std::string::npos)
        << w.output();
    EXPECT_NE(w.output().find("--melt"), std::string::npos) << w.output();

    EXPECT_EQ(w.run("upgrade --bogus"), 1);
    EXPECT_NE(w.output().find("unknown argument"), std::string::npos)
        << w.output();

    fs::path proj = w.initProject("basic", "upproj");
    EXPECT_EQ(w.runIn(proj, "upgrade name@"), 1);          // malformed spec
    EXPECT_NE(w.output().find("malformed"), std::string::npos) << w.output();
    EXPECT_EQ(w.runIn(proj, "upgrade @1.0.0"), 1);
}

TEST(BuildToolArmsTests, upgradeDryRunLeavesTheManifestAlone) {
    ArmWorld w;
    fs::path proj = w.initProject("basic", "dryproj");
    ASSERT_EQ(w.runIn(proj, "add com.example.dep@1.0.0"), 0) << w.output();
    std::string before = w.manifestOf(proj);

    // Offline: the registry lookup fails, but --dry-run must never write.
    w.runIn(proj, "upgrade --dry-run --yes");
    EXPECT_EQ(w.manifestOf(proj), before) << w.output();
}

TEST(BuildToolArmsTests, upgradeMissingManifestReported) {
    ArmWorld w;
    EXPECT_NE(w.run("upgrade --manifest=/no/such/cajeta.json"), 0);
    EXPECT_FALSE(w.output().empty()) << "expected a diagnostic";
}

// ─── publish ───────────────────────────────────────────────────────

TEST(BuildToolArmsTests, publishHelpAndUrlRequirement) {
    ArmWorld w;
    EXPECT_EQ(w.run("publish --help"), 0);
    EXPECT_NE(w.output().find("Usage: cajeta publish"), std::string::npos)
        << w.output();
    EXPECT_NE(w.output().find("--url=URL"), std::string::npos) << w.output();

    EXPECT_EQ(w.run("publish"), 2);
    EXPECT_NE(w.output().find("--url is required"), std::string::npos)
        << w.output();
}

TEST(BuildToolArmsTests, publishNeedsAReadableManifest) {
    ArmWorld w;
    EXPECT_NE(w.run("publish --url=https://example.invalid "
                    "--manifest=/no/such/cajeta.json"), 0);
    EXPECT_FALSE(w.output().empty()) << "expected a diagnostic";
}

// ─── coverage dispatcher + verify-reproducible ─────────────────────

TEST(BuildToolArmsTests, coverageUsageHelpAndUnknownSubcommand) {
    ArmWorld w;
    EXPECT_EQ(w.run("coverage"), 1);
    EXPECT_NE(w.output().find("Usage: cajeta coverage"), std::string::npos)
        << w.output();

    EXPECT_EQ(w.run("coverage --help"), 0);
    EXPECT_NE(w.output().find("ignore"), std::string::npos) << w.output();

    EXPECT_EQ(w.run("coverage frobnicate"), 1);
    EXPECT_NE(w.output().find("expected: ignore, list, remove"),
              std::string::npos) << w.output();
}

TEST(BuildToolArmsTests, coverageListFiltersByKindAndRemoveTakesHelp) {
    ArmWorld w;
    fs::path proj = w.initProject("basic", "covproj");
    w.declareCoveragePlugin(proj);
    ASSERT_EQ(w.runIn(proj, "coverage ignore --kind=file "
                            "--pattern=src/Gen.cajeta --reason=generated"), 0)
        << w.output();
    ASSERT_EQ(w.runIn(proj, "coverage ignore --kind=package "
                            "--pattern=com.example.gen --reason=generated"), 0)
        << w.output();

    EXPECT_EQ(w.runIn(proj, "coverage list --kind=package"), 0) << w.output();
    std::string out = w.output();
    EXPECT_NE(out.find("com.example.gen"), std::string::npos) << out;
    EXPECT_EQ(out.find("src/Gen.cajeta"), std::string::npos) << out;

    EXPECT_EQ(w.runIn(proj, "coverage list --help"), 0);
    EXPECT_NE(w.output().find("Usage: cajeta coverage list"),
              std::string::npos) << w.output();
    EXPECT_EQ(w.runIn(proj, "coverage remove --help"), 0);
    EXPECT_NE(w.output().find("Usage: cajeta coverage remove"),
              std::string::npos) << w.output();

    EXPECT_EQ(w.runIn(proj, "coverage list --bogus"), 1);
    EXPECT_NE(w.output().find("unknown argument"), std::string::npos)
        << w.output();
}

