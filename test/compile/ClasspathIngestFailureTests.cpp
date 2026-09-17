// A --classpath entry the ingest cannot use is reported, never fatal on the
// lint paths. Regression: an escaping cajeta::Exception ended the run in
// SIGABRT with the reason swallowed by the signal handler (2026-09-15).

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_cpingest_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// std::system runs /bin/sh -c, so a signal-killed child surfaces as a NORMAL
// shell exit of 128+signo; WIFSIGNALED never fires here.
int exitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

constexpr int kShellAbort = 128 + 6;   // SIGABRT through /bin/sh

::testing::AssertionResult DidNotCrash(int rc, const std::string& out) {
    if (rc == -1 || rc == kShellAbort || out.find("SIGABRT") != std::string::npos)
        return ::testing::AssertionFailure()
               << "process crashed (rc=" << rc << "):\n" << out;
    return ::testing::AssertionSuccess();
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << body;
}

struct Result { int exit; std::string out; };

Result runLint(const fs::path& root, const std::string& srcRoot,
               const std::string& classpathArg, const fs::path& xrefOut) {
    auto log = root / "lint.log";
    std::string cmd = compilerBinary() + " --lint " + (root / srcRoot).string()
        + " --diag-format=json --emit-xref=" + xrefOut.string()
        + classpathArg + " > " + log.string() + " 2>&1";
    int rc = exitCodeOf(std::system(cmd.c_str()));
    return {rc, readAll(log)};
}

/// Two archives built separately, so `lib.cja` alone is an incomplete classpath.
struct Fixture {
    fs::path root, depCja, libCja;
    bool ok = false;

    static Fixture build() {
        Fixture f{freshTempDir("libs")};
        writeFile(f.root / "dep/src/dep/Payload.cajeta",
            "package dep;\n"
            "public class Payload {\n"
            "    public int32 size() { return 7; }\n"
            "}\n");
        writeFile(f.root / "lib/src/lib/Holder.cajeta",
            "package lib;\n"
            "import dep.Payload;\n"
            "public class Holder {\n"
            "    Payload p;\n"
            "    public int32 total() { return 1; }\n"
            "}\n");
        writeFile(f.root / "app/src/app/Use.cajeta",
            "package app;\n"
            "import lib.Holder;\n"
            "public class Use {\n"
            "    public static int32 run() {\n"
            "        Holder h = heap Holder();\n"
            "        return h.total();\n"
            "    }\n"
            "}\n");

        auto out = f.root / "out";
        fs::create_directories(out);
        auto cja = [&](const std::string& entry, const std::string& srcDir,
                       const std::string& extra) {
            std::string cmd = compilerBinary() + " " + entry + " "
                + (f.root / srcDir).string() + " " + out.string()
                + " --emit=cja" + extra + " > "
                + (f.root / "lib.log").string() + " 2>&1";
            return exitCodeOf(std::system(cmd.c_str()));
        };
        if (cja("dep.Payload", "dep/src", "") != 0) return f;
        if (cja("lib.Holder", "lib/src",
                " --classpath=" + (out / "dep.cja").string()) != 0) return f;
        f.depCja = out / "dep.cja";
        f.libCja = out / "lib.cja";
        f.ok = fs::exists(f.depCja) && fs::exists(f.libCja);
        return f;
    }
};

}  // namespace

// Was SIGABRT; the contract is a named diagnostic and a live process.
TEST(ClasspathIngestFailure, AnUnresolvableTypeInAnArchiveIsReportedNotFatal) {
    auto f = Fixture::build();
    ASSERT_TRUE(f.ok) << "fixture archives were not built";

    auto xref = f.root / "incomplete.json";
    auto r = runLint(f.root, "app/src", " --classpath=" + f.libCja.string(), xref);

    EXPECT_TRUE(DidNotCrash(r.exit, r.out));
    EXPECT_NE(0, r.exit) << "...and the failure must still reach the exit status";
    EXPECT_NE(std::string::npos, r.out.find("CAJETA_ERROR_UNKNOWN_TYPE"))
        << "the diagnostic must name what could not be resolved:\n" << r.out;
    EXPECT_NE(std::string::npos, r.out.find("Payload"))
        << "...and which type it was:\n" << r.out;
    EXPECT_TRUE(fs::exists(xref))
        << "a partial export beats no export — the IDE builds its shard from this";
}

// Control: the same fixture with a complete classpath must stay clean.
TEST(ClasspathIngestFailure, TheCompleteClasspathIsStillClean) {
    auto f = Fixture::build();
    ASSERT_TRUE(f.ok) << "fixture archives were not built";

    auto xref = f.root / "complete.json";
    auto r = runLint(f.root, "app/src",
                     " --classpath=" + f.depCja.string() + "," + f.libCja.string(),
                     xref);

    EXPECT_EQ(0, r.exit) << "a complete classpath must lint clean:\n" << r.out;
    EXPECT_EQ(std::string::npos, r.out.find("CAJETA_ERROR_UNKNOWN_TYPE")) << r.out;
    EXPECT_TRUE(fs::exists(xref));
}

// Control: a build must FAIL on an incomplete classpath, and already did —
// main wrapped compile() while calling lintRoot bare. Pins that it still does.
TEST(ClasspathIngestFailure, ABuildWithAnIncompleteClasspathFailsCleanly) {
    auto f = Fixture::build();
    ASSERT_TRUE(f.ok) << "fixture archives were not built";

    auto out = f.root / "buildout";
    fs::create_directories(out);
    auto log = f.root / "build.log";
    std::string cmd = compilerBinary() + " app.Use " + (f.root / "app/src").string()
        + " " + out.string() + " --emit=cja --diag-format=json"
        + " --classpath=" + f.libCja.string()          // lib without its dep
        + " > " + log.string() + " 2>&1";
    int rc = exitCodeOf(std::system(cmd.c_str()));
    std::string text = readAll(log);

    EXPECT_TRUE(DidNotCrash(rc, text));
    EXPECT_NE(0, rc) << "...and an incomplete classpath must FAIL the build, "
                        "never emit a quietly incomplete artifact:\n" << text;
    EXPECT_NE(std::string::npos, text.find("CAJETA_ERROR"))
        << "the failure must be named on the diagnostic channel:\n" << text;
}

// A path that is not an archive: reported in the established form, not fatal.
TEST(ClasspathIngestFailure, AnUnreadableEntryIsReportedNotFatal) {
    auto f = Fixture::build();
    ASSERT_TRUE(f.ok) << "fixture archives were not built";

    auto junk = f.root / "notanarchive.cja";
    writeFile(junk, "this is not a cja\n");

    for (const auto& bad : {junk.string(), (f.root / "absent.cja").string()}) {
        auto xref = f.root / "unreadable.json";
        auto r = runLint(f.root, "app/src", " --classpath=" + bad, xref);
        EXPECT_TRUE(DidNotCrash(r.exit, r.out)) << "for " << bad;
        EXPECT_NE(0, r.exit) << "must still fail for " << bad;
        // compiler-jsonl 3.1.1: a levelled kind:"log" record, not a diagnostic.
        EXPECT_NE(std::string::npos, r.out.find("--classpath read failed"))
            << "the entry must be reported in the established form for " << bad
            << ":\n" << r.out;
        EXPECT_NE(std::string::npos, r.out.find("\"kind\":\"log\""))
            << "under --diag-format=json it must stay a levelled log record for "
            << bad << ":\n" << r.out;
    }
}
