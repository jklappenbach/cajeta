// build-output-layout unit 2 — generated files have declared homes.
//
// Spec §3.1 gives the three roles distinct defaults: intermediates (per-class
// objects, bitcode, staging) under `build/obj/`, artifacts under
// `build/archive/` (a .cja) and the exe directory. Today an --emit=exe build
// puts BOTH in one place:
//
//     build/exe/app/Main.o                    <- intermediate
//     build/exe/cajeta.runtime.__stdlib__.o   <- intermediate
//     build/exe/com.example.library           <- the artifact
//
// That is what §2.5's incident was about at the compiler level, and it is
// also the cause of a separate open defect (cajeta-five's
// buildtool-exe-package-name-collision): because the exe is written to
// `build/exe/<details.name>` while the project's own objects go to a
// package-mirroring tree UNDER THE SAME DIRECTORY, a `details.name` equal to
// a top-level package name makes the linker try to write a file over a
// directory. That spec guessed "the asymmetry looks unintentional and
// removing it may fix the defect outright" — separating the roles is exactly
// that removal, so a regression test for the collision lives here too.
//
// Everything is asserted by LOCATING FILES, never by reading a log line: the
// plan asks for that specifically, because a build that prints the right path
// and writes somewhere else is the failure this unit exists to prevent.
//
// What deliberately does NOT move: the `.cja` stays at
// `build/archive/<name>-<version>.cja` (already the spec's default, and
// ~10 sibling scripts glob it), and the executable stays at
// `build/exe/<name>` (documented in runtime/skills/toolchain and asserted by
// scripts/check-guide-part1.sh). Spec §3.1 names `build/bin/` for binaries;
// moving there is a rename with real breakage and no correctness gain, so it
// is left for an explicit decision rather than taken as a side effect here.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <map>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string layoutCompilerBinary() {
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

int layoutExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct LayoutProject {
    fs::path root;
    std::string name;

    ~LayoutProject() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // `entry` empty => a library project (emits .cja); otherwise an exe.
    // `outputBlock` is spliced verbatim as `settings.output` when non-empty —
    // unit 3's knob. Passed as raw JSON so a test can hand in a malformed
    // value and watch the parse reject it.
    static std::unique_ptr<LayoutProject> create(const std::string& projName,
                                                 const std::string& entry,
                                                 const std::string& pkg = "t",
                                                 const std::string& outputBlock = "") {
        static std::mt19937_64 rng(std::random_device{}());
        auto p = std::make_unique<LayoutProject>();
        p->name = projName;
        p->root = fs::temp_directory_path()
                / ("cajeta_layout_" + std::to_string(rng()));
        fs::path src = p->root / "src" / "main" / "cajeta" / pkg;
        fs::create_directories(src);

        std::ofstream(src / "Util.cajeta")
            << "package " << pkg << ";\n"
               "public final class Util {\n"
               "    public static int32 ten() { return 10; }\n"
               "}\n";
        std::ofstream(src / "Main.cajeta")
            << "package " << pkg << ";\n"
               "import " << pkg << ".Util;\n"
               "public final class Main {\n"
               "    public static int32 run() { return Util.ten(); }\n"
               "}\n";

        std::ofstream m(p->root / "cajeta.json");
        m << "{\n"
             "  \"details\": { \"name\": \"" << projName << "\","
             " \"version\": \"0.1.0\",\n"
             "                 \"cajeta-lang-version\": \"1.0\" },\n";
        if (!entry.empty() || !outputBlock.empty()) {
            m << "  \"settings\": {\n";
            if (!entry.empty()) {
                m << "    \"build\": { \"entry-method\": \"" << entry
                  << "\" }" << (outputBlock.empty() ? "\n" : ",\n");
            }
            if (!outputBlock.empty()) {
                m << "    \"output\": " << outputBlock << "\n";
            }
            m << "  },\n";
        }
        // `clean` is a TASK, not a built-in verb — every archetype ships one,
        // and without it `cajeta clean` falls through to printing help.
        m << "  \"tasks\": {\n"
             "    \"build\": { \"actions\": [\n"
             "      { \"action\": \"build\", \"flavor\": \"debug\","
             " \"id\": \"art\" } ] },\n"
             "    \"clean\": { \"actions\": [ { \"action\": \"clean\" } ] }\n"
             "  }\n"
             "}\n";
        return p;
    }

    int build(std::string& output) const {
        fs::path log = root / "out.log";
        std::string cmd = "cd " + root.string() + " && \""
            + layoutCompilerBinary() + "\" build > " + log.string() + " 2>&1";
        int rc = layoutExit(std::system(cmd.c_str()));
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
        return rc;
    }

    int clean(std::string& output) const {
        fs::path log = root / "clean.log";
        std::string cmd = "cd " + root.string() + " && \""
            + layoutCompilerBinary() + "\" clean > " + log.string() + " 2>&1";
        int rc = layoutExit(std::system(cmd.c_str()));
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
        return rc;
    }

    // Every generated intermediate found under `dir`, relative to it.
    std::vector<std::string> intermediatesUnder(const fs::path& dir) const {
        std::vector<std::string> out;
        if (!fs::exists(dir)) return out;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            if (ext == ".o" || ext == ".obj" || ext == ".bc") {
                out.push_back(fs::relative(e.path(), dir).string());
            }
        }
        return out;
    }
};

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += "\n    " + x;
    return s;
}

}  // namespace

// 2.1.1 — the executable build. Intermediates under build/obj/, and the exe
// directory holding no intermediates at all.
TEST(BuildOutputLayoutTests, executableBuildPutsObjectsUnderBuildObj) {
    auto p = LayoutProject::create("t.app", "t.Main::run");
    std::string out;
    ASSERT_EQ(0, p->build(out)) << out;

    auto inObj = p->intermediatesUnder(p->root / "build" / "obj");
    auto inExe = p->intermediatesUnder(p->root / "build" / "exe");

    EXPECT_FALSE(inObj.empty())
        << "per-class objects belong under build/obj/ (spec §3.1); found none";
    EXPECT_TRUE(inExe.empty())
        << "build/exe/ is an ARTIFACT directory and must hold no"
           " intermediates, but it holds:" << join(inExe);
    EXPECT_TRUE(fs::exists(p->root / "build" / "exe" / "t.app"))
        << "the executable itself stays at build/exe/<name> — scripts and the"
           " toolchain skill document that path";
}

// 2.1.1 — the library build. The .cja must stay exactly where consumers glob
// for it; this is the arm that fails if the artifact role is moved.
TEST(BuildOutputLayoutTests, libraryBuildLeavesTheCjaInBuildArchive) {
    auto p = LayoutProject::create("t.lib", /*entry=*/"");
    std::string out;
    ASSERT_EQ(0, p->build(out)) << out;

    fs::path cja = p->root / "build" / "archive" / "t.lib-0.1.0.cja";
    EXPECT_TRUE(fs::exists(cja))
        << "the .cja must stay at build/archive/<name>-<version>.cja:\n" << out;
    EXPECT_TRUE(p->intermediatesUnder(p->root / "build" / "archive").empty())
        << "build/archive/ is an ARTIFACT directory and must hold no"
           " intermediates";
}

// 2.1.2 — a dependency's classes are not this project's source, so their
// objects get their own subtree (spec §3.2). Driven through the compiler's
// --classpath rather than the build tool on purpose: the compiler is what
// chooses object paths, and a `path` dependency never reaches the build
// tool's classpath anyway (Dependency.cpp leaves path/git entries with no
// version constraint and "downstream resolution skips entries with no
// constraint") — a separate gap, filed, not fixed here.
//
// Before this, a dependency's object sat beside the project's own with a
// FLAT dotted name — `out/dlib.Helper.o` next to `out/app/Main.o` — the same
// asymmetry cajeta-five's collision spec noticed. `deps/<module>/` also
// matches the archive format's own nesting of dependency entries under
// `deps/<name>-<version>/`.
TEST(BuildOutputLayoutTests, dependencyObjectsLandUnderTheirOwnDepsSubtree) {
    auto p = LayoutProject::create("t.app", "t.Main::run", "app");
    // A separate library, built to a .cja, consumed via --classpath.
    fs::path libRoot = p->root / "dep";
    fs::path libSrc  = libRoot / "src" / "dlib";
    fs::create_directories(libSrc);
    fs::create_directories(libRoot / "out");
    std::ofstream(libSrc / "Helper.cajeta")
        << "package dlib;\n"
           "public final class Helper {\n"
           "    public static int32 five() { return 5; }\n"
           "}\n";
    fs::path cja = libRoot / "com.example.dlib-0.2.0.cja";
    {
        std::string cmd = "\"" + layoutCompilerBinary() + "\" --emit=cja -o \""
            + cja.string() + "\" '*' \"" + (libRoot / "src").string()
            + "\" \"" + (libRoot / "out").string() + "\" > /dev/null 2>&1";
        ASSERT_EQ(0, layoutExit(std::system(cmd.c_str())))
            << "could not build the dependency archive";
    }
    ASSERT_TRUE(fs::exists(cja));

    // The consumer calls into it, so the dependency class is really compiled.
    fs::path appSrc = p->root / "src" / "main" / "cajeta" / "app";
    std::ofstream(appSrc / "Main.cajeta")
        << "package app;\n"
           "import dlib.Helper;\n"
           "public final class Main {\n"
           "    public static int32 run() { return Helper.five(); }\n"
           "}\n";

    fs::path out = p->root / "cout";
    fs::create_directories(out);
    std::string log = (p->root / "cp.log").string();
    std::string cmd = "\"" + layoutCompilerBinary() + "\" --emit=exe"
        " --classpath=\"" + cja.string() + "\" -o \""
        + (out / "prog").string() + "\" app.Main.run \""
        + (p->root / "src" / "main" / "cajeta").string() + "\" \""
        + out.string() + "\" > " + log + " 2>&1";
    ASSERT_EQ(0, layoutExit(std::system(cmd.c_str())))
        << "classpath build failed";

    bool depUnderDeps = fs::exists(out / "deps" / "com.example.dlib"
                                       / "dlib.Helper.o");
    EXPECT_TRUE(depUnderDeps)
        << "a dependency's object belongs under deps/<module>/ (spec §3.2)";
    EXPECT_FALSE(fs::exists(out / "dlib.Helper.o"))
        << "the flat dotted object beside the project's own must be gone";
    // The negative arm: the project's OWN classes must NOT be relocated.
    EXPECT_TRUE(fs::exists(out / "app" / "Main.o"))
        << "the project's own objects keep their package tree";
}

// 2.1.3 — `clean` removes everything generated and NOTHING a human wrote.
//
// Asserted as a byte-identical source tree, not as "the sources still exist":
// a clean that truncated or rewrote a file would pass the weaker check. The
// snapshot covers content, so it also catches a clean that helpfully
// reformats something.
//
// The positive half matters just as much. `clean` that deletes nothing also
// leaves the sources untouched, so the test first proves there WAS generated
// output to remove, then that none of it survives — under `build/` and under
// `.cajeta/cache/`, which are two separate roots and were the two places the
// repo audit found committed junk.
TEST(BuildOutputLayoutTests, cleanRemovesGeneratedFilesAndNothingElse) {
    auto p = LayoutProject::create("t.app", "t.Main::run");

    // Snapshot every hand-written file: relative path -> exact bytes.
    std::map<std::string, std::string> before;
    for (const auto& e : fs::recursive_directory_iterator(p->root)) {
        if (!e.is_regular_file()) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        before[fs::relative(e.path(), p->root).string()] = ss.str();
    }
    ASSERT_FALSE(before.empty()) << "fixture wrote no files";

    std::string out;
    ASSERT_EQ(0, p->build(out)) << out;

    // The instrument check: there must be something to clean, or the
    // assertions below are vacuous.
    ASSERT_TRUE(fs::exists(p->root / "build"))
        << "nothing was generated, so this proves nothing";
    ASSERT_FALSE(p->intermediatesUnder(p->root / "build").empty())
        << "no intermediates were produced, so this proves nothing";

    std::string cleanOut;
    ASSERT_EQ(0, p->clean(cleanOut)) << cleanOut;

    // Nothing generated survives, in either root.
    EXPECT_TRUE(p->intermediatesUnder(p->root / "build").empty())
        << "clean left intermediates:"
        << join(p->intermediatesUnder(p->root / "build"));
    EXPECT_FALSE(fs::exists(p->root / "build" / "exe" / "t.app"))
        << "clean left the executable behind";
    EXPECT_FALSE(fs::exists(p->root / ".cajeta" / "cache"))
        << "clean left the compiler cache behind — the audit found 313 of"
           " these committed across four repos";

    // And every hand-written file is byte-for-byte what it was.
    for (const auto& [rel, bytes] : before) {
        fs::path f = p->root / rel;
        ASSERT_TRUE(fs::exists(f)) << "clean deleted a hand-written file: "
                                   << rel;
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        EXPECT_EQ(ss.str(), bytes) << "clean modified " << rel;
    }
}

// ─── unit 3: settings.output ──────────────────────────────────────────
//
// Spec §3.3. Four keys, all optional, all relative to the project root
// unless absolute: `root` is the one knob most projects touch, and
// `intermediates` / `artifacts` / `binaries` override it individually.
//
// `binaries` defaults to <root>/exe, NOT §3.1's `build/bin`: unit 2 kept the
// executable at build/exe/<name> because that path is documented in the
// toolchain skill and asserted by scripts/check-guide-part1.sh. The KEY is
// still `binaries`, so adopting build/bin later is a one-line default change
// rather than a new setting.

// 3.1.1 — `root` moves intermediates AND artifacts together.
TEST(BuildOutputLayoutTests, outputRootMovesIntermediatesAndArtifactsTogether) {
    auto p = LayoutProject::create("t.app", "t.Main::run", "t",
                                   R"({ "root": "out" })");
    std::string out;
    ASSERT_EQ(0, p->build(out)) << out;

    EXPECT_FALSE(p->intermediatesUnder(p->root / "out" / "obj").empty())
        << "intermediates follow output.root";
    EXPECT_TRUE(fs::exists(p->root / "out" / "exe" / "t.app"))
        << "the binary follows output.root";
    EXPECT_FALSE(fs::exists(p->root / "build"))
        << "nothing may be left at the default root once it is overridden";
}

// 3.1.2 — an explicit `artifacts` overrides `root` for ARTIFACTS ONLY, and
// intermediates stay under `root`. This is the arm that fails if the four
// keys are collapsed into one path.
TEST(BuildOutputLayoutTests, explicitArtifactsOverridesRootForArtifactsOnly) {
    auto p = LayoutProject::create(
        "t.lib", /*entry=*/"", "t",
        R"({ "root": "out", "artifacts": "dist" })");
    std::string out;
    ASSERT_EQ(0, p->build(out)) << out;

    EXPECT_TRUE(fs::exists(p->root / "dist" / "t.lib-0.1.0.cja"))
        << "the .cja follows output.artifacts:\n" << out;
    EXPECT_FALSE(fs::exists(p->root / "out" / "archive" / "t.lib-0.1.0.cja"))
        << "artifacts must NOT also land under root once overridden";
}

// 3.1.3 — a bad value fails at MANIFEST PARSE naming the offending value
// (§4.5), not at first write. Absolute paths are legal (§3.3, and 3.3.1
// wants intermediates on tmpfs), so the rejected shapes are escapes and
// non-strings — never "it starts with /".
TEST(BuildOutputLayoutTests, badOutputSettingIsRejectedAtParseNamingTheValue) {
    struct Case { const char* block; const char* needle; };
    const Case cases[] = {
        { R"({ "root": "../escape" })",   "../escape" },
        { R"({ "intermediates": "" })",   "intermediates" },
        { R"({ "artifacts": 42 })",       "artifacts" },
        { R"({ "binaries": ["a"] })",     "binaries" },
    };
    for (const auto& c : cases) {
        auto p = LayoutProject::create("t.app", "t.Main::run", "t", c.block);
        std::string out;
        EXPECT_NE(0, p->build(out)) << "accepted a bad output setting: "
                                    << c.block;
        EXPECT_NE(std::string::npos, out.find(c.needle))
            << "the diagnostic must name the offending value.\nblock: "
            << c.block << "\ngot:\n" << out;
        // Rejected at PARSE means nothing was written first.
        EXPECT_TRUE(p->intermediatesUnder(p->root).empty())
            << "a rejected manifest must not have produced output first";
    }
}

// 3.1.4 — no `output` block is byte-identical to unit 2. The negative arm:
// adding the setting must not change the default layout for the projects
// that never opt in.
TEST(BuildOutputLayoutTests, noOutputBlockKeepsTheUnitTwoDefaults) {
    auto p = LayoutProject::create("t.app", "t.Main::run");
    std::string out;
    ASSERT_EQ(0, p->build(out)) << out;

    EXPECT_FALSE(p->intermediatesUnder(p->root / "build" / "obj").empty());
    EXPECT_TRUE(p->intermediatesUnder(p->root / "build" / "exe").empty());
    EXPECT_TRUE(fs::exists(p->root / "build" / "exe" / "t.app"));
}

// The collision this separation resolves: a details.name equal to a top-level
// package name. With objects in a package tree under build/exe/, the linker
// was asked to write the file build/exe/t over the directory build/exe/t —
// "cannot open output file build/exe/t: Is a directory". With intermediates
// moved out, the two names no longer share a parent.
TEST(BuildOutputLayoutTests, projectNamedAfterItsTopLevelPackageStillLinks) {
    auto p = LayoutProject::create("t", "t.Main::run");
    std::string out;
    EXPECT_EQ(0, p->build(out))
        << "details.name == top-level package must not collide:\n" << out;
    EXPECT_EQ(std::string::npos, out.find("Is a directory")) << out;
    EXPECT_TRUE(fs::exists(p->root / "build" / "exe" / "t"));
}
