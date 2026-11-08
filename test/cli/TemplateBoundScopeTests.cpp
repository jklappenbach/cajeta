// A template bound `<S extends B>` names the B its own file sees: the one in its package,
// or the one it imports. Two libraries that each declare a `TileShape` and a
// `TileFamily<S extends TileShape>` (cajeta-llm and dev.cajeta.ml.kernels, 2026-11-08)
// compiled together resolved llm's bound to the kernel package's interface by short
// name, and refused llm's own shape (cajeta-ml-gpu 32).

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string scopeCompilerBinary() {
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

int scopeExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Builds `files` (paths relative to the source root) and runs app.Main.main: its exit
// code, or -100 when the build failed. `output` gets both logs.
int buildAndRun(const std::map<std::string, std::string>& files, std::string& output) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_tplscope_" + std::to_string(rng()));
    for (const auto& [name, text] : files) {
        fs::path p = root / "src" / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::create_directories(root / "out");
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + scopeCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = scopeExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int result = -100;
    if (rc == 0) {
        fs::path runLog = root / "run.log";
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string()
            + "\" 2>&1";
        result = scopeExit(std::system(cajeta_shell(run).c_str()));
        std::ifstream in(runLog);
        output.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return result;
}

// Packages aaa and zzz each declare a Shape and a Wshape; whichever a short-name lookup
// finds first or last, it is not app's own Shape nor the Wshape app imports from lib.
std::map<std::string, std::string> twins() {
    std::map<std::string, std::string> files;
    for (const char* pkg : {"aaa", "zzz"}) {
        std::string p = pkg;
        files[p + "/Shape.cajeta"] =
            "package " + p + ";\npublic interface Shape { int32 other(); }\n";
        files[p + "/Wshape.cajeta"] =
            "package " + p + ";\npublic interface Wshape { int32 other(); }\n";
    }
    files["lib/Wshape.cajeta"] = "package lib;\npublic interface Wshape { int32 rows(); }\n";
    files["app/Shape.cajeta"] = "package app;\npublic interface Shape { int32 size(); }\n";
    files["app/Fam.cajeta"] =
        "package app;\n"
        "public final class Fam<S extends Shape> {\n"
        "    public Fam() { return; }\n"
        "    public int32 of(S s) { return s.size(); }\n"
        "}\n";
    files["app/Wfam.cajeta"] =
        "package app;\n"
        "import lib.Wshape;\n"
        "public final class Wfam<S extends Wshape> {\n"
        "    public Wfam() { return; }\n"
        "    public int32 of(S s) { return s.rows(); }\n"
        "}\n";
    files["app/Impl.cajeta"] =
        "package app;\n"
        "import lib.Wshape;\n"
        "public final class Impl implements Shape, Wshape {\n"
        "    public Impl() { return; }\n"
        "    public int32 size() { return 4; }\n"
        "    public int32 rows() { return 9; }\n"
        "}\n";
    files["app/Main.cajeta"] =
        "package app;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.lang.System;\n"
        "public class Main {\n"
        "    public static int32 main(String[] args) {\n"
        "        Fam<Impl> f = heap Fam<Impl>();\n"
        "        Wfam<Impl> w = heap Wfam<Impl>();\n"
        "        Impl i = heap Impl();\n"
        "        System.stdout.println(\"go \" + (f.of(i) * 10 + w.of(i)));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    return files;
}

}  // namespace

TEST(TemplateBoundScopeTests, aBoundResolvesInItsOwnPackageAndImportsNotByShortName) {
    std::string out;
    int rc = buildAndRun(twins(), out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("go 49\n")) << out;
}

TEST(TemplateBoundScopeTests, aShapeOfAnotherPackagesTwinIsStillRejected) {
    auto files = twins();
    files["app/Other.cajeta"] =
        "package app;\n"
        "public final class Other implements aaa.Shape {\n"
        "    public Other() { return; }\n"
        "    public int32 other() { return 1; }\n"
        "}\n";
    files["app/Main.cajeta"] =
        "package app;\n"
        "import cajeta.lang.String;\n"
        "public class Main {\n"
        "    public static int32 main(String[] args) {\n"
        "        Fam<Other> f = heap Fam<Other>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    std::string out;
    int rc = buildAndRun(files, out);
    EXPECT_EQ(-100, rc) << out;
    EXPECT_NE(std::string::npos, out.find("CAJETA_ERROR_TYPE_PARAMETER_BOUND")) << out;
    EXPECT_NE(std::string::npos, out.find("argument 'app.Other' does not satisfy bound")) << out;
}

// Impl.cajeta parses before Shape.cajeta, and aaa.Shape is already registered: the
// qualified name must still mean app.Shape, not the twin a short-name scan finds first.
TEST(TemplateBoundScopeTests, aQualifiedImplementsNamesItsPackageNotATwinParsedEarlier) {
    auto files = twins();
    files["app/Impl.cajeta"] =
        "package app;\n"
        "import lib.Wshape;\n"
        "public final class Impl implements app.Shape, Wshape {\n"
        "    public Impl() { return; }\n"
        "    public int32 size() { return 4; }\n"
        "    public int32 rows() { return 9; }\n"
        "}\n";
    std::string out;
    int rc = buildAndRun(files, out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("go 49\n")) << out;
}
