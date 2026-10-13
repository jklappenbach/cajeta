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

// A templated interface instantiated over a class whose file has not been read
// yet: Main.cajeta sorts before Signup.cajeta, so `Codec<Signup>` in Main's
// signature first meets a forward placeholder. It used to resolve to the bare
// template, and every call through it was CAJETA_ERROR_MEMBER_NOT_FOUND
// (v0.39.0, found by primavera-web Unit 5).
namespace {

const char* kSignup =
    "package app;\n"
    "public class Signup {\n"
    "    public int32 n;\n"
    "    public Signup() { this.n = 5; return; }\n"
    "}\n";

const char* kCodec =
    "package app;\n"
    "import cajeta.lang.String;\n"
    "public interface Codec<T> {\n"
    "    String mediaType();\n"
    "    int32 sizeOf(T value);\n"
    "    #T make();\n"
    "}\n";

const char* kSignupCodec =
    "package app;\n"
    "import cajeta.lang.String;\n"
    "public class SignupCodec implements Codec<Signup> {\n"
    "    public SignupCodec() { return; }\n"
    "    public String mediaType() { return \"x-signup\"; }\n"
    "    public int32 sizeOf(Signup v) { return v.n * 10; }\n"
    "    public #Signup make() { return heap Signup(); }\n"
    "}\n";

}  // namespace

TEST(TemplateInterfaceOrderTests, aTemplatedInterfaceOverALaterClassKeepsItsMethods) {
    std::string main =
        "package app;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.lang.System;\n"
        "public class Main {\n"
        "    static int32 size(Codec<Signup> c) {\n"
        "        Signup s #= c.make();\n"
        "        return c.sizeOf(s);\n"
        "    }\n"
        "    public static int32 main(String[] args) {\n"
        "        Codec<Signup> c = heap SignupCodec();\n"
        "        System.stdout.println(\"type \" + c.mediaType());\n"
        "        System.stdout.println(\"size \" + Main.size(c));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    OrderWorld w({{"Codec.cajeta", kCodec}, {"Main.cajeta", main},
                  {"Signup.cajeta", kSignup}, {"SignupCodec.cajeta", kSignupCodec}});
    std::string out;
    int rc = w.buildAndRun(out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("type x-signup")) << out;
    EXPECT_NE(std::string::npos, out.find("size 50")) << out;
}

// The stdlib's own templated interface, over a class declared later.
TEST(TemplateInterfaceOrderTests, aStdlibTemplatedInterfaceOverALaterClassKeepsItsMethods) {
    std::string main =
        "package app;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.lang.System;\n"
        "import cajeta.wire.Encoder;\n"
        "public class Main {\n"
        "    static int64 width(Encoder<Signup> c, Signup s) {\n"
        "        int8[] b #= c.encode(s);\n"
        "        return b.count();\n"
        "    }\n"
        "    public static int32 main(String[] args) {\n"
        "        Signup s = heap Signup();\n"
        "        System.stdout.println(\"width \" + Main.width(heap Wire(), s));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    std::string wire =
        "package app;\n"
        "import cajeta.wire.Encoder;\n"
        "public class Wire implements Encoder<Signup> {\n"
        "    public Wire() { return; }\n"
        "    public #int8[] encode(Signup v) { return heap int8[v.n]; }\n"
        "    public #Signup decode(int8[] b) { return heap Signup(); }\n"
        "}\n";
    OrderWorld w({{"Main.cajeta", main}, {"Signup.cajeta", kSignup}, {"Wire.cajeta", wire}});
    std::string out;
    int rc = w.buildAndRun(out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("width 5")) << out;
}

// A class template holding a list of the interface, the shape a per-type codec
// registry takes, instantiated over the later class.
TEST(TemplateInterfaceOrderTests, aListOfATemplatedInterfaceOverALaterClassDispatches) {
    std::string registry =
        "package app;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class Registry<T> {\n"
        "    ArrayList<Codec<T>> all;\n"
        "    public Registry() { this.all = heap ArrayList<Codec<T>>(); return; }\n"
        "    public void add(#Codec<T> c) { this.all.add(#c); return; }\n"
        "    public int32 total(T v) {\n"
        "        int32 t = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < this.all.count()) { t = t + this.all.get(i).sizeOf(v); i = i + 1; }\n"
        "        return t;\n"
        "    }\n"
        "}\n";
    std::string main =
        "package app;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.lang.System;\n"
        "public class Main {\n"
        "    public static int32 main(String[] args) {\n"
        "        Registry<Signup> r = heap Registry<Signup>();\n"
        "        r.add(heap SignupCodec());\n"
        "        r.add(heap SignupCodec());\n"
        "        Signup s = heap Signup();\n"
        "        System.stdout.println(\"total \" + r.total(s));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    OrderWorld w({{"Codec.cajeta", kCodec}, {"Main.cajeta", main}, {"Registry.cajeta", registry},
                  {"Signup.cajeta", kSignup}, {"SignupCodec.cajeta", kSignupCodec}});
    std::string out;
    int rc = w.buildAndRun(out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("total 100")) << out;
}
