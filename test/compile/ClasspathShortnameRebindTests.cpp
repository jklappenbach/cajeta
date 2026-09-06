// classpath-signature-shortname-rebind — cross-package short-name capture
// in the classpath ingest (specs/classpath-signature-shortname-rebind-spec.md).
//
// The shape that bit cajeta-xgboost (2026-07-31, v0.12.1): two classpath
// archives declare same-SHORT-NAME classes in different packages, and a
// signature in one archive re-resolved its own-package formal to the OTHER
// package's class — `Split.crossValScore(Predictor, …)` re-bound to
// `dev.cajeta.xgboost.predict.Predictor`, a candidate list impossible from
// the source.
//
// Root cause (2026-08-04): placeholder-fill short-name capture, not the
// resolver tiers. Ingredients, all required:
//   1. `liba.Split`'s formal names same-package `Walker`, and Split sorts
//      BEFORE Walker in the archive — so at Split's parse the formal is a
//      forward reference.
//   2. An EARLIER-parsed archive entry imported `liba.Walker` (libb.XGB
//      implements it), synthesizing a correct placeholder registered under
//      BOTH `liba.Walker` and the bare short key `Walker`.
//   3. The decoy `libb.predict.Walker` then declared: the placeholder-reuse
//      lookup missed its canonical, fell back to the SHORT key, and
//      fillFromDeclaration mutated the shared_ptr — so canonicalMap
//      ["liba.Walker"] BECAME the decoy, and Split's own-package tier-1
//      lookup "hit" the wrong class.
// The fix guards every placeholder-fill short-name fallback: a short-key
// hit is only accepted when the placeholder's recorded canonical equals the
// declaring class's.
//
// Spec 2.2: both entry orders, constructed deliberately (v0.14.1 made
// archive entry order alphabetical, so the orders are set by naming and
// classpath sequence, not by rebuild luck).

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
              / ("cajeta_rebind_" + tag + "_" + std::to_string(rng()));
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

struct Fixture {
    fs::path root;
    fs::path libaCja, libbCja, appSrc;

    static Fixture build() {
        Fixture f{freshTempDir("libs")};
        // liba: interface Walker + Split whose STATIC method's formal names
        // it unqualified. `Split` < `Walker` alphabetically, so within the
        // archive the signature parses before the interface — the forward
        // reference the defect needs.
        writeFile(f.root / "liba/src/liba/Walker.cajeta",
            "package liba;\n"
            "public interface Walker {\n"
            "    int32 kind();\n"
            "}\n");
        writeFile(f.root / "liba/src/liba/Split.cajeta",
            "package liba;\n"
            "public class Split {\n"
            "    public static int32 crossValScore(Walker w, int32 k) {\n"
            "        return w.kind() + k;\n"
            "    }\n"
            "}\n");
        // libb: the DECOY (same short name, another package) plus a
        // conformer of liba.Walker — the import is what synthesizes the
        // capturable placeholder before the decoy declares. `XGB` <
        // `predict/Walker` in entry order, matching the failing sequence.
        writeFile(f.root / "libb/src/libb/predict/Walker.cajeta",
            "package libb.predict;\n"
            "public class Walker {\n"
            "    public int32 unrelated() { return 100; }\n"
            "}\n");
        writeFile(f.root / "libb/src/libb/XGB.cajeta",
            "package libb;\n"
            "import liba.Walker;\n"
            "public class XGB implements Walker {\n"
            "    public int32 kind() { return 1; }\n"
            "}\n");
        // Consumer: never names Walker — the wrong bind must be caught by
        // the CALL matching against Split's stored signature, exactly the
        // xgboost NO_MATCHING_OVERLOAD shape.
        writeFile(f.root / "app/src/app/Main.cajeta",
            "package app;\n"
            "import liba.Split;\n"
            "import libb.XGB;\n"
            "public final class Main {\n"
            "    public static int32 run() {\n"
            "        XGB x = heap XGB();\n"
            "        return Split.crossValScore(x, 5);\n"
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
        if (cja("liba.Split", "liba/src", "") != 0) return f;
        if (cja("libb.XGB", "libb/src",
                " --classpath=" + (out / "liba.cja").string()) != 0) return f;
        f.libaCja = out / "liba.cja";
        f.libbCja = out / "libb.cja";
        f.appSrc = f.root / "app/src";
        return f;
    }

    bool ok() const {
        return !libaCja.empty() && fs::exists(libaCja) && fs::exists(libbCja);
    }

    // Compile the consumer with the given classpath order, run it, and
    // return the process exit code (kind()+5 == 6 when the bind is right);
    // -1 for a failed compile.
    int compileAndRun(const std::string& tag, bool decoyFirst) const {
        auto outDir = root / ("app-" + tag);
        std::string cp = decoyFirst
            ? libbCja.string() + "," + libaCja.string()
            : libaCja.string() + "," + libbCja.string();
        auto log = root / (tag + ".log");
        std::string cmd = compilerBinary() + " app.Main::run "
            + appSrc.string() + " " + outDir.string()
            + " --emit=exe --classpath=" + cp
            + " > " + log.string() + " 2>&1";
        if (exitCodeOf(std::system(cmd.c_str())) != 0) return -1;
        std::string run = (outDir / "a.out").string() + " > " CAJETA_PORTABLE_DEVNULL " 2>&1";
        return exitCodeOf(std::system(run.c_str()));
    }

    std::string log(const std::string& tag) const {
        return readAll(root / (tag + ".log"));
    }
};

} // namespace

// Spec 2.1/2.2 — the formal must bind liba.Walker in BOTH orders. Before
// the placeholder-fill guard, the decoy-first order failed the compile with
//   CAJETA_ERROR_NO_MATCHING_OVERLOAD ... crossValScore(libb.predict.Walker, int32)
TEST(ClasspathShortnameRebind, SignatureBindsDeclaringPackageBothOrders) {
    if (!fs::exists(compilerBinary()))
        GTEST_SKIP() << "compiler binary unavailable";
    Fixture f = Fixture::build();
    ASSERT_TRUE(f.ok()) << readAll(f.root / "lib.log");

    EXPECT_EQ(f.compileAndRun("ownFirst", /*decoyFirst=*/false), 6)
        << f.log("ownFirst");
    EXPECT_EQ(f.compileAndRun("decoyFirst", /*decoyFirst=*/true), 6)
        << f.log("decoyFirst");
}
