// A class template's `implements` must not depend on source order: a template
// whose file sorts before its interface lost the interface table (v0.39.0).

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

std::string orderCompilerBinary() {
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

int orderExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct OrderWorld {
    fs::path root;
    explicit OrderWorld(const std::map<std::string, std::string>& files) {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path() / ("cajeta_tplorder_" + std::to_string(rng()));
        for (const auto& [name, text] : files) {
            fs::path p = root / "src" / "app" / name;
            fs::create_directories(p.parent_path());
            std::ofstream(p) << text;
        }
        fs::create_directories(root / "out");
    }
    ~OrderWorld() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The run's exit code, or -100 when the build failed; `output` gets both logs.
    int buildAndRun(std::string& output) const {
        fs::path log = root / "build.log";
        fs::path exe = root / "out" / "prog";
        std::string cmd = "\"" + orderCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
            + "\" app.Main.main \"" + (root / "src").string() + "\" \""
            + (root / "out").string() + "\" > \"" + log.string() + "\" 2>&1";
        int rc = orderExit(std::system(cajeta_shell(cmd).c_str()));
        {
            std::ifstream in(log);
            output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (rc != 0) return -100;
        fs::path runLog = root / "run.log";
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string()
            + "\" 2>&1";
        int runRc = orderExit(std::system(cajeta_shell(run).c_str()));
        std::ifstream in(runLog);
        output.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return runRc;
    }
};

const char* kShape =
    "package app;\n"
    "public interface Shape { int32 size(); }\n";

std::string templateTile(const std::string& name) {
    return "package app;\n"
           "@Instantiate({" + name + "<4>})\n"
           "public final class " + name + "<uint32 N> implements Shape {\n"
           "    public " + name + "() { return; }\n"
           "    public int32 size() { return (int32) N; }\n"
           "}\n";
}

std::string plainTile(const std::string& name) {
    return "package app;\n"
           "public final class " + name + " implements Shape {\n"
           "    public " + name + "() { return; }\n"
           "    public int32 size() { return 4; }\n"
           "}\n";
}

std::string mainOf(const std::string& made) {
    return "package app;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.lang.System;\n"
           "public class Main {\n"
           "    static int32 take(Shape s) { return s.size(); }\n"
           "    public static int32 main(String[] args) {\n"
           "        Shape s = heap " + made + "();\n"
           "        System.stdout.println(\"size \" + s.size());\n"
           "        System.stdout.println(\"took \" + Main.take(heap " + made + "()));\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

void expectDispatch(const std::map<std::string, std::string>& files, const std::string& what) {
    OrderWorld w(files);
    std::string out;
    int rc = w.buildAndRun(out);
    EXPECT_EQ(0, rc) << what << ":\n" << out;
    EXPECT_NE(std::string::npos, out.find("size 4")) << what << ":\n" << out;
    EXPECT_NE(std::string::npos, out.find("took 4")) << what << ":\n" << out;
}

}  // namespace

// ATile.cajeta sorts before Shape.cajeta: the case that faulted.
TEST(TemplateInterfaceOrderTests, aTemplateSortedBeforeItsInterfaceDispatches) {
    expectDispatch({{"ATile.cajeta", templateTile("ATile")}, {"Shape.cajeta", kShape},
                    {"Main.cajeta", mainOf("ATile<4>")}},
                   "template file before its interface");
}

TEST(TemplateInterfaceOrderTests, aTemplateSortedAfterItsInterfaceDispatches) {
    expectDispatch({{"Shape.cajeta", kShape}, {"ZTile.cajeta", templateTile("ZTile")},
                    {"Main.cajeta", mainOf("ZTile<4>")}},
                   "template file after its interface");
}

TEST(TemplateInterfaceOrderTests, aPlainClassDispatchesInEitherOrder) {
    expectDispatch({{"ATile.cajeta", plainTile("ATile")}, {"Shape.cajeta", kShape},
                    {"Main.cajeta", mainOf("ATile")}},
                   "plain class before its interface");
    expectDispatch({{"Shape.cajeta", kShape}, {"ZTile.cajeta", plainTile("ZTile")},
                    {"Main.cajeta", mainOf("ZTile")}},
                   "plain class after its interface");
}

// The interface in another package, imported, still sorting after the template.
TEST(TemplateInterfaceOrderTests, anImportedInterfaceSortedAfterTheTemplateDispatches) {
    std::string tile =
        "package app;\n"
        "import zz.Shape;\n"
        "@Instantiate({ATile<4>})\n"
        "public final class ATile<uint32 N> implements Shape {\n"
        "    public ATile() { return; }\n"
        "    public int32 size() { return (int32) N; }\n"
        "}\n";
    std::string shape = "package zz;\npublic interface Shape { int32 size(); }\n";
    std::string main = mainOf("ATile<4>");
    main.insert(main.find("import cajeta.lang.String;"), "import zz.Shape;\n");
    OrderWorld w({{"ATile.cajeta", tile}, {"Main.cajeta", main}});
    fs::create_directories(w.root / "src" / "zz");
    std::ofstream(w.root / "src" / "zz" / "Shape.cajeta") << shape;
    std::string out;
    int rc = w.buildAndRun(out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("size 4")) << out;
    EXPECT_NE(std::string::npos, out.find("took 4")) << out;
}
