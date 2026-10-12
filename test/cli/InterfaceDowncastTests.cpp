// A cast from an interface to one that extends it looks the instance's class up at run
// time and takes its table for the derived interface; a class without it throws.

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

std::string downcastCompilerBinary() {
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

int downcastExit(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Builds `files` under package app (a key with a slash names its own package path) and
// runs app.Main.main. The run's exit code, or -100 when the build failed.
int buildAndRun(const std::map<std::string, std::string>& files, std::string& output) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_downcast_" + std::to_string(rng()));
    for (const auto& [name, text] : files) {
        fs::path p = root / "src" / (name.find('/') == std::string::npos ? "app/" + name : name);
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::create_directories(root / "out");
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + downcastCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = downcastExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int result = -100;
    if (rc == 0) {
        fs::path runLog = root / "run.log";
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string()
            + "\" 2>&1";
        result = downcastExit(std::system(cajeta_shell(run).c_str()));
        std::ifstream in(runLog);
        output.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return result;
}

std::string iface(const std::string& decl) {
    return "package app;\npublic interface " + decl + "\n";
}

std::string impl(const std::string& name, const std::string& iface, const std::string& body) {
    return "package app;\npublic final class " + name + " implements " + iface + " {\n"
           "    public " + name + "() { return; }\n" + body + "}\n";
}

const char* kMain =
    "package app;\n"
    "import cajeta.error.ClassCastException;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.lang.System;\n"
    "public class Main {\n"
    "    static #Base fresh() { Wide w = heap Impl(); return #w; }\n"
    "    static #Wide freshW() { Wide w = heap Impl(); return #w; }\n"
    "    static int32 viaWide(Wide w) { return w.rows(); }\n"
    "    static int32 take(#Wide w) { return w.rows(); }\n"
    "    public static int32 main(String[] args) {\n"
    "        Base b = heap Impl();\n"
    "        Wide w = (Wide) b;\n"
    "        System.stdout.println(\"down \" + w.rows() + \" \" + w.size());\n"
    "        System.stdout.println(\"inline \" + ((Wide) b).rows());\n"
    "        System.stdout.println(\"arg \" + Main.viaWide((Wide) b));\n"
    "        Wide orig = heap Impl();\n"
    "        Base up = orig;\n"
    "        System.stdout.println(\"round \" + ((Wide) up).rows());\n"
    "        Base db = heap DeepImpl();\n"
    "        Mid dm = (Mid) db;\n"
    "        Deep dd = (Deep) db;\n"
    "        Deep md = (Deep) dm;\n"
    "        System.stdout.println(\"deep \" + dm.mid() + \" \" + dd.deep() + \" \" + md.size());\n"
    "        Left lft = heap BothImpl();\n"
    "        Both two = (Both) lft;\n"
    "        System.stdout.println(\"second \" + two.both() + \" \" + two.right());\n"
    "        Base other = heap Other();\n"
    "        int32 threw = 0;\n"
    "        try {\n"
    "            Wide bad = (Wide) other;\n"
    "            System.stdout.println(\"no throw \" + bad.rows());\n"
    "        } catch (ClassCastException e) {\n"
    "            threw = 1;\n"
    "        }\n"
    "        System.stdout.println(\"threw \" + threw + \" \" + other.size());\n"
    "        Base nb = null;\n"
    "        Wide nw = (Wide) nb;\n"
    "        if (nw == null) { System.stdout.println(\"null ok\"); }\n"
    "        int64 l0 = Cajeta.liveCount();\n"
    "        int32 i = 0;\n"
    "        while (i < 8) { Base f #= Main.fresh(); Wide fw = (Wide) f; i = i + fw.rows() - 8; }\n"
    "        System.stdout.println(\"lent \" + (Cajeta.liveCount() - l0));\n"
    "        int64 l1 = Cajeta.liveCount();\n"
    "        int32 k = 0;\n"
    "        while (k < 8) { Base f #= Main.fresh(); Wide fw #= (Wide) f; k = k + fw.rows() - 8; }\n"
    "        System.stdout.println(\"moved \" + (Cajeta.liveCount() - l1));\n"
    "        int64 l2 = Cajeta.liveCount();\n"
    "        int32 t = 0;\n"
    "        while (t < 8) { Base f #= Main.fresh(); Wide fw #= (Wide) f; t = t + Main.take(#fw) - 8; }\n"
    "        System.stdout.println(\"taken \" + (Cajeta.liveCount() - l2));\n"
    "        Base sf #= Main.fresh();\n"
    "        int64 m0 = Cajeta.liveCount();\n"
    "        { Wide sw #= (Wide) #sf; int32 r0 = sw.rows(); }\n"
    "        int64 m1 = Cajeta.liveCount() - m0;\n"
    "        System.stdout.println(\"sharp scope \" + m1);\n"
    "        Base sg #= Main.fresh();\n"
    "        int64 n0 = Cajeta.liveCount();\n"
    "        int32 r1 = Main.take(#(Wide) sg);\n"
    "        int64 n1 = Cajeta.liveCount() - n0;\n"
    "        System.stdout.println(\"sharp arg \" + r1 + \" \" + n1);\n"
    "        Base sh #= Main.fresh();\n"
    "        int64 q0 = Cajeta.liveCount();\n"
    "        { Wide hw = (Wide) sh; int32 r2 = hw.rows(); }\n"
    "        int64 q1 = Cajeta.liveCount() - q0;\n"
    "        System.stdout.println(\"lent scope \" + q1 + \" \" + sh.size());\n"
    "        Wide su #= Main.freshW();\n"
    "        int64 u0 = Cajeta.liveCount();\n"
    "        { Base ub #= (Base) #su; int32 r3 = ub.size(); }\n"
    "        int64 u1 = Cajeta.liveCount() - u0;\n"
    "        System.stdout.println(\"sharp up \" + u1);\n"
    "        int64 i0 = Cajeta.liveCount();\n"
    "        int32 s = 0;\n"
    "        while (s < 8) { Wide w0 #= Main.freshW(); Wide same #= (Wide) w0; s = s + Main.take(#same) - 8; }\n"
    "        System.stdout.println(\"same cast \" + (Cajeta.liveCount() - i0));\n"
    "        Base keepB = heap Impl();\n"
    "        {\n"
    "            Wide inner = (Wide) keepB;\n"
    "            System.stdout.println(\"inner \" + inner.rows());\n"
    "        }\n"
    "        System.stdout.println(\"after \" + keepB.size());\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

class InterfaceDowncastTests : public ::testing::Test {
protected:
    static std::string out;
    static int rc;

    static void SetUpTestSuite() {
        rc = buildAndRun({
            {"Base.cajeta", iface("Base { int32 size(); }")},
            {"Wide.cajeta", iface("Wide extends Base { int32 rows(); }")},
            {"Mid.cajeta", iface("Mid extends Base { int32 mid(); }")},
            {"Deep.cajeta", iface("Deep extends Mid { int32 deep(); }")},
            {"Left.cajeta", iface("Left { int32 left(); }")},
            {"Right.cajeta", iface("Right { int32 right(); }")},
            {"Both.cajeta", iface("Both extends Left, Right { int32 both(); }")},
            {"Impl.cajeta", impl("Impl", "Wide",
                "    public int32 size() { return 4; }\n"
                "    public int32 rows() { return 9; }\n")},
            {"Other.cajeta", impl("Other", "Base", "    public int32 size() { return 7; }\n")},
            {"DeepImpl.cajeta", impl("DeepImpl", "Deep",
                "    public int32 size() { return 4; }\n"
                "    public int32 mid() { return 5; }\n"
                "    public int32 deep() { return 6; }\n")},
            {"BothImpl.cajeta", impl("BothImpl", "Both",
                "    public int32 left() { return 1; }\n"
                "    public int32 right() { return 2; }\n"
                "    public int32 both() { return 3; }\n")},
            {"Main.cajeta", kMain}}, out);
    }

    void expectLine(const std::string& line) {
        EXPECT_EQ(0, rc) << out;
        EXPECT_NE(std::string::npos, out.find(line + "\n")) << "want '" << line << "':\n" << out;
    }
};

std::string InterfaceDowncastTests::out;
int InterfaceDowncastTests::rc = -1;

}  // namespace

TEST_F(InterfaceDowncastTests, aDowncastDispatchesTheDerivedMethods) { expectLine("down 9 4"); }
TEST_F(InterfaceDowncastTests, aDowncastAsAReceiverDispatches) { expectLine("inline 9"); }
TEST_F(InterfaceDowncastTests, aDowncastAsAnArgumentDispatches) { expectLine("arg 9"); }
TEST_F(InterfaceDowncastTests, anUpcastValueDowncastsBack) { expectLine("round 9"); }
TEST_F(InterfaceDowncastTests, aTwoLevelChainDowncastsToEachLevel) { expectLine("deep 5 6 4"); }
TEST_F(InterfaceDowncastTests, aDowncastFromTheFirstParentReachesTheSecond) {
    expectLine("second 3 2");
}
TEST_F(InterfaceDowncastTests, aClassWithoutTheInterfaceThrowsClassCastException) {
    expectLine("threw 1 7");
}
TEST_F(InterfaceDowncastTests, aNullDowncastsToNull) { expectLine("null ok"); }
TEST_F(InterfaceDowncastTests, aLentDowncastLeavesTheTitleWithTheSource) { expectLine("lent 0"); }
TEST_F(InterfaceDowncastTests, aSharpDowncastTakesTheTitleOnce) { expectLine("moved 0"); }
TEST_F(InterfaceDowncastTests, aSharpDowncastTransfersToAParameter) { expectLine("taken 0"); }
// The deltas read the live count into a local first: a count read inside a String
// concatenation includes the concatenation's own allocation.
TEST_F(InterfaceDowncastTests, aSharpDowncastFreesAtTheNewOwnersScope) {
    expectLine("sharp scope -1");
}
TEST_F(InterfaceDowncastTests, aSharpDowncastArgumentIsFreedByTheCallee) {
    expectLine("sharp arg 9 -1");
}
TEST_F(InterfaceDowncastTests, aLentDowncastFreesNothing) { expectLine("lent scope 0 4"); }
TEST_F(InterfaceDowncastTests, aSharpUpcastFreesAtTheNewOwnersScope) { expectLine("sharp up -1"); }
TEST_F(InterfaceDowncastTests, aSameInterfaceCastCopiesTheBody) { expectLine("same cast 0"); }
TEST_F(InterfaceDowncastTests, aLentDowncastDoesNotFreeTheSource) {
    expectLine("inner 9");
    expectLine("after 4");
}
