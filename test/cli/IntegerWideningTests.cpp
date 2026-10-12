// int64-scan-verbs 8.4: a widening extends by the SOURCE type's signedness, at every position
// and for the explicit cast (decided 2026-10-12). v0.40.0 sign-extended every implicit widening,
// so a uint32 above 2^31 read back with its high word set, and cast by the target type.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string wideningCompilerBinary() {
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

int wideningExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

struct Source { const char* tag; const char* type; const char* value; const char* asU64; const char* asI64; };
struct Target { const char* tag; const char* type; };

const std::vector<Source> kSources = {
    {"u32", "uint32", "2147483648", "2147483648", "2147483648"},
    {"u16", "uint16", "40000", "40000", "40000"},
    {"u8", "uint8", "200", "200", "200"},
    {"i32", "int32", "-5", "18446744073709551611", "-5"},
};
const std::vector<Target> kTargets = {{"u64", "uint64"}, {"i64", "int64"}};
const std::vector<std::string> kPositions = {"init", "assign", "field", "static", "element",
                                             "arg", "ret", "ternary", "cast", "binop"};

std::string valueOf(const Source& s) {
    if (std::string(s.tag) == "i32") return "0 - 5";
    return s.value;
}

// One program: every (position, source, target) prints "<position>_<source>_<target> <value>".
std::string program() {
    std::ostringstream m;
    m << "package app;\n\npublic class Main {\n";
    m << "    static uint64 gu64;\n    static int64 gi64;\n";
    for (const auto& s : kSources)
        m << "    public static " << s.type << " " << s.tag << "v() { " << s.type << " x = "
          << valueOf(s) << "; return x; }\n";
    m << "    public static boolean yes() { return true; }\n";
    m << "    public static uint64 idu64(uint64 v) { return v; }\n";
    m << "    public static int64 idi64(int64 v) { return v; }\n";
    for (const auto& s : kSources)
        for (const auto& t : kTargets)
            m << "    public static " << t.type << " ret_" << s.tag << "_" << t.tag << "() { "
              << s.type << " x = Main." << s.tag << "v(); return x; }\n";
    m << "    public static void main(String[] args) {\n";
    m << "        Box box = heap Box();\n";
    m << "        System.stdout.println(\"fieldinit_u32_u64 \" + box.initU);\n";
    m << "        System.stdout.println(\"fieldinit_u16_i64 \" + box.initI);\n";
    m << "        uint32 big = Main.u32v();\n";
    m << "        float64 fInit = big;\n";
    m << "        float64 fAssign = 0.0;\n        fAssign = big;\n";
    m << "        float64 fSum = 0.0 + big;\n";
    m << "        uint64[] lit = [big];\n";
    m << "        System.stdout.println(\"float_init \" + (fInit > 0.0));\n";
    m << "        System.stdout.println(\"float_assign \" + (fAssign > 0.0));\n";
    m << "        System.stdout.println(\"float_binop \" + (fSum > 0.0));\n";
    m << "        System.stdout.println(\"literal_u32_u64 \" + lit[0]);\n";
    for (const auto& s : kSources) {
        m << "        " << s.type << " s_" << s.tag << " = Main." << s.tag << "v();\n";
        for (const auto& t : kTargets) {
            std::string k = std::string("_") + s.tag + "_" + t.tag;
            std::string src = std::string("s_") + s.tag;
            std::string T = t.type;
            auto line = [&](const std::string& pos, const std::string& setup, const std::string& expr) {
                m << "        {\n" << setup << "            System.stdout.println(\"" << pos << k
                  << " \" + " << expr << ");\n        }\n";
            };
            line("init", "            " + T + " v = " + src + ";\n", "v");
            line("assign", "            " + T + " v = 0;\n            v = " + src + ";\n", "v");
            line("field", "            box.f" + std::string(t.tag) + " = " + src + ";\n",
                 "box.f" + std::string(t.tag));
            line("static", "            Main.g" + std::string(t.tag) + " = " + src + ";\n",
                 "Main.g" + std::string(t.tag));
            line("element", "            " + T + "[] a = heap " + T + "[1];\n            a[0] = "
                 + src + ";\n", "a[0]");
            line("arg", "", "Main.id" + std::string(t.tag) + "(" + src + ")");
            line("ret", "", "Main.ret" + k + "()");
            line("ternary", "            " + T + " v = Main.yes() ? " + src + " : " + src + ";\n", "v");
            line("cast", "            " + T + " v = (" + T + ") " + src + ";\n", "v");
            line("binop", "            " + T + " z = 0;\n            " + T + " v = z + " + src + ";\n", "v");
        }
    }
    m << "    }\n}\n";
    return m.str();
}

const char* kBox =
    "package app;\n\npublic class Box {\n    public uint64 fu64;\n    public int64 fi64;\n"
    "    public uint64 initU = Main.u32v();\n    public int64 initI = Main.u16v();\n}\n";

struct WideningRun { int rc = -1; std::string output; std::map<std::string, std::string> got; };

const WideningRun& runOnce() {
    static WideningRun run;
    static bool done = false;
    if (done) return run;
    done = true;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_widening_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "app");
    fs::create_directories(root / "out");
    std::ofstream(root / "src" / "app" / "Main.cajeta") << program();
    std::ofstream(root / "src" / "app" / "Box.cajeta") << kBox;
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + wideningCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = wideningExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        run.output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (rc != 0) { run.rc = -100; return run; }
    fs::path runLog = root / "run.log";
    std::string r = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string() + "\" 2>&1";
    run.rc = wideningExit(std::system(cajeta_shell(r).c_str()));
    std::ifstream in(runLog);
    std::string line;
    while (std::getline(in, line)) {
        run.output += line + "\n";
        size_t sp = line.find(' ');
        if (sp != std::string::npos) run.got[line.substr(0, sp)] = line.substr(sp + 1);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return run;
}

void checkPosition(const std::string& pos) {
    const WideningRun& run = runOnce();
    ASSERT_EQ(run.rc, 0) << run.output;
    for (const auto& s : kSources)
        for (const auto& t : kTargets) {
            std::string key = pos + "_" + s.tag + "_" + t.tag;
            std::string want = std::string(t.tag) == "u64" ? s.asU64 : s.asI64;
            auto it = run.got.find(key);
            ASSERT_NE(it, run.got.end()) << key << " not printed:\n" << run.output;
            EXPECT_EQ(it->second, want) << key << ": a " << s.type << " widened into a " << t.type;
        }
}

}  // namespace

TEST(IntegerWidening, aLocalInitializerWidensBySource) { checkPosition("init"); }
TEST(IntegerWidening, anAssignmentWidensBySource) { checkPosition("assign"); }
TEST(IntegerWidening, aFieldStoreWidensBySource) { checkPosition("field"); }
TEST(IntegerWidening, aStaticFieldStoreWidensBySource) { checkPosition("static"); }
TEST(IntegerWidening, anElementStoreWidensBySource) { checkPosition("element"); }
TEST(IntegerWidening, anArgumentWidensBySource) { checkPosition("arg"); }
TEST(IntegerWidening, aReturnWidensBySource) { checkPosition("ret"); }
TEST(IntegerWidening, aTernaryArmWidensBySource) { checkPosition("ternary"); }
TEST(IntegerWidening, anExplicitCastWidensBySource) { checkPosition("cast"); }
TEST(IntegerWidening, aBinaryOperandWidensBySource) { checkPosition("binop"); }

TEST(IntegerWidening, aFieldInitializerWidensBySource) {
    const WideningRun& run = runOnce();
    ASSERT_EQ(run.rc, 0) << run.output;
    EXPECT_EQ(run.got.count("fieldinit_u32_u64") ? run.got.at("fieldinit_u32_u64") : "", "2147483648");
    EXPECT_EQ(run.got.count("fieldinit_u16_i64") ? run.got.at("fieldinit_u16_i64") : "", "40000");
}

TEST(IntegerWidening, anArrayLiteralElementWidensBySource) {
    const WideningRun& run = runOnce();
    ASSERT_EQ(run.rc, 0) << run.output;
    EXPECT_EQ(run.got.count("literal_u32_u64") ? run.got.at("literal_u32_u64") : "", "2147483648");
}

// An unsigned integer converted to floating point stays non-negative.
TEST(IntegerWidening, anUnsignedValueConvertsToAPositiveFloat) {
    const WideningRun& run = runOnce();
    ASSERT_EQ(run.rc, 0) << run.output;
    for (const char* k : {"float_init", "float_assign", "float_binop"})
        EXPECT_EQ(run.got.count(k) ? run.got.at(k) : "", "true") << k;
}
