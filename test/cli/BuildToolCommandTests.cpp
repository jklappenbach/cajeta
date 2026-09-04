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
#include <llvm/Support/JSON.h>

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

    // `cajeta coverage ignore` edits `plugins.cajeta.coverage`, so a project
    // must DECLARE that plugin first. The basic archetype used to ship the
    // declaration, and these tests inherited it; it no longer does, because a
    // declaration of an unpublished plugin made `cajeta init basic &&
    // cajeta build` fail outright. Declaring it here makes the precondition
    // the test's own rather than a side effect of archetype content — and it
    // uses the string shorthand, which is the spelling users write.
    void declareCoveragePlugin(const fs::path& dir) const {
        std::string m = manifestOf(dir);
        size_t brace = m.find('{');
        ASSERT_NE(brace, std::string::npos) << "manifest has no object";
        m.insert(brace + 1,
                 "\n    \"plugins\": { \"cajeta.coverage\": \"1.0.*\" },\n");
        std::ofstream(dir / "cajeta.json") << m;
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
    w.declareCoveragePlugin(dir);

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

// ---------------------------------------------------------------------------
// dependency-tree plan, unit 3 — `cajeta deps` (spec §5.4, §6.1–§6.3).
// Store fixtures are written as files in the ~/.olla layout under
// <root>/home/.olla — FilesystemRepository lists versions from the directory,
// and the sidecar cajeta.json supplies the transitive edges — so no second
// binary is needed.

namespace {

    void seedStorePackage(const ToolWorld& w, const std::string& name,
                          const std::string& version,
                          const std::string& depsJson) {
        fs::path pkg = w.root / "home" / ".olla" / name;
        fs::path dir = pkg / version;
        fs::create_directories(dir);
        std::ofstream(dir / (name + "-" + version + ".cja"), std::ios::binary)
            << "STUB " << name << " " << version;
        std::ofstream(dir / "cajeta.json")
            << "{\"details\":{\"name\":\"" << name << "\",\"version\":\""
            << version << "\"},\"settings\":{\"dependencies\":{" << depsJson
            << "}}}";
        std::ofstream(pkg / "versions.json")
            << "{\"versions\":[\"" << version << "\"]}";
    }

    // The basic archetype ships `"dependencies": {}`; fill it.
    void declareDependency(const ToolWorld& w, const fs::path& dir,
                           const std::string& name,
                           const std::string& constraint) {
        std::string m = w.manifestOf(dir);
        const std::string empty = "\"dependencies\": {}";
        size_t at = m.find(empty);
        ASSERT_NE(at, std::string::npos) << m;
        m.replace(at, empty.size(),
                  "\"dependencies\": { \"" + name + "\": \"" + constraint + "\" }");
        std::ofstream(dir / "cajeta.json") << m;
    }

    // First `"key": "value"` in the manifest text.
    std::string manifestField(const std::string& m, const std::string& key) {
        size_t k = m.find("\"" + key + "\"");
        if (k == std::string::npos) return "";
        size_t q1 = m.find('"', m.find(':', k) + 1);
        size_t q2 = m.find('"', q1 + 1);
        return m.substr(q1 + 1, q2 - q1 - 1);
    }

} // namespace

// 3.1.1 — no dependencies: exit 0 and the single root line (§3.6).
TEST(BuildToolCommandTests, depsOnEmptyProjectPrintsRoot) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "deps"), 0) << w.output();
    std::string m = w.manifestOf(dir);
    EXPECT_EQ(w.output(),
              manifestField(m, "name") + " " + manifestField(m, "version") + "\n");
}

// 3.1.2 — JSON parses with empty lists, `--json` is the same document, CSV
// is the header alone (§5).
TEST(BuildToolCommandTests, depsJsonAndCsvOnEmptyProject) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "deps --format=json"), 0) << w.output();
    std::string viaFormat = w.output();
    auto v = llvm::json::parse(viaFormat);
    ASSERT_TRUE(static_cast<bool>(v)) << viaFormat;
    auto* root = v->getAsObject();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->getString("name").value_or(""),
              manifestField(w.manifestOf(dir), "name"));
    ASSERT_NE(root->getArray("dependencies"), nullptr);
    EXPECT_TRUE(root->getArray("dependencies")->empty());
    ASSERT_NE(root->getArray("cycles"), nullptr);
    EXPECT_TRUE(root->getArray("cycles")->empty());
    std::string manifest = root->getString("manifest").value_or("").str();
    EXPECT_TRUE(fs::path(manifest).is_absolute()) << manifest;
    EXPECT_EQ(fs::path(manifest).filename(), "cajeta.json");

    EXPECT_EQ(w.runIn(dir, "deps --json"), 0) << w.output();
    EXPECT_EQ(w.output(), viaFormat);

    EXPECT_EQ(w.runIn(dir, "deps --format=csv"), 0) << w.output();
    EXPECT_EQ(w.output(),
              "parent,name,version,requested,repository,checksum,depth,status\n");
}

// 3.1.3 — a → b from the store: b under a in text; CSV's parent column
// names a on b's row (§3.1, §5.3).
TEST(BuildToolCommandTests, depsListsTransitiveFromStore) {
    ToolWorld w;
    fs::path dir = w.initProject();
    seedStorePackage(w, "b.pkg", "1.0.0", "");
    seedStorePackage(w, "a.pkg", "1.0.0", "\"b.pkg\": \"1.0.*\"");
    declareDependency(w, dir, "a.pkg", "1.0.0");

    EXPECT_EQ(w.runIn(dir, "deps --ascii"), 0) << w.output();
    EXPECT_NE(w.output().find("`-- a.pkg 1.0.0\n    `-- b.pkg 1.0.0\n"),
              std::string::npos) << w.output();

    EXPECT_EQ(w.runIn(dir, "deps --format=csv"), 0) << w.output();
    std::string project = manifestField(w.manifestOf(dir), "name");
    EXPECT_NE(w.output().find(project + ",a.pkg,1.0.0,1.0.0,olla,sha256:"),
              std::string::npos) << w.output();
    EXPECT_NE(w.output().find("a.pkg,b.pkg,1.0.0,1.0.*,olla,sha256:"),
              std::string::npos) << w.output();
    EXPECT_NE(w.output().find(",2,\n"), std::string::npos) << w.output();
}

// 3.1.4 — a → b → a: exit 1, the chain on stderr, the tree still on stdout
// (§4.4, §4.5).
TEST(BuildToolCommandTests, depsExitsOneOnCycle) {
    ToolWorld w;
    fs::path dir = w.initProject();
    seedStorePackage(w, "a.pkg", "1.0.0", "\"b.pkg\": \"1.0.0\"");
    seedStorePackage(w, "b.pkg", "1.0.0", "\"a.pkg\": \"1.0.0\"");
    declareDependency(w, dir, "a.pkg", "1.0.0");

    EXPECT_EQ(w.runIn(dir, "deps --ascii"), 1) << w.output();
    EXPECT_NE(w.output().find(
                  "dependency cycle detected: a.pkg -> b.pkg -> a.pkg"),
              std::string::npos) << w.output();
    EXPECT_NE(w.output().find("`-- a.pkg 1.0.0 (cycle)"), std::string::npos)
        << w.output();

    // JSON carries the same cycle.
    EXPECT_EQ(w.runIn(dir, "deps --format=json"), 1) << w.output();
    // stdout and stderr share the log; the JSON is everything before the
    // cycle line, which is written after the document (§4.5).
    std::string merged = w.output();
    std::string doc = merged.substr(0, merged.find("dependency cycle detected"));
    auto v = llvm::json::parse(doc);
    ASSERT_TRUE(static_cast<bool>(v)) << merged;
    auto* cycles = v->getAsObject()->getArray("cycles");
    ASSERT_NE(cycles, nullptr);
    ASSERT_EQ(cycles->size(), 1u);
    EXPECT_EQ((*(*cycles)[0].getAsArray())[0].getAsString().value_or(""), "a.pkg");
}

// 3.1.5 — an unknown format is a usage error: exit 2 (§5.4.3).
TEST(BuildToolCommandTests, depsRejectsUnknownFormat) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "deps --format=xml"), 2) << w.output();
    EXPECT_NE(w.output().find("Usage: cajeta deps"), std::string::npos)
        << w.output();
    EXPECT_EQ(w.runIn(dir, "deps --depth=-3"), 2) << w.output();
    EXPECT_EQ(w.runIn(dir, "deps --bogus"), 2) << w.output();
}

// 3.1.6 — --help exits 0 (§5.4.4).
TEST(BuildToolCommandTests, depsHelpExitsZero) {
    ToolWorld w;
    EXPECT_EQ(w.run("deps --help"), 0) << w.output();
    EXPECT_NE(w.output().find("Usage: cajeta deps"), std::string::npos)
        << w.output();
}

// 3.1.7 — no manifest: exit 1 (§5.4.1).
TEST(BuildToolCommandTests, depsWithoutManifestExitsOne) {
    ToolWorld w;
    EXPECT_EQ(w.run("deps"), 1) << w.output();
}

// 3.1.8 — `tasks --json` advertises the verb to the IDE (§6.2).
TEST(BuildToolCommandTests, tasksJsonListsDepsBuiltin) {
    ToolWorld w;
    fs::path dir = w.initProject();
    EXPECT_EQ(w.runIn(dir, "tasks --json"), 0) << w.output();
    EXPECT_NE(w.output().find("\"deps\""), std::string::npos) << w.output();
    EXPECT_NE(w.output().find("Print the dependency tree"), std::string::npos)
        << w.output();
    // The frozen `info` description is untouched.
    EXPECT_NE(w.output().find("Print dependency tree / capabilities"),
              std::string::npos) << w.output();
}
