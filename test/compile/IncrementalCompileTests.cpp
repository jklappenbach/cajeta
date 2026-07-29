// Incremental compilation Phase 3 — codegen skip + obligation replay
// (docs/specification/buildtool/IncrementalCompilation.md; plan Phase 3).
//
// The build tool owns the dirty-set decision and hands it to the compiler
// via `--cache-manifest=<path>` (cache-manifest-v1 JSON, pulled forward from
// plan Phase 4 per decision D1). Clean sources are parsed for declarations
// but codegen-skipped: their cached `.bc` is loaded in place of generated IR
// and their recorded instantiation obligations are replayed into stdlib so
// the loaded `.bc` still links.
//
// Manifest contract exercised here:
//   { "version": "cache-manifest-v1",
//     "discriminator": "<value>" | "" (populate mode; all entries dirty),
//     "sources": [ { "path": "<source-root-relative>", "clean": bool,
//                    "bc": "<abs slot>", "obligations": "<abs slot>" } ] }
//
// Observable contract:
//   - `[incremental] discriminator <value>` printed when a manifest is active.
//   - `[incremental] skip <rel path>` printed per codegen-skipped source.
//   - dirty sources write their `.bc` + obligations sidecar (possibly empty,
//     always present) to the manifest slots.
//   - discriminator mismatch ⇒ `discriminator mismatch` warning, manifest
//     ignored, full rebuild still succeeds.
//
// Process-isolated (fork+exec the built compiler), same pattern as
// InstantiationObligationTests.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define CAJETA_INC_DEVNULL "NUL"
#else
#  include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

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
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
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
              / ("cajeta_incr_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

// One source's manifest row.
struct ManifestEntry {
    std::string relPath;      // source-root-relative
    bool clean;
};

// The four-source fixture. `test.Util.streamed()` is the program's ONLY use
// of `xs.stream()` — when Util is clean/skipped, ArrayStream<int32> reaches
// stdlib solely via obligation replay, so exe link+run proves the replay.
struct Fixture {
    fs::path root, srcRoot, build, cacheDir, manifestPath, outLog;
    std::string discriminator;   // learned from the populate build

    static constexpr const char* kSources[4] =
        {"test/Base.cajeta", "test/Util.cajeta",
         "test/Main.cajeta", "test/Lone.cajeta"};

    fs::path src(const std::string& rel) const { return srcRoot / rel; }
    fs::path bcSlot(const std::string& rel) const {
        return cacheDir / (fs::path(rel).stem().string() + ".bc");
    }
    fs::path obligationsSlot(const std::string& rel) const {
        return cacheDir / (fs::path(rel).stem().string() + ".obligations");
    }
    fs::path exePath() const { return build / "a.out"; }

    void writeSources(int baseVal, const std::string& mainTail) {
        writeFile(src("test/Base.cajeta"),
            "package test;\n"
            "public final class Base {\n"
            "    public static int32 val() { return " + std::to_string(baseVal) + "; }\n"
            "}\n");
        writeFile(src("test/Util.cajeta"),
            "package test;\n"
            "public final class Util {\n"
            "    public static int32 compute() { return Base.val() + 3; }\n"
            "    public static int32 streamed() {\n"
            "        int32[] xs = [1, 2, 3];\n"
            "        return xs.stream().count();\n"
            "    }\n"
            "}\n");
        writeFile(src("test/Main.cajeta"),
            "package test;\n"
            "public final class Main {\n"
            "    public static int32 run() {"
            " return Util.compute() + Util.streamed()" + mainTail + "; }\n"
            "}\n");
        writeFile(src("test/Lone.cajeta"),
            "package test;\n"
            "public final class Lone {\n"
            "    public static int32 x() { return 1; }\n"
            "}\n");
    }

    void writeManifest(const std::string& discriminatorValue,
                       const std::vector<ManifestEntry>& entries) {
        std::stringstream m;
        m << "{\n  \"version\": \"cache-manifest-v1\",\n"
          << "  \"discriminator\": \"" << discriminatorValue << "\",\n"
          << "  \"sources\": [\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            m << "    { \"path\": \"" << e.relPath << "\", "
              << "\"clean\": " << (e.clean ? "true" : "false") << ", "
              << "\"bc\": \"" << bcSlot(e.relPath).string() << "\", "
              << "\"obligations\": \""
              << obligationsSlot(e.relPath).string() << "\" }"
              << (i + 1 < entries.size() ? "," : "") << "\n";
        }
        m << "  ]\n}\n";
        writeFile(manifestPath, m.str());
    }

    // Runs `cajeta --emit=exe --cache-manifest=... test.Main::run src build`,
    // capturing combined stdout+stderr into outLog. Returns compiler exit code.
    int build2(bool withManifest = true) {
        std::string cmd = compilerBinary() + " --emit=exe";
        if (withManifest) cmd += " --cache-manifest=" + manifestPath.string();
        cmd += " test.Main::run " + srcRoot.string() + " " + build.string()
             + " > " + outLog.string() + " 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    std::string buildOutput() const { return readAll(outLog); }

    // Runs the produced executable; returns its exit code (= run()'s int32).
    int runExe() const {
        std::string cmd = exePath().string() + " > /dev/null 2>&1";
        return exitCodeOf(std::system(cmd.c_str()));
    }

    std::vector<ManifestEntry> allDirty() const {
        std::vector<ManifestEntry> v;
        for (auto* s : kSources) v.push_back({s, false});
        return v;
    }

    // Extracts the discriminator the compiler printed:
    //   [incremental] discriminator <value>
    static std::string parseDiscriminator(const std::string& output) {
        const std::string tag = "[incremental] discriminator ";
        auto pos = output.find(tag);
        if (pos == std::string::npos) return {};
        auto start = pos + tag.size();
        auto end = output.find_first_of(" \r\n", start);
        return output.substr(start, end - start);
    }

    static bool available() { return fs::exists(compilerBinary()); }

    // Populate build: all sources dirty, empty discriminator ("adopt mine").
    // Learns the real discriminator for the incremental builds that follow.
    bool populate() {
        root = freshTempDir("fixture");
        srcRoot = root / "src";
        build = root / "build";
        cacheDir = root / "cache";
        manifestPath = root / "manifest.json";
        outLog = root / "out.log";
        fs::create_directories(build);
        fs::create_directories(cacheDir);
        writeSources(/*baseVal=*/7, /*mainTail=*/"");
        writeManifest("", allDirty());
        if (build2() != 0) return false;
        discriminator = parseDiscriminator(buildOutput());
        return !discriminator.empty();
    }
};

constexpr const char* Fixture::kSources[4];

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// Populate build writes every dirty source's .bc + obligations sidecar to the
// manifest slots. Obligation sidecar is ALWAYS present for a dirty source —
// explicitly empty when the module has none (absence must mean "never
// written", not "no obligations").
TEST(IncrementalCompile, PopulateBuildFillsManifestSlots) {
    if (!Fixture::available()) GTEST_SKIP() << "compiler binary unavailable";
    Fixture f;
    ASSERT_TRUE(f.populate()) << f.buildOutput();
    EXPECT_EQ(f.runExe(), 13);   // compute 10 + streamed 3

    for (auto* s : Fixture::kSources) {
        EXPECT_TRUE(fs::exists(f.bcSlot(s))) << "missing .bc slot for " << s;
        EXPECT_TRUE(fs::exists(f.obligationsSlot(s)))
            << "missing obligations slot for " << s;
    }
    // Util drives the program's only template use.
    std::string utilObligations = readAll(f.obligationsSlot("test/Util.cajeta"));
    EXPECT_TRUE(contains(utilObligations, "ArrayStream")) << utilObligations;
    // Lone drives none — slot exists but is empty.
    EXPECT_TRUE(readAll(f.obligationsSlot("test/Lone.cajeta")).empty());
}

// Acceptance "touch one leaf source ⇒ only it is recompiled" + "a clean
// module whose sole template use is skipped still links" (obligation replay):
// Main is the only dirty source; Util (the only stream() user) is skipped, so
// the exe links+runs only if Util's recorded ArrayStream<int32> obligation was
// replayed into stdlib.
TEST(IncrementalCompile, LeafTouchSkipsCleanModulesAndReplaysObligations) {
    if (!Fixture::available()) GTEST_SKIP() << "compiler binary unavailable";
    Fixture f;
    ASSERT_TRUE(f.populate()) << f.buildOutput();

    f.writeSources(/*baseVal=*/7, /*mainTail=*/" + 1");   // touch Main only
    f.writeManifest(f.discriminator, {
        {"test/Base.cajeta", true},
        {"test/Util.cajeta", true},
        {"test/Main.cajeta", false},
        {"test/Lone.cajeta", true},
    });
    ASSERT_EQ(f.build2(), 0) << f.buildOutput();

    std::string out = f.buildOutput();
    EXPECT_TRUE(contains(out, "[incremental] skip test/Base.cajeta")) << out;
    EXPECT_TRUE(contains(out, "[incremental] skip test/Util.cajeta")) << out;
    EXPECT_TRUE(contains(out, "[incremental] skip test/Lone.cajeta")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip test/Main.cajeta")) << out;

    EXPECT_EQ(f.runExe(), 14);   // 13 + the touched "+ 1"
}

// Acceptance "touch a widely-imported source ⇒ it + all transitive dependents
// recompiled, the rest skipped". The dirty-set expansion itself is the build
// tool's job (D1); the compiler-side contract is honoring a mixed manifest:
// recompile the three dirty sources, skip exactly the unaffected one.
TEST(IncrementalCompile, SharedTouchRecompilesDependentsSkipsRest) {
    if (!Fixture::available()) GTEST_SKIP() << "compiler binary unavailable";
    Fixture f;
    ASSERT_TRUE(f.populate()) << f.buildOutput();

    f.writeSources(/*baseVal=*/8, /*mainTail=*/"");   // touch Base
    f.writeManifest(f.discriminator, {
        {"test/Base.cajeta", false},
        {"test/Util.cajeta", false},   // imports Base → dirty
        {"test/Main.cajeta", false},   // imports Util → dirty
        {"test/Lone.cajeta", true},
    });
    ASSERT_EQ(f.build2(), 0) << f.buildOutput();

    std::string out = f.buildOutput();
    EXPECT_TRUE(contains(out, "[incremental] skip test/Lone.cajeta")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip test/Base.cajeta")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip test/Util.cajeta")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip test/Main.cajeta")) << out;

    EXPECT_EQ(f.runExe(), 14);   // compute 11 + streamed 3
}

// Acceptance "discriminator mismatch ⇒ ignore cache, full rebuild, warn".
// The stale manifest must not feed IR into the build; the build still
// succeeds and produces a correct executable.
TEST(IncrementalCompile, DiscriminatorMismatchWarnsAndFullyRebuilds) {
    if (!Fixture::available()) GTEST_SKIP() << "compiler binary unavailable";
    Fixture f;
    ASSERT_TRUE(f.populate()) << f.buildOutput();

    f.writeManifest("deadbeef", {
        {"test/Base.cajeta", true},
        {"test/Util.cajeta", true},
        {"test/Main.cajeta", false},
        {"test/Lone.cajeta", true},
    });
    ASSERT_EQ(f.build2(), 0) << f.buildOutput();

    std::string out = f.buildOutput();
    EXPECT_TRUE(contains(out, "discriminator mismatch")) << out;
    EXPECT_FALSE(contains(out, "[incremental] skip")) << out;

    EXPECT_EQ(f.runExe(), 13);   // untouched program, full rebuild
}

// Populate mode (empty discriminator) is write-only: a manifest that couples
// it with a clean entry is malformed and must be rejected loudly, not
// half-honored.
TEST(IncrementalCompile, EmptyDiscriminatorWithCleanEntryIsRejected) {
    if (!Fixture::available()) GTEST_SKIP() << "compiler binary unavailable";
    Fixture f;
    ASSERT_TRUE(f.populate()) << f.buildOutput();

    f.writeManifest("", {
        {"test/Base.cajeta", true},
        {"test/Util.cajeta", false},
        {"test/Main.cajeta", false},
        {"test/Lone.cajeta", false},
    });
    EXPECT_NE(f.build2(), 0) << f.buildOutput();
}
