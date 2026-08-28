// The compile verb must refuse to write generated files into a source tree.
//
// build-output-layout-spec §4.1. On 2026-08-27 a compile handed two source
// roots bound the SECOND to the output directory and wrote object files into
// it — exit 0, no diagnostic — depositing 180 objects in cajeta-cabra/src and
// 75 in cajeta-llm/src, both then swept into git by a routine `git add -A`.
// The arity that let it happen is rejected now (PositionalArityCliTests), but
// that closes ONE way in. Since §3.4 settled that output destinations are the
// BUILD TOOL's concern and the compiler keeps its bare positional, a script
// invoking `cajeta` directly gets nothing from the layout — this guard is the
// entirety of its protection.
//
// SHAPE OF THE RULE. The spec's first wording was "reject an output directory
// that is, contains, or is contained by the source root". Writing the tests
// showed that over-fires on an ordinary and correct invocation: source root
// `.` with output `./build` has the output CONTAINED BY the source root, yet
// nothing is polluted — sources live in ./src and artifacts in ./build. So
// the rule keys on the actual hazard instead: refuse an output directory that
// CONTAINS CAJETA SOURCES. That catches every real case (output == the source
// root, or any directory of .cajeta files) and cannot fire on a build
// directory, which by definition holds no sources.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string guardCompilerBinary() {
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

int guardExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct GuardWorld {
    fs::path root;
    GuardWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_outguard_" + std::to_string(rng()));
        fs::create_directories(root / "src" / "pkg");
        fs::create_directories(root / "build");
        // A second layout for the nested-output case: a source root that
        // holds its package directly, with a sources-free `out/` beneath it.
        fs::create_directories(root / "flat" / "pkg");
        fs::create_directories(root / "flat" / "out");
        std::ofstream(root / "flat" / "pkg" / "Main.cajeta")
            << "package pkg;\n"
               "import cajeta.lang.String;\n"
               "import cajeta.lang.System;\n"
               "public final class Main {\n"
               "    public static int32 main(String[] args) {\n"
               "        System.stdout.println(\"ok\");\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
        std::ofstream(root / "src" / "pkg" / "Main.cajeta")
            << "package pkg;\n"
               "import cajeta.lang.String;\n"
               "import cajeta.lang.System;\n"
               "public final class Main {\n"
               "    public static int32 main(String[] args) {\n"
               "        System.stdout.println(\"ok\");\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
    }
    ~GuardWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    int run(const fs::path& srcRoot, const fs::path& outDir,
            std::string& output) const {
        fs::path log = root / "run.log";
        std::string cmd = "\"" + guardCompilerBinary() + "\" --emit=exe -o \""
            + (root / "prog").string() + "\" pkg.Main.main \""
            + srcRoot.string() + "\" \"" + outDir.string() + "\" > \""
            + log.string() + "\" 2>&1";
        int rc = guardExit(std::system(cmd.c_str()));
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
        return rc;
    }

    bool hasResidue(const fs::path& dir) const {
        if (!fs::exists(dir)) return false;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            std::string n = e.path().filename().string();
            if (n.size() > 2 && n.substr(n.size() - 2) == ".o") return true;
            if (n.rfind("__cajeta", 0) == 0) return true;
        }
        return false;
    }
};

}  // namespace

// FIRES: output dir IS the source root. The exact shape that filled two
// repos with object files.
TEST(OutputDirGuardCliTests, outputIntoTheSourceRootIsRefused) {
    GuardWorld w;
    std::string out;
    int rc = w.run(w.root / "src", w.root / "src", out);

    EXPECT_NE(0, rc) << "writing output into a source tree must fail:\n" << out;
    EXPECT_NE(std::string::npos, out.find("source"))
        << "the diagnostic must say why:\n" << out;
    EXPECT_FALSE(w.hasResidue(w.root / "src"))
        << "refused BEFORE writing — the source tree must stay clean";
}

// FIRES: a nested directory that holds sources is still a source tree.
TEST(OutputDirGuardCliTests, outputIntoANestedSourceDirIsRefused) {
    GuardWorld w;
    std::string out;
    int rc = w.run(w.root / "src", w.root / "src" / "pkg", out);
    EXPECT_NE(0, rc) << "a directory of .cajeta files is a source tree:\n"
                     << out;
    EXPECT_FALSE(w.hasResidue(w.root / "src"));
}

// DOES NOT FIRE: the ordinary sibling layout still builds. A guard that
// rejected this would break every project.
TEST(OutputDirGuardCliTests, siblingBuildDirStillCompiles) {
    GuardWorld w;
    std::string out;
    int rc = w.run(w.root / "src", w.root / "build", out);
    EXPECT_EQ(0, rc) << "src -> build must be unaffected:\n" << out;
    EXPECT_TRUE(fs::exists(w.root / "prog"));
}

// DOES NOT FIRE, and this is the case that reshaped the rule: an output
// directory NESTED INSIDE the source root, holding no sources. Source root
// `.` with output `./build` is ordinary and correct; a containment-based
// rule would have rejected it.
TEST(OutputDirGuardCliTests, buildDirNestedInsideTheSourceRootIsAllowed) {
    GuardWorld w;
    std::string out;
    // Source root `flat/` holds pkg/Main.cajeta; output `flat/out/` sits
    // INSIDE it and holds no sources. A containment-based rule rejects this;
    // the contains-sources rule must not.
    int rc = w.run(w.root / "flat", w.root / "flat" / "out", out);
    EXPECT_EQ(0, rc)
        << "a sources-free output dir under the source root is legitimate:\n"
        << out;
}
