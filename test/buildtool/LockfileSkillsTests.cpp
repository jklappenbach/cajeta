// `cajeta build` records the resolved dependencies in `cajeta.lock`, and the
// skill search finds a dependency's skills through it (BuildTool.md, Lockfile).
//
// Measured 2026-10-21 from cajeta-cloud-aws: `search-skill` and the MCP server
// read packages from `./cajeta.lock`, but `cajeta build` wrote no lockfile and
// `cajeta info --write-lockfile` wrote one with no packages, so no dependency's
// skill was ever found. dev.cajeta.codec 0.9.0's XML skill was packed and
// indexed in its archive and invisible to every consumer.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

#include "../PortableEnv.h"

#ifndef _WIN32
#  include <sys/wait.h>
#endif

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

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

// A library `t.skilled` 0.1.0 with one class and one skill, published into a
// filesystem repository, and a consumer `t.consumer` depending on it. HOME is
// a fresh directory, so ~/.olla on the box never takes part.
struct SkillFixture {
    fs::path root;
    std::shared_ptr<void> cleanup;

    static SkillFixture create(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto dir = fs::temp_directory_path()
                 / ("cajeta_lockskill_" + tag + "_" + std::to_string(rng()));
        fs::create_directories(dir);
        SkillFixture f;
        f.root = dir;
        f.cleanup = std::shared_ptr<void>(nullptr, [dir](void*) {
            if (std::getenv("CAJETA_KEEP_TEMP")) return;
            std::error_code ec;
            fs::remove_all(dir, ec);
        });
        fs::create_directories(f.home());

        fs::path lsrc = f.lib() / "src" / "main" / "cajeta" / "t";
        fs::create_directories(lsrc);
        std::ofstream(lsrc / "Greeter.cajeta")
            << "package t;\n"
               "public final class Greeter {\n"
               "    public static int32 answer() { return 42; }\n"
               "}\n";
        fs::create_directories(f.lib() / "skills");
        std::ofstream(f.lib() / "skills" / "greeter-overview.md")
            << "---\n"
               "id: greeter-overview\n"
               "applies-to: [t/Greeter]\n"
               "title: Greeter, the fixture skill\n"
               "description: A skill a consumer must be able to find.\n"
               "---\n\n# Greeter\n\nCall `Greeter.answer()`.\n";
        std::ofstream(f.lib() / "cajeta.json")
            << "{\n"
               "  \"details\": { \"name\": \"t.skilled\", \"version\": \"0.1.0\",\n"
               "                 \"cajeta-lang-version\": \"1.0\" },\n"
               "  \"settings\": { \"build\": { \"source-root\": \"src/main/cajeta\" } },\n"
               "  \"tasks\": { \"build\": { \"actions\": [\n"
               "      { \"action\": \"build\", \"flavor\": \"debug\", \"id\": \"art\" } ] } }\n"
               "}\n";

        fs::path csrc = f.consumer() / "src" / "main" / "cajeta" / "c";
        fs::create_directories(csrc);
        std::ofstream(csrc / "Use.cajeta")
            << "package c;\n"
               "import t.Greeter;\n"
               "public final class Use {\n"
               "    public static int32 get() { return Greeter.answer(); }\n"
               "}\n";
        std::ofstream(f.consumer() / "cajeta.json")
            << "{\n"
               "  \"details\": { \"name\": \"t.consumer\", \"version\": \"0.1.0\",\n"
               "                 \"cajeta-lang-version\": \"1.0\" },\n"
               "  \"settings\": {\n"
               "    \"repositories\": [ { \"name\": \"local\", \"type\": \"filesystem\",\n"
               "        \"path\": \"" << f.repo().generic_string() << "\",\n"
               "        \"verification\": \"checksum\" } ],\n"
               "    \"dependencies\": { \"t.skilled\": \"0.1.0\" },\n"
               "    \"build\": { \"source-root\": \"src/main/cajeta\" }\n"
               "  },\n"
               "  \"tasks\": { \"build\": { \"actions\": [\n"
               "      { \"action\": \"build\", \"flavor\": \"debug\", \"id\": \"art\" } ] } }\n"
               "}\n";
        return f;
    }

    fs::path home() const { return root / "home"; }
    fs::path lib() const { return root / "lib"; }
    fs::path consumer() const { return root / "consumer"; }
    fs::path repo() const { return root / "repo"; }
    fs::path lockfile() const { return consumer() / "cajeta.lock"; }
    fs::path outLog() const { return root / "out.log"; }

    // Runs `cajeta <args>` in `dir` with HOME at the fixture's home.
    int run(const fs::path& dir, const std::string& args) const {
        setenv("HOME", home().string().c_str(), 1);
        unsetenv("OLLA_HOME");
        std::string cmd = CAJETA_PORTABLE_CD + dir.string() + " && "
            + compilerBinary() + " " + args + " > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    std::string output() const { return readAll(outLog()); }

    // Builds the library and publishes its archive into the repository.
    bool publishLib() const {
        if (run(lib(), "build") != 0) return false;
        fs::path cja = lib() / "build" / "archive" / "t.skilled-0.1.0.cja";
        fs::path dst = repo() / "t.skilled" / "0.1.0";
        fs::create_directories(dst);
        std::error_code ec;
        fs::copy_file(cja, dst / "t.skilled-0.1.0.cja",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        std::ofstream(dst / "cajeta.json")
            << "{\"details\":{\"name\":\"t.skilled\",\"version\":\"0.1.0\"}}";
        return true;
    }
};

} // namespace

TEST(LockfileSkills, buildRecordsResolvedPackagesInTheLockfile) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto f = SkillFixture::create("lock");
    ASSERT_TRUE(f.publishLib()) << f.output();
    ASSERT_EQ(f.run(f.consumer(), "build"), 0) << f.output();
    ASSERT_TRUE(fs::exists(f.lockfile())) << "cajeta build wrote no cajeta.lock";
    std::string lock = readAll(f.lockfile());
    EXPECT_TRUE(contains(lock, "\"name\": \"t.skilled\"")) << lock;
    EXPECT_TRUE(contains(lock, "\"version\": \"0.1.0\"")) << lock;
    EXPECT_TRUE(contains(lock, "\"checksum\": \"sha256:")) << lock;
}

TEST(LockfileSkills, searchSkillFindsADependencysSkillAfterBuild) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto f = SkillFixture::create("search");
    ASSERT_TRUE(f.publishLib()) << f.output();
    ASSERT_EQ(f.run(f.consumer(), "build"), 0) << f.output();
    ASSERT_EQ(f.run(f.consumer(), "search-skill t/Greeter"), 0) << f.output();
    EXPECT_TRUE(contains(f.output(), "cja-skill://t.skilled@0.1.0/greeter-overview"))
        << f.output();
}

TEST(LockfileSkills, anUnchangedRebuildLeavesTheLockfileByteIdentical) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto f = SkillFixture::create("stable");
    ASSERT_TRUE(f.publishLib()) << f.output();
    ASSERT_EQ(f.run(f.consumer(), "build"), 0) << f.output();
    ASSERT_TRUE(fs::exists(f.lockfile()));
    std::string first = readAll(f.lockfile());
    ASSERT_EQ(f.run(f.consumer(), "build"), 0) << f.output();
    EXPECT_EQ(readAll(f.lockfile()), first)
        << "a rebuild of the same graph rewrote cajeta.lock (resolved-at churn)";
}

TEST(LockfileSkills, infoWriteLockfileRecordsResolvedPackages) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto f = SkillFixture::create("info");
    ASSERT_TRUE(f.publishLib()) << f.output();
    ASSERT_EQ(f.run(f.consumer(), "info --write-lockfile"), 0) << f.output();
    ASSERT_TRUE(fs::exists(f.lockfile()));
    EXPECT_TRUE(contains(readAll(f.lockfile()), "\"name\": \"t.skilled\""))
        << readAll(f.lockfile());
}

TEST(LockfileSkills, aProjectWithNoDependenciesSearchesStdlibSkillsStill) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable";
    auto f = SkillFixture::create("nodeps");
    ASSERT_EQ(f.run(f.lib(), "build"), 0) << f.output();
    ASSERT_EQ(f.run(f.lib(), "search-skill cajeta/codec/Base64"), 0) << f.output();
    EXPECT_TRUE(contains(f.output(), "cja-skill://cajeta.codec@")) << f.output();
}
