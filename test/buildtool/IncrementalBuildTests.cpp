// Incremental compilation Phase 4 — `cajeta build` drives the compiler's
// cache manifest (incremental-compilation plan; Phase 3 landed the compiler
// side, exercised by test/compile/IncrementalCompileTests.cpp).
//
// The build action, when its `incremental` param is true:
//   1. asks the compiler for the cache discriminator of the exact flag set
//      it is about to pass (`--print-cache-discriminator` probe — same
//      binary, so the value is correct by construction);
//   2. computes per-source transitive digests (SourceDigestRegistry),
//      derives each source's `.bc`/obligations slots in the IrCache tree
//      (`.cajeta/cache/ir/<discriminator>/<digest>.{bc,obligations}`), and
//      marks a source clean when both slots exist;
//   3. writes the cache-manifest-v1 file and passes --cache-manifest;
//   4. after a successful build, evicts per settings.build.cache
//      ({"max-bytes": N, "max-age-seconds": N}).
//
// Drives the real built binary end-to-end (`cajeta build` in a temp
// project), same pattern as BuildToolDiagFormatJsonTests.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#ifdef _WIN32
#  define CAJETA_IB_DEVNULL "NUL"
#else
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

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_incrbuild_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

// Two-source project: Main (the entry; imports Util) + Util (the program's
// only stream() user, so a clean/skipped Util exercises obligation replay
// through the whole build-tool path). `cacheSettings` is spliced verbatim
// into settings.build when non-empty.
struct Project {
    fs::path root;

    fs::path srcDir() const {
        return root / "src" / "main" / "cajeta" / "t";
    }
    fs::path cacheIrDir() const {
        return root / ".cajeta" / "cache" / "ir";
    }
    fs::path exePath() const { return root / "build" / "exe" / "t.app"; }
    fs::path outLog() const { return root / "out.log"; }

    void writeManifest(const std::string& cacheSettings) const {
        std::ofstream m(root / "cajeta.json");
        m << "{\n"
             "  \"details\": { \"name\": \"t.app\", \"version\": \"0.1.0\",\n"
             "                 \"cajeta-lang-version\": \"1.0\" },\n"
             "  \"settings\": { \"build\": {\n"
             "      \"entry-method\": \"t.Main::run\"";
        if (!cacheSettings.empty()) m << ",\n      \"cache\": " << cacheSettings;
        m << " } },\n"
             "  \"tasks\": { \"build\": { \"actions\": [\n"
             "      { \"action\": \"build\", \"flavor\": \"debug\",\n"
             "        \"incremental\": true, \"id\": \"art\" } ] } }\n"
             "}\n";
    }

    void writeSources(int mainBias) const {
        fs::create_directories(srcDir());
        {
            std::ofstream s(srcDir() / "Util.cajeta");
            s << "package t;\n"
                 "public final class Util {\n"
                 "    public static int32 streamed() {\n"
                 "        int32[] xs = {1, 2, 3};\n"
                 "        return xs.stream().count();\n"
                 "    }\n"
                 "}\n";
        }
        {
            std::ofstream s(srcDir() / "Main.cajeta");
            s << "package t;\n"
                 "import t.Util;\n"
                 "public final class Main {\n"
                 "    public static int32 run() {"
                 " return Util.streamed() + " << mainBias << "; }\n"
                 "}\n";
        }
    }

    static Project create(const std::string& tag,
                          const std::string& cacheSettings = "") {
        Project p{freshTempDir(tag)};
        p.writeManifest(cacheSettings);
        p.writeSources(/*mainBias=*/4);   // streamed 3 + 4 = 7
        return p;
    }

    // `cajeta build` in the project dir; combined stdout+stderr → outLog.
    int build() const {
        std::string cmd = "cd " + root.string() + " && " + compilerBinary()
            + " build > " + outLog().string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    std::string buildOutput() const { return readAll(outLog()); }

    int runExe() const {
        std::string cmd = exePath().string() + " > /dev/null 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    // Files under .cajeta/cache/ir/ with the given extension.
    int cacheFileCount(const std::string& ext) const {
        int n = 0;
        std::error_code ec;
        if (!fs::exists(cacheIrDir(), ec)) return 0;
        for (auto& e : fs::recursive_directory_iterator(cacheIrDir(), ec))
            if (e.is_regular_file() && e.path().extension() == ext) n++;
        return n;
    }
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// First build populates the IrCache tree (a .bc + obligations pair per
// source); a no-change second build marks every source clean, the compiler
// skips all user codegen, and every user module's object is byte-identical
// to the full build's. (The stdlib object — and hence the exe — is NOT
// byte-pinned: replayed instantiations codegen at a different point than
// mid-loop ones, which only renumbers local `.Lcast.target.<N>` labels;
// symbol sets are identical. Known cosmetic gap, noted in the plan.)
TEST(IncrementalBuild, SecondBuildSkipsAllSourcesUserObjectsUnchanged) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("noop");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[incremental] skip"))
        << p.buildOutput();
    ASSERT_EQ(p.runExe(), 7);
    EXPECT_EQ(p.cacheFileCount(".bc"), 2) << "one .bc slot per source";
    EXPECT_EQ(p.cacheFileCount(".obligations"), 2);
    auto exeDir = p.root / "build" / "exe" / "t";
    auto mainO1 = readAll(exeDir / "Main.o");
    auto utilO1 = readAll(exeDir / "Util.o");
    ASSERT_FALSE(mainO1.empty());

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_TRUE(contains(out, "[incremental] skip t/Main.cajeta")) << out;
    EXPECT_TRUE(contains(out, "[incremental] skip t/Util.cajeta")) << out;
    EXPECT_EQ(p.runExe(), 7);
    EXPECT_EQ(readAll(exeDir / "Main.o"), mainO1)
        << "cache round-trip must reproduce the user module object";
    EXPECT_EQ(readAll(exeDir / "Util.o"), utilO1);
}

// Touching Main (which nothing imports) dirties only Main: the second build
// recompiles it, skips Util (whose sole stream() use rides obligation
// replay), and the executable tracks the edit. This pins the build-tool-side
// dirty-set computation from transitive digests (plan D1).
TEST(IncrementalBuild, TouchedSourceRecompilesOnlyItself) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("touch");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    ASSERT_EQ(p.runExe(), 7);

    p.writeSources(/*mainBias=*/5);   // touch Main only → 3 + 5 = 8
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_TRUE(contains(out, "[incremental] skip t/Util.cajeta")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip t/Main.cajeta")) << out;
    EXPECT_EQ(p.runExe(), 8);
}

// Touching Util dirties Util AND Main (Main's transitive digest includes
// Util's): nothing is skipped.
TEST(IncrementalBuild, TouchedImportDirtiesDependents) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("dep");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();

    // Rewrite Util with an extra (behavior-preserving) method — its digest
    // changes, cascading into Main's transitive digest.
    {
        std::ofstream s(p.srcDir() / "Util.cajeta");
        s << "package t;\n"
             "public final class Util {\n"
             "    public static int32 pad() { return 0; }\n"
             "    public static int32 streamed() {\n"
             "        int32[] xs = {1, 2, 3};\n"
             "        return xs.stream().count();\n"
             "    }\n"
             "}\n";
    }
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[incremental] skip"))
        << p.buildOutput();
    EXPECT_EQ(p.runExe(), 7);
}

// settings.build.cache size cap: after a successful build the action evicts
// the IrCache tree down to max-bytes — a 1-byte cap empties it.
TEST(IncrementalBuild, CacheSizeCapEvictsAfterBuild) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("evict", "{ \"max-bytes\": 1 }");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_EQ(p.cacheFileCount(".bc"), 0)
        << "post-build eviction must enforce the 1-byte cap";

    // The next build finds no cache — everything dirty, still correct.
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[incremental] skip"))
        << p.buildOutput();
    ASSERT_EQ(p.runExe(), 7);
}
