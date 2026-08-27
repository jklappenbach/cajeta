// `cajeta <entry> <sourceRoot> <outputDir>` — the compile verb's arity.
//
// The driver took `positional.size() < 3` as its only guard, so the HIGH side
// was unchecked: a fourth positional was silently discarded AND the third was
// reinterpreted. Someone reaching for a multi-source-root build --
// `cajeta ... Main.main srcA srcB out` -- got exit 0, an `out` that stayed
// empty, srcB used as the BUILD DIRECTORY (object files written into a source
// tree), and none of srcB's types compiled. The symptom read as a resolution
// bug; it was arity.
//
// Multi-tree builds are a --classpath concern: emit the first tree as a .cja,
// then compile the second against it. These tests pin the rejection, and --
// because a check that never fires reads exactly like a clean run -- pin the
// NEGATIVE arms too: the correct three-positional form must still compile, and
// the low-side usage path must be untouched.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string arityCompilerBinary() {
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

int arityExitCodeOf(int systemStatus) {
#ifdef _WIN32
    return systemStatus;
#else
    return WIFEXITED(systemStatus) ? WEXITSTATUS(systemStatus) : -1;
#endif
}

// Two source trees plus two candidate output dirs, so a misread of any
// positional is observable as a file landing in the wrong place.
struct ArityWorld {
    fs::path root;
    ArityWorld() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path()
             / ("cajeta_arity_" + std::to_string(rng()));
        fs::create_directories(root / "srcA" / "pkg");
        fs::create_directories(root / "srcB" / "pkg");
        fs::create_directories(root / "out");

        std::ofstream(root / "srcA" / "pkg" / "Main.cajeta")
            << "package pkg;\n"
               "import cajeta.lang.String;\n"
               "import cajeta.lang.System;\n"
               "public final class Main {\n"
               "    public static int32 main(String[] args) {\n"
               "        System.stdout.println(\"ok\");\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
        std::ofstream(root / "srcB" / "pkg" / "Helper.cajeta")
            << "package pkg;\n"
               "public final class Helper {\n"
               "    public static int32 answer() { return 42; }\n"
               "}\n";
    }
    ~ArityWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // Run the compiler, capturing the merged streams.
    int run(const std::string& tail, std::string& output) const {
        fs::path log = root / "run.log";
        std::string cmd = "\"" + arityCompilerBinary() + "\" " + tail
                        + " > \"" + log.string() + "\" 2>&1";
        int rc = arityExitCodeOf(std::system(cmd.c_str()));
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
        return rc;
    }

    std::string q(const fs::path& p) const {
        return "\"" + p.string() + "\"";
    }

    // Any build residue at all -- object files, package dirs -- in a tree that
    // the caller offered as SOURCE.
    bool hasBuildResidue(const fs::path& dir) const {
        for (const auto& e : fs::directory_iterator(dir)) {
            std::string n = e.path().filename().string();
            if (n.size() > 2 && n.substr(n.size() - 2) == ".o") return true;
            if (n.rfind("__cajeta", 0) == 0) return true;
        }
        return false;
    }
};

}  // namespace

// FIRES: the four-positional form is refused, and refused BEFORE anything is
// written -- the source tree offered as the second root stays clean.
TEST(PositionalArityCliTests, fourPositionalsRejected) {
    ArityWorld w;
    std::string out;
    int rc = w.run("--emit=exe -o " + w.q(w.root / "prog") + " pkg.Main.main "
                       + w.q(w.root / "srcA") + " " + w.q(w.root / "srcB")
                       + " " + w.q(w.root / "out"),
                   out);

    EXPECT_NE(0, rc) << "a 4-positional compile must fail, not silently "
                        "discard an argument; output:\n" << out;
    // The diagnostic has to name the arity, or the user re-reads it as the
    // resolution failure it used to masquerade as.
    EXPECT_NE(std::string::npos, out.find("positional"))
        << "the rejection must say which arguments are at fault:\n" << out;

    EXPECT_FALSE(w.hasBuildResidue(w.root / "srcB"))
        << "srcB was offered as a source root and must never be written to";
    EXPECT_FALSE(fs::exists(w.root / "prog"))
        << "no binary may be produced from a rejected command line";
}

// FIRES: the message must point at the actual mechanism for multi-tree builds,
// not merely complain. Without this the user's next move is another guess.
TEST(PositionalArityCliTests, rejectionNamesTheClasspathRemedy) {
    ArityWorld w;
    std::string out;
    w.run("--emit=exe -o " + w.q(w.root / "prog") + " pkg.Main.main "
              + w.q(w.root / "srcA") + " " + w.q(w.root / "srcB")
              + " " + w.q(w.root / "out"),
          out);
    EXPECT_NE(std::string::npos, out.find("--classpath"))
        << "multi-root is a --classpath workflow; say so:\n" << out;
}

// DOES NOT FIRE: the correct arity still compiles. A guard that rejected the
// valid form would read, from the suite alone, exactly like a working fix.
TEST(PositionalArityCliTests, threePositionalsStillCompile) {
    ArityWorld w;
    std::string out;
    int rc = w.run("--emit=exe -o " + w.q(w.root / "prog") + " pkg.Main.main "
                       + w.q(w.root / "srcA") + " " + w.q(w.root / "out"),
                   out);
    EXPECT_EQ(0, rc) << "the documented 3-positional form must be unaffected;"
                        " output:\n" << out;
    EXPECT_TRUE(fs::exists(w.root / "prog"))
        << "the valid form must still produce its binary";
}

// DOES NOT FIRE on the low side: too FEW positionals keeps its usage path.
TEST(PositionalArityCliTests, twoPositionalsStillPrintUsage) {
    ArityWorld w;
    std::string out;
    int rc = w.run("--emit=exe pkg.Main.main " + w.q(w.root / "srcA"), out);
    EXPECT_NE(0, rc);
    EXPECT_NE(std::string::npos, out.find("Usage"))
        << "the under-supplied form still prints usage:\n" << out;
}
