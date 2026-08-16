// Class.allClasses keep-all defect — the two halves of the fix, end-to-end
// against the built binary:
//
//   1. `classesWithMethodAnnotated` is a BOUNDED registry enumerator: with a
//      literal (or token) selector the lean linker keeps exactly the classes
//      declaring a matching method, instead of the keep-all that every
//      `allClasses()` + per-method-filter discovery loop (cajeta-unit's
//      Runner) used to force.
//
//   2. A dependency's reflection sites now travel with its archive
//      (`meta/reflection-keep.v1`, written at the dep's own build) and merge
//      into the consumer's accumulator at classpath ingest. Without that, a
//      dep-internal reflection call is INVISIBLE to the consumer's keep-set
//      computation — classpath ingest is signature-only — and a lean link
//      would silently strip the classes the dep enumerates at runtime.
//      Bounded dep sites keep narrowly; unbounded ones degrade to keep-all
//      with the warning attributed to the dep.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

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

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_reflkeep_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << body;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The user program every test compiles lean: an annotation, a suite class
// whose METHOD carries it, a decoy that carries nothing, and a main. `body`
// supplies main's statements.
std::string userProgram(const std::string& mainBody,
                        const std::string& extraImports = "") {
    return
        "package demo;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        + extraImports +
        "annotation Probe { }\n"
        "public class Suite {\n"
        "    public Suite() { return; }\n"
        "    @Probe public int32 f() { return 3; }\n"
        "}\n"
        "public class Decoy {\n"
        "    public Decoy() { return; }\n"
        "    public int32 f() { return 4; }\n"
        "}\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        + mainBody +
        "    }\n"
        "}\n";
}

struct Compiled {
    int exit;
    std::string log;      // combined stdout+stderr
    std::string keepset;  // keepset json, when requested
};

Compiled compileLean(const fs::path& root, const std::string& srcRel,
                     const std::string& outRel, const std::string& extra) {
    auto log = root / (outRel + ".log");
    auto ks = root / (outRel + ".keepset.json");
    std::string cmd = compilerBinary() + " demo.Main::run "
        + (root / srcRel).string() + " " + (root / outRel).string()
        + " --emit=ir --link-mode=lean --keepset-json=" + ks.string()
        + " " + extra + " > " + log.string() + " 2>&1";
    Compiled c;
    c.exit = exitCodeOf(std::system(cmd.c_str()));
    c.log = readAll(log);
    c.keepset = readAll(ks);
    return c;
}

} // namespace

// Half 1 — the bounded enumerator narrows the keep-set: Suite (a @Probe
// method) is kept with the site as provenance, Decoy is stripped, and no
// forces-keep-all warning fires.
TEST(ReflectionKeepMethodAnnotated, BoundedEnumeratorNarrowsTheKeepSet) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto root = freshTempDir("bounded");
    writeFile(root / "src/demo/Main.cajeta", userProgram(
        "        Class<?>[] hits #= Class.classesWithMethodAnnotated(\"code.Probe\");\n"
        "        return (int32) hits.count();\n"));

    Compiled c = compileLean(root, "src", "out", "");
    ASSERT_EQ(c.exit, 0) << c.log;
    EXPECT_FALSE(contains(c.log, "reflection-forces-keep-all")) << c.log;
    EXPECT_TRUE(contains(c.keepset, "demo.Suite")) << c.keepset;
    EXPECT_TRUE(contains(c.keepset, "classesWithMethodAnnotated(@Probe)"))
        << c.keepset;
    EXPECT_FALSE(contains(c.keepset, "demo.Decoy")) << c.keepset;

    fs::remove_all(root);
}

// Control — allClasses() still degrades to keep-all, loudly.
TEST(ReflectionKeepMethodAnnotated, AllClassesStillForcesKeepAll) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto root = freshTempDir("forceall");
    writeFile(root / "src/demo/Main.cajeta", userProgram(
        "        Class<?>[] all #= Class.allClasses();\n"
        "        return (int32) all.count();\n"));

    Compiled c = compileLean(root, "src", "out", "");
    ASSERT_EQ(c.exit, 0) << c.log;
    EXPECT_TRUE(contains(c.log, "reflection-forces-keep-all")) << c.log;

    fs::remove_all(root);
}

namespace {

// Build a one-class library whose method reflects, emit it as a .cja, and
// return the archive path (empty on failure).
fs::path buildReflectingDep(const fs::path& root, const std::string& reflExpr) {
    writeFile(root / "dep/src/depd/Disco.cajeta",
        "package depd;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "public class Disco {\n"
        "    public static int32 scan() {\n"
        "        Class<?>[] hits = " + reflExpr + ";\n"
        "        return (int32) hits.count();\n"
        "    }\n"
        "}\n");
    auto out = root / "depout";
    fs::create_directories(out);
    auto log = root / "dep.log";
    std::string cmd = compilerBinary() + " depd.Disco "
        + (root / "dep/src").string() + " " + out.string()
        + " --emit=cja > " + log.string() + " 2>&1";
    if (exitCodeOf(std::system(cmd.c_str())) != 0) return {};
    return out / "depd.cja";
}

} // namespace

// Half 2a — an UNBOUNDED reflection site inside a dependency reaches the
// consumer's keep-set computation through the archive summary: the consumer
// has no reflection of its own, yet the build degrades to keep-all and the
// warning names the dependency. Before the summary, this build was silently
// lean — and the dep's runtime enumeration found stripped registries.
TEST(ReflectionKeepMethodAnnotated, DepAllClassesReachesTheConsumerLoudly) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto root = freshTempDir("depwide");
    auto cja = buildReflectingDep(root, "Class.allClasses()");
    ASSERT_FALSE(cja.empty()) << readAll(root / "dep.log");

    writeFile(root / "src/demo/Main.cajeta", userProgram(
        "        return Disco.scan();\n",
        "import depd.Disco;\n"));

    Compiled c = compileLean(root, "src", "out",
                             "--classpath=" + cja.string());
    ASSERT_EQ(c.exit, 0) << c.log;
    EXPECT_TRUE(contains(c.log, "reflection-forces-keep-all")) << c.log;
    EXPECT_TRUE(contains(c.log, "dependency")) << c.log;

    fs::remove_all(root);
}

// Half 2b — a BOUNDED dep site keeps narrowly: the dep's
// classesWithMethodAnnotated("code.Probe") summary keeps the consumer's
// Suite (its @Probe method) and strips Decoy, with no keep-all warning.
TEST(ReflectionKeepMethodAnnotated, DepBoundedSiteKeepsNarrow) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    auto root = freshTempDir("depnarrow");
    auto cja = buildReflectingDep(
        root, "Class.classesWithMethodAnnotated(\"code.Probe\")");
    ASSERT_FALSE(cja.empty()) << readAll(root / "dep.log");

    writeFile(root / "src/demo/Main.cajeta", userProgram(
        "        return Disco.scan();\n",
        "import depd.Disco;\n"));

    Compiled c = compileLean(root, "src", "out",
                             "--classpath=" + cja.string());
    ASSERT_EQ(c.exit, 0) << c.log;
    EXPECT_FALSE(contains(c.log, "reflection-forces-keep-all")) << c.log;
    EXPECT_TRUE(contains(c.keepset, "demo.Suite")) << c.keepset;
    EXPECT_TRUE(contains(c.keepset, "classesWithMethodAnnotated(@Probe)"))
        << c.keepset;
    EXPECT_FALSE(contains(c.keepset, "demo.Decoy")) << c.keepset;

    fs::remove_all(root);
}
