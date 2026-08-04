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
#include <map>
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

    // `incrementalParam`: "" omits the param (exercises the default),
    // otherwise spliced verbatim ("true" / "false").
    void writeManifest(const std::string& cacheSettings,
                       const std::string& incrementalParam = "true") const {
        std::ofstream m(root / "cajeta.json");
        m << "{\n"
             "  \"details\": { \"name\": \"t.app\", \"version\": \"0.1.0\",\n"
             "                 \"cajeta-lang-version\": \"1.0\" },\n"
             "  \"settings\": { \"build\": {\n"
             "      \"entry-method\": \"t.Main::run\"";
        if (!cacheSettings.empty()) m << ",\n      \"cache\": " << cacheSettings;
        m << " } },\n"
             "  \"tasks\": { \"build\": { \"actions\": [\n"
             "      { \"action\": \"build\", \"flavor\": \"debug\",\n";
        if (!incrementalParam.empty())
            m << "        \"incremental\": " << incrementalParam << ",\n";
        m << "        \"id\": \"art\" } ] } }\n"
             "}\n";
    }

    void writeSources(int mainBias) const {
        fs::create_directories(srcDir());
        {
            std::ofstream s(srcDir() / "Util.cajeta");
            s << "package t;\n"
                 "public final class Util {\n"
                 "    public static int32 streamed() {\n"
                 "        int32[] xs = [1, 2, 3];\n"
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
                          const std::string& cacheSettings = "",
                          const std::string& incrementalParam = "true") {
        Project p{freshTempDir(tag)};
        p.writeManifest(cacheSettings, incrementalParam);
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

    // Hand-authored agent skills live beside cajeta.json, OUTSIDE the source
    // root, and are embedded into the built artifact.
    void writeSkill(const std::string& body) const {
        fs::create_directories(root / "skills");
        std::ofstream s(root / "skills" / "t-overview.md");
        s << "---\n"
             "id: t-overview\n"
             "applies-to: [t.Main]\n"
             "title: t — orientation\n"
             "description: routing for t\n"
             "---\n\n"
          << body << "\n";
    }

    // Force the next build past the Phase-0 whole-artifact layer so it
    // exercises the manifest/codegen-skip path (artifact evicted, IR slots
    // intact — a real cache state).
    void dropArtifactCache() const {
        std::error_code ec;
        fs::remove_all(root / ".cajeta" / "cache" / "artifact", ec);
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

    p.dropArtifactCache();   // exercise the manifest path, not the Phase-0 hit
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_TRUE(contains(out, "[incremental] skip t/Main.cajeta")) << out;
    // Phase 6-alt: both clean modules' native objects come from the cache —
    // no target lowering at all.
    EXPECT_TRUE(contains(out, "[incremental] reused 2 cached object"))
        << out;
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
             "        int32[] xs = [1, 2, 3];\n"
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

// Phase 5: incremental is the DEFAULT for `cajeta build` — a manifest with
// no `incremental` param behaves like `incremental: true`.
TEST(IncrementalBuild, IncrementalIsTheDefault) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("dflt", "", /*incrementalParam=*/"");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_EQ(p.cacheFileCount(".bc"), 2) << "default-on must populate";

    // No-change rebuild: the Phase-0 whole-artifact layer answers first.
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();
    EXPECT_EQ(p.runExe(), 7);

    // With the artifact evicted, the manifest layer answers: all skipped.
    p.dropArtifactCache();
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_TRUE(contains(out, "[incremental] skip t/Main.cajeta")) << out;
    EXPECT_TRUE(contains(out, "[incremental] skip t/Util.cajeta")) << out;
    EXPECT_EQ(p.runExe(), 7);
}

// `incremental: false` opts out entirely: no probe, no cache tree, no skips.
TEST(IncrementalBuild, ExplicitOptOutDisablesCache) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("optout", "", /*incrementalParam=*/"false");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_EQ(p.cacheFileCount(".bc"), 0);

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[incremental] skip"))
        << p.buildOutput();
    EXPECT_EQ(p.runExe(), 7);
}

// Rebuild after eviction produces byte-identical IR — the build-tool-plan
// Phase 5b criterion. Populate, snapshot every .bc slot, wipe the cache,
// rebuild: the same slot paths reappear with the same bytes (deterministic
// codegen + content-addressed keys).
TEST(IncrementalBuild, RebuildAfterEvictionByteIdenticalIr) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("evictbytes");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::map<std::string, std::string> before;
    for (auto& e : fs::recursive_directory_iterator(p.cacheIrDir()))
        if (e.is_regular_file() && e.path().extension() == ".bc")
            before[e.path().string()] = readAll(e.path());
    ASSERT_EQ(before.size(), 2u);

    fs::remove_all(p.cacheIrDir());
    p.dropArtifactCache();
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    for (auto& [path, bytes] : before) {
        ASSERT_TRUE(fs::exists(path)) << "slot did not repopulate: " << path;
        EXPECT_EQ(readAll(path), bytes)
            << "rebuild after eviction must be byte-identical: " << path;
    }
}

// compile-cache D1 — the demand set is larger than the obligation set.
// A FOLDABLE stdlib static (JsonValue.OBJECT = 5) is defined in the stdlib
// object only when a referencing module's codegen demands it
// (getOrCreateStaticFieldGlobal); non-foldable statics are covered by the
// declaring module's unconditional clinit pass and were never at risk.
// When the referencing module is clean and skipped, the demand must be
// replayed from its obligations sidecar (`Owner::field` key) or the
// symbol vanishes from the fresh stdlib object and the link fails with
// `undefined symbol: cajeta.codec.json.JsonValue.OBJECT`.
TEST(IncrementalBuild, WarmRebuildKeepsDemandedStdlibStatics) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    Project p = Project::create("stdstatic");
    {   // Util becomes the program's ONLY referencer of the static.
        std::ofstream s(p.srcDir() / "Util.cajeta");
        s << "package t;\n"
             "import cajeta.codec.json.JsonValue;\n"
             "public final class Util {\n"
             "    public static int32 streamed() {\n"
             "        return JsonValue.OBJECT;\n"
             "    }\n"
             "}\n";
    }
    ASSERT_EQ(p.build(), 0) << p.buildOutput();      // cold: links fine
    ASSERT_EQ(p.runExe(), 9) << "OBJECT(5) + mainBias(4)";
    p.dropArtifactCache();   // past Phase-0, onto the manifest/skip path
    ASSERT_EQ(p.build(), 0) << p.buildOutput();      // D1 died at this link
    EXPECT_TRUE(contains(p.buildOutput(), "[incremental] skip"))
        << p.buildOutput();
    ASSERT_EQ(p.runExe(), 9)
        << "the replayed static must keep its folded value";
}

// Phase 5 real-source-tree smoke: samples/tour (~87 sources, no deps)
// builds end-to-end under default-on incremental, and a no-change rebuild
// skips every source. Runs the suite's usual CI lane, satisfying the
// "wire into CI" criterion.
// (DISABLED_ 2026-07-31 → re-enabled 2026-08-04: the tour carries the
// compile-cache D1 repro content, so this smoke was red — undefined
// JsonValue.OBJECT at the warm link — until the static-field obligation
// kind landed. Re-enabling it was part of D1's definition of done.)
TEST(IncrementalBuild, TourSmokeBuildsIncrementally) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string repoRoot;
    if (envRoot && *envRoot) repoRoot = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        repoRoot = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        repoRoot = ".";
#endif
    }
    fs::path tour = fs::path(repoRoot) / "samples" / "tour";
    if (!fs::exists(tour / "cajeta.json"))
        GTEST_SKIP() << "samples/tour unavailable";

    auto root = freshTempDir("tour");
    std::error_code ec;
    fs::copy(tour / "cajeta.json", root / "cajeta.json", ec);
    fs::copy(tour / "src", root / "src", fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto log = root / "out.log";
    auto build = [&]() {
        std::string cmd = "cd " + root.string() + " && " + compilerBinary()
            + " build > " + log.string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    };
    ASSERT_EQ(build(), 0) << readAll(log);
    EXPECT_FALSE(contains(readAll(log), "[incremental] skip"))
        << readAll(log);

    // Force past the Phase-0 layer so the manifest/skip path is what runs.
    std::error_code aec;
    fs::remove_all(root / ".cajeta" / "cache" / "artifact", aec);
    ASSERT_EQ(build(), 0) << readAll(log);
    std::string out = readAll(log);
    EXPECT_TRUE(contains(out, "[incremental] skip tour/Tour.cajeta")) << out;
    // Every source clean on the no-change rebuild — no compile lines for any.
    int skips = 0;
    for (size_t pos = 0; (pos = out.find("[incremental] skip", pos))
                         != std::string::npos; ++pos)
        skips++;
    EXPECT_GE(skips, 80) << "expected ~87 skipped sources, got " << skips
                         << ":\n" << out;

    // And a third, untouched build takes the Phase-0 fast path outright.
    ASSERT_EQ(build(), 0) << readAll(log);
    EXPECT_TRUE(contains(readAll(log), "[cache] hit")) << readAll(log);
}

// Phase 0: whole-artifact fast path. A no-change second build computes the
// same whole-build digest, finds the cached artifact, and re-publishes it
// WITHOUT running a compile: `cache = hit` in the outputs, byte-identical
// artifact, and none of the incremental machinery's output.
TEST(IncrementalBuild, WholeArtifactHitSkipsTheCompile) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("wahit");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();
    ASSERT_EQ(p.runExe(), 7);
    auto exe1 = readAll(p.exePath());

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_TRUE(contains(out, "[cache] hit")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip"))
        << "a whole-artifact hit must not reach the compile at all:\n" << out;
    EXPECT_EQ(readAll(p.exePath()), exe1);
    EXPECT_EQ(p.runExe(), 7);
}

// A touch misses the whole-artifact layer and falls through to the normal
// incremental build; the NEXT no-change build hits with the new artifact.
TEST(IncrementalBuild, WholeArtifactMissFallsThroughThenRehits) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("wamiss");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    ASSERT_EQ(p.runExe(), 7);

    p.writeSources(/*mainBias=*/5);   // touch Main → 8
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_FALSE(contains(out, "[cache] hit")) << out;
    EXPECT_TRUE(contains(out, "[incremental] skip t/Util.cajeta")) << out;
    ASSERT_EQ(p.runExe(), 8);
    auto exe2 = readAll(p.exePath());

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();
    EXPECT_EQ(readAll(p.exePath()), exe2);
    EXPECT_EQ(p.runExe(), 8);
}

// Editing an EMBEDDED RESOURCE busts the whole-artifact key.
//
// `skills/` is authored beside cajeta.json, outside the positional source
// root, so the key's `.cajeta` walk never saw it: a skill edit left the key
// unchanged and the cache re-published an artifact carrying the OLD skill —
// byte-identical, silently stale, and invisible until someone read the
// shipped documentation and found it describing the previous release.
// Documentation that ships inside the artifact is a build input.
TEST(IncrementalBuild, EditedSkillMissesTheWholeArtifactCache) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("skillkey");
    p.writeSkill("first body");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();

    // No change at all — the key must still hit, or the fix would be a
    // blanket cache defeat rather than a correction.
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();

    // Skill edited, sources untouched → MISS.
    p.writeSkill("second body, materially different");
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[cache] hit"))
        << "an edited skill must re-key the artifact:\n" << p.buildOutput();

    // ...and the rebuilt state hits again, so the key is content-addressed
    // rather than merely invalidated once.
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();

    // Reverting to the ORIGINAL skill returns to the ORIGINAL key — content
    // addressing, not a monotonic counter.
    p.writeSkill("first body");
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_TRUE(contains(p.buildOutput(), "[cache] hit"))
        << "reverting a skill should land back on the first artifact:\n"
        << p.buildOutput();
}

// Adding a NEW skill file re-keys too — the walk must be over the directory,
// not a fixed set of paths sampled once.
TEST(IncrementalBuild, AddedSkillFileMissesTheWholeArtifactCache) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto p = Project::create("skilladd");
    p.writeSkill("only skill");

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    ASSERT_TRUE(contains(p.buildOutput(), "[cache] hit")) << p.buildOutput();

    {
        std::ofstream s(p.root / "skills" / "t-second.md");
        s << "---\nid: t-second\napplies-to: [t.Util]\n"
             "title: t — second\ndescription: more routing\n---\n\nbody\n";
    }
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    EXPECT_FALSE(contains(p.buildOutput(), "[cache] hit"))
        << "a new skill file must re-key the artifact:\n" << p.buildOutput();
}

// `no-cache: true` bypasses BOTH cache layers: no artifact reuse, no
// manifest — every build is a plain full compile.
TEST(IncrementalBuild, NoCacheParamBypassesBothLayers) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    Project p{freshTempDir("nocache")};
    {
        std::ofstream m(p.root / "cajeta.json");
        m << "{\n"
             "  \"details\": { \"name\": \"t.app\", \"version\": \"0.1.0\",\n"
             "                 \"cajeta-lang-version\": \"1.0\" },\n"
             "  \"settings\": { \"build\": {\n"
             "      \"entry-method\": \"t.Main::run\" } },\n"
             "  \"tasks\": { \"build\": { \"actions\": [\n"
             "      { \"action\": \"build\", \"flavor\": \"debug\",\n"
             "        \"no-cache\": true, \"id\": \"art\" } ] } }\n"
             "}\n";
    }
    p.writeSources(/*mainBias=*/4);

    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    ASSERT_EQ(p.build(), 0) << p.buildOutput();
    std::string out = p.buildOutput();
    EXPECT_FALSE(contains(out, "[cache] hit")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip")) << out;
    EXPECT_EQ(p.cacheFileCount(".bc"), 0);
    EXPECT_EQ(p.runExe(), 7);
}
