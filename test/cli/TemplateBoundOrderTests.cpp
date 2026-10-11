// A template bound `<S extends B>` holds for any argument that reaches B through extends
// or implements, whichever file declares B. v0.40.0 rejected a bound whose file sorts after
// the instantiating one ("did not resolve to a class") and a class implementing the bound.

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

std::string boundCompilerBinary() {
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

int boundExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Builds `files` under package app and runs app.Main.main: its exit code, or -100 when
// the build failed. `output` gets both logs.
int buildAndRun(const std::map<std::string, std::string>& files, std::string& output) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_tplbound_" + std::to_string(rng()));
    for (const auto& [name, text] : files) {
        fs::path p = root / "src" / "app" / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::create_directories(root / "out");
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + boundCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = boundExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int result = -100;
    if (rc == 0) {
        fs::path runLog = root / "run.log";
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string()
            + "\" 2>&1";
        result = boundExit(std::system(cajeta_shell(run).c_str()));
        std::ifstream in(runLog);
        output.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return result;
}

// The bound interface is named `bound`, so its file sorts where the test wants it.
std::map<std::string, std::string> world(const std::string& bound, const std::string& userFile,
                                         const std::string& userBody) {
    std::string user = userFile.substr(0, userFile.find('.'));
    return {
        {"Fam.cajeta",
            "package app;\n"
            "public final class Fam<S extends " + bound + "> {\n"
            "    public Fam() { return; }\n"
            "    public int32 of(S s) { return s.size(); }\n"
            "}\n"},
        {bound + ".cajeta", "package app;\npublic interface " + bound + " { int32 size(); }\n"},
        {"Wshape.cajeta",
            "package app;\npublic interface Wshape extends " + bound + " { int32 rows(); }\n"},
        {"Impl.cajeta",
            "package app;\npublic final class Impl implements Wshape {\n"
            "    public Impl() { return; }\n"
            "    public int32 size() { return 4; }\n"
            "    public int32 rows() { return 9; }\n"
            "}\n"},
        {"Flat.cajeta",
            "package app;\npublic final class Flat implements " + bound + " {\n"
            "    public Flat() { return; }\n"
            "    public int32 size() { return 3; }\n"
            "}\n"},
        {userFile,
            "package app;\n"
            "public final class " + user + " {\n"
            "    public static int32 go() {\n" + userBody + "    }\n"
            "}\n"},
        {"Main.cajeta",
            "package app;\n"
            "import cajeta.lang.String;\n"
            "import cajeta.lang.System;\n"
            "public class Main {\n"
            "    public static int32 main(String[] args) {\n"
            "        System.stdout.println(\"go \" + " + user + ".go());\n"
            "        return 0;\n"
            "    }\n"
            "}\n"}};
}

const char* kEveryShape =
    "        Fam<Wshape> fw = heap Fam<Wshape>();\n"
    "        Wshape w = heap Impl();\n"
    "        Fam<Flat> ff = heap Fam<Flat>();\n"
    "        Flat x = heap Flat();\n"
    "        Fam<Impl> fi = heap Fam<Impl>();\n"
    "        Impl i = heap Impl();\n"
    "        return fw.of(w) * 100 + ff.of(x) * 10 + fi.of(i);\n";

void expectEveryShape(const std::string& bound, const std::string& userFile) {
    std::string out;
    int rc = buildAndRun(world(bound, userFile, kEveryShape), out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("go 434\n")) << out;
}

void expectRejected(const std::string& bound, const std::string& userFile,
                    const std::string& body, const std::string& message) {
    std::string out;
    int rc = buildAndRun(world(bound, userFile, body), out);
    EXPECT_EQ(-100, rc) << out;
    EXPECT_NE(std::string::npos, out.find("CAJETA_ERROR_TYPE_PARAMETER_BOUND")) << out;
    EXPECT_NE(std::string::npos, out.find(message)) << out;
    EXPECT_NE(std::string::npos, out.find(userFile + ":4:")) << out;
}

}  // namespace

// Aprobe.cajeta sorts before Zshape.cajeta: the case that did not resolve.
TEST(TemplateBoundOrderTests, aBoundDeclaredInALaterFileHoldsForEveryShape) {
    expectEveryShape("Zshape", "Aprobe.cajeta");
}

TEST(TemplateBoundOrderTests, aBoundDeclaredInAnEarlierFileHoldsForEveryShape) {
    expectEveryShape("Ashape", "Zuser.cajeta");
}

TEST(TemplateBoundOrderTests, aViolatingArgumentIsRejectedWhenTheBoundComesLater) {
    expectRejected("Zshape", "Aprobe.cajeta", "        Fam<Main> f = heap Fam<Main>();\n"
                   "        return 0;\n",
                   "argument 'app.Main' does not satisfy bound 'Zshape'");
}

TEST(TemplateBoundOrderTests, aViolatingArgumentIsRejectedWhenTheBoundComesFirst) {
    expectRejected("Ashape", "Zuser.cajeta", "        Fam<Main> f = heap Fam<Main>();\n"
                   "        return 0;\n",
                   "argument 'app.Main' does not satisfy bound 'Ashape'");
}

TEST(TemplateBoundOrderTests, aBoundNamingNoTypeIsRejected) {
    auto files = world("Zshape", "Aprobe.cajeta", "        Fam<Impl> f = heap Fam<Impl>();\n"
                       "        return 0;\n");
    files["Fam.cajeta"] =
        "package app;\n"
        "public final class Fam<S extends Nowhere> {\n"
        "    public Fam() { return; }\n"
        "}\n";
    std::string out;
    int rc = buildAndRun(files, out);
    EXPECT_EQ(-100, rc) << out;
    EXPECT_NE(std::string::npos, out.find("bound 'Nowhere' on parameter 'S' did not resolve"))
        << out;
}
