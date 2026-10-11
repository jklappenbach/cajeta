// A class template's static, named by the bare template name inside the template's own
// body, is the instantiation's static: v0.40.0 left `Box.K` unresolved under every
// instantiation (CAJETA_ERROR_NULL_ARRAY_DIMENSION on `heap int32[Box.K]`).

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

std::string staticCompilerBinary() {
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

int staticExit(int status) {
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
    fs::path root = fs::temp_directory_path() / ("cajeta_tplstatic_" + std::to_string(rng()));
    for (const auto& [name, text] : files) {
        fs::path p = root / "src" / "app" / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::create_directories(root / "out");
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + staticCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = staticExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int result = -100;
    if (rc == 0) {
        fs::path runLog = root / "run.log";
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string()
            + "\" 2>&1";
        result = staticExit(std::system(cajeta_shell(run).c_str()));
        std::ifstream in(runLog);
        output.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return result;
}

const char* kBox =
    "package app;\n"
    "public final class Box<T> {\n"
    "    static final int32 K = 16;\n"
    "    static int32 made = 0;\n"
    "    int32[] slots;\n"
    "    public Box() { this.slots #= heap int32[Box.K]; Box.made = Box.made + 1; }\n"
    "    static int32 twice(int32 v) { return v * 2; }\n"
    "    public int32 n() {\n"
    "        int32 i = 0;\n"
    "        int32 s = 0;\n"
    "        while (i < Box.K) { s = s + 1; i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    public int32 doubled() { return Box.twice((int32) this.slots.count()); }\n"
    "    public int32 count() { return Box.made; }\n"
    "    public int32 plain() { return Plain.K; }\n"
    "}\n";

const char* kPlain =
    "package app;\n"
    "public final class Plain {\n"
    "    static final int32 K = 5;\n"
    "    public Plain() { return; }\n"
    "    public int32 own() { return Plain.K; }\n"
    "}\n";

const char* kTags =
    "package app;\n"
    "public final class Tag { public Tag() { return; } }\n";

const char* kOtherTag =
    "package app;\n"
    "public final class Zag { public Zag() { return; } }\n";

// `holder` holds a Box<Tag> in a field and is the first user of the instantiation.
std::string holder(const std::string& name) {
    return "package app;\n"
           "public final class " + name + " {\n"
           "    Box<Tag> f;\n"
           "    public " + name + "() { this.f #= heap Box<Tag>(); }\n"
           "    public int32 go() { return this.f.n(); }\n"
           "}\n";
}

std::string mainOf(const std::string& holderName) {
    return "package app;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.lang.System;\n"
           "public class Main {\n"
           "    public static int32 main(String[] args) {\n"
           "        " + holderName + " h = heap " + holderName + "();\n"
           "        Box<Tag> a = heap Box<Tag>();\n"
           "        Box<Zag> z = heap Box<Zag>();\n"
           "        Plain p = heap Plain();\n"
           "        System.stdout.println(\"held \" + h.go());\n"
           "        System.stdout.println(\"doubled \" + a.doubled());\n"
           "        System.stdout.println(\"made \" + a.count() + \" \" + z.count());\n"
           "        System.stdout.println(\"plain \" + a.plain() + \" \" + p.own());\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

class TemplateStaticByNameTests : public ::testing::Test {
protected:
    static std::string out;
    static int rc;

    // Holder.cajeta sorts after Box.cajeta, AHolder.cajeta before it.
    static void SetUpTestSuite() {
        rc = buildAndRun({{"AHolder.cajeta", holder("AHolder")}, {"Box.cajeta", kBox},
                          {"Plain.cajeta", kPlain}, {"Tag.cajeta", kTags},
                          {"Zag.cajeta", kOtherTag}, {"Main.cajeta", mainOf("AHolder")}},
                         out);
    }

    void expectLine(const std::string& line) {
        EXPECT_EQ(0, rc) << out;
        EXPECT_NE(std::string::npos, out.find(line + "\n")) << "want '" << line << "':\n" << out;
    }
};

std::string TemplateStaticByNameTests::out;
int TemplateStaticByNameTests::rc = -1;

}  // namespace

TEST_F(TemplateStaticByNameTests, aStaticConstantSizesAnArrayAndBoundsALoop) {
    expectLine("held 16");
}
TEST_F(TemplateStaticByNameTests, aStaticMethodIsCalledByTheBareName) { expectLine("doubled 32"); }
TEST_F(TemplateStaticByNameTests, eachInstantiationKeepsItsOwnMutableStatic) {
    expectLine("made 2 1");
}
TEST_F(TemplateStaticByNameTests, anotherClassStaticIsUnchanged) { expectLine("plain 5 5"); }

TEST(TemplateStaticByNameOrderTests, aHolderSortedAfterTheTemplateBuilds) {
    std::string out;
    int rc = buildAndRun({{"Box.cajeta", kBox}, {"Plain.cajeta", kPlain}, {"Tag.cajeta", kTags},
                          {"Zag.cajeta", kOtherTag}, {"ZHolder.cajeta", holder("ZHolder")},
                          {"Main.cajeta", mainOf("ZHolder")}},
                         out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("held 16\n")) << out;
}
