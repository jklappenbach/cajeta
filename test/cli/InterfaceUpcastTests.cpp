// A value of an interface that extends another, held where the base is expected, must
// dispatch the base's methods: v0.40.0 kept the derived table and called the wrong slot.

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

std::string upcastCompilerBinary() {
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

int upcastExit(int status) {
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
    fs::path root = fs::temp_directory_path() / ("cajeta_upcast_" + std::to_string(rng()));
    for (const auto& [name, text] : files) {
        fs::path p = root / "src" / (name.find('/') == std::string::npos ? "app/" + name : name);
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
    fs::create_directories(root / "out");
    fs::path log = root / "build.log";
    fs::path exe = root / "out" / "prog";
    std::string cmd = "\"" + upcastCompilerBinary() + "\" --emit=exe -o \"" + exe.string()
        + "\" app.Main.main \"" + (root / "src").string() + "\" \"" + (root / "out").string()
        + "\" > \"" + log.string() + "\" 2>&1";
    int rc = upcastExit(std::system(cajeta_shell(cmd).c_str()));
    {
        std::ifstream in(log);
        output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int result = -100;
    if (rc == 0) {
        fs::path runLog = root / "run.log";
        std::string run = "\"" + cajeta_exe_path(exe).string() + "\" > \"" + runLog.string()
            + "\" 2>&1";
        result = upcastExit(std::system(cajeta_shell(run).c_str()));
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
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.lang.System;\n"
    "public class Main {\n"
    "    static Base keep;\n"
    "    static int32 viaBase(Base b) { return b.size(); }\n"
    "    static int32 viaWide(Wide w) { return w.rows() + w.size(); }\n"
    "    static Base up(Wide w) { return w; }\n"
    "    static int32 viaRight(Right r) { return r.right(); }\n"
    "    static #Base fresh() { Wide w = heap Impl(); return #w; }\n"
    "    static int32 take(#Base b) { return b.size(); }\n"
    "    public static int32 main(String[] args) {\n"
    "        Wide w = heap Impl();\n"
    "        System.stdout.println(\"arg \" + Main.viaBase(w));\n"
    "        Base local = w;\n"
    "        System.stdout.println(\"local \" + local.size());\n"
    "        Base assigned = heap Other();\n"
    "        assigned = w;\n"
    "        System.stdout.println(\"assign \" + assigned.size());\n"
    "        Main.keep #= w;\n"
    "        System.stdout.println(\"field \" + Main.keep.size());\n"
    "        System.stdout.println(\"return \" + Main.up(w).size());\n"
    "        System.stdout.println(\"cast \" + ((Base) w).size());\n"
    "        Base[] arr #= heap Base[1];\n"
    "        arr[0] = w;\n"
    "        System.stdout.println(\"element \" + arr[0].size());\n"
    "        ArrayList<Base> list #= heap ArrayList<Base>();\n"
    "        list.add(w);\n"
    "        System.stdout.println(\"list \" + list.get(0).size());\n"
    "        Deep d = heap DeepImpl();\n"
    "        Base db = d;\n"
    "        Mid dm = d;\n"
    "        System.stdout.println(\"deep \" + db.size() + \" \" + dm.mid());\n"
    "        Both two = heap BothImpl();\n"
    "        System.stdout.println(\"second \" + Main.viaRight(two));\n"
    "        Left lft = two;\n"
    "        System.stdout.println(\"first \" + lft.left());\n"
    "        System.stdout.println(\"derived \" + Main.viaWide(w));\n"
    "        Wide same = w;\n"
    "        System.stdout.println(\"same \" + same.size());\n"
    "        Base direct = heap Impl();\n"
    "        System.stdout.println(\"direct \" + direct.size());\n"
    "        boolean yes = args.count() >= 0;\n"
    "        boolean no = args.count() < 0;\n"
    "        Base otherB = heap Other();\n"
    "        Base t1 = no ? otherB : w;\n"
    "        Base t2 = yes ? w : otherB;\n"
    "        System.stdout.println(\"ternary \" + t1.size() + \" \" + t2.size());\n"
    "        Wide none = null;\n"
    "        Base nb = none;\n"
    "        if (nb == null) { System.stdout.println(\"null ok\"); }\n"
    "        int64 l0 = Cajeta.liveCount();\n"
    "        int32 i = 0;\n"
    "        while (i < 8) { Base f #= Main.fresh(); i = i + f.size() - 3; }\n"
    "        System.stdout.println(\"live \" + (Cajeta.liveCount() - l0));\n"
    "        int64 l1 = Cajeta.liveCount();\n"
    "        int32 got = 0;\n"
    "        {\n"
    "            ArrayList<Base> owned #= heap ArrayList<Base>();\n"
    "            int32 j = 0;\n"
    "            while (j < 4) { Wide ow = heap Impl(); owned.add(#ow); j = j + 1; }\n"
    "            got = owned.get(3).size();\n"
    "        }\n"
    "        int64 ownedLive = Cajeta.liveCount() - l1;\n"
    "        System.stdout.println(\"owned \" + got + \" \" + ownedLive);\n"
    "        int64 l2 = Cajeta.liveCount();\n"
    "        int32 k = 0;\n"
    "        while (k < 4) { Wide mw = heap Impl(); Base mb #= mw; k = k + mb.size() - 3; }\n"
    "        System.stdout.println(\"moved \" + (Cajeta.liveCount() - l2));\n"
    "        int64 l3 = Cajeta.liveCount();\n"
    "        int32 t = 0;\n"
    "        while (t < 4) { Wide tw = heap Impl(); t = t + Main.take(#tw) - 3; }\n"
    "        System.stdout.println(\"taken \" + (Cajeta.liveCount() - l3));\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

class InterfaceUpcastTests : public ::testing::Test {
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

std::string InterfaceUpcastTests::out;
int InterfaceUpcastTests::rc = -1;

}  // namespace

TEST_F(InterfaceUpcastTests, anArgumentDispatchesTheBaseMethod) { expectLine("arg 4"); }
TEST_F(InterfaceUpcastTests, aLocalDispatchesTheBaseMethod) { expectLine("local 4"); }
TEST_F(InterfaceUpcastTests, anAssignmentDispatchesTheBaseMethod) { expectLine("assign 4"); }
TEST_F(InterfaceUpcastTests, aFieldDispatchesTheBaseMethod) { expectLine("field 4"); }
TEST_F(InterfaceUpcastTests, aReturnDispatchesTheBaseMethod) { expectLine("return 4"); }
TEST_F(InterfaceUpcastTests, aCastDispatchesTheBaseMethod) { expectLine("cast 4"); }
TEST_F(InterfaceUpcastTests, anArrayElementDispatchesTheBaseMethod) { expectLine("element 4"); }
TEST_F(InterfaceUpcastTests, aCollectionElementDispatchesTheBaseMethod) { expectLine("list 4"); }
TEST_F(InterfaceUpcastTests, aTwoLevelChainDispatchesEachAncestor) { expectLine("deep 4 5"); }
TEST_F(InterfaceUpcastTests, aConditionalOfBaseAndDerivedDispatchesEitherArm) {
    expectLine("ternary 4 4");
}
TEST_F(InterfaceUpcastTests, theSecondParentDispatchesItsOwnMethod) { expectLine("second 2"); }
TEST_F(InterfaceUpcastTests, theFirstParentDispatchesItsOwnMethod) { expectLine("first 1"); }
TEST_F(InterfaceUpcastTests, anOwnedUpcastIsDroppedOnce) { expectLine("live 0"); }
TEST_F(InterfaceUpcastTests, aTransferIntoABaseCollectionIsDroppedOnce) { expectLine("owned 4 0"); }
TEST_F(InterfaceUpcastTests, aMoveIntoABaseLocalIsDroppedOnce) { expectLine("moved 0"); }
TEST_F(InterfaceUpcastTests, aTransferToABaseParameterIsDroppedOnce) { expectLine("taken 0"); }

// The paths that never changed tables.
TEST_F(InterfaceUpcastTests, theDerivedInterfaceStillDispatchesBoth) { expectLine("derived 13"); }
TEST_F(InterfaceUpcastTests, aSameInterfaceCopyIsUnchanged) { expectLine("same 4"); }
TEST_F(InterfaceUpcastTests, aClassToBaseConversionIsUnchanged) { expectLine("direct 4"); }
TEST_F(InterfaceUpcastTests, aNullDerivedValueUpcastsToNull) { expectLine("null ok"); }

// The base interface's file sorts after the derived one's: this faulted at the call.
TEST(InterfaceUpcastOrderTests, aBaseSortedAfterTheDerivedInterfaceDispatches) {
    std::string out;
    int rc = buildAndRun({
        {"Narrow.cajeta", iface("Narrow extends Zbase { int32 rows(); }")},
        {"Zbase.cajeta", iface("Zbase { int32 size(); }")},
        {"Impl.cajeta", impl("Impl", "Narrow",
            "    public int32 size() { return 4; }\n"
            "    public int32 rows() { return 9; }\n")},
        {"Main.cajeta",
            "package app;\n"
            "import cajeta.lang.String;\n"
            "import cajeta.lang.System;\n"
            "public class Main {\n"
            "    static int32 viaBase(Zbase b) { return b.size(); }\n"
            "    public static int32 main(String[] args) {\n"
            "        Narrow n = heap Impl();\n"
            "        System.stdout.println(\"base \" + Main.viaBase(n) + \" rows \" + n.rows());\n"
            "        return 0;\n"
            "    }\n"
            "}\n"}}, out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("base 4 rows 9\n")) << out;
}

// A user interface extending a stdlib one, passed where the stdlib type is expected.
TEST(InterfaceUpcastOrderTests, aStdlibBaseDispatchesThroughAUserExtension) {
    std::string out;
    int rc = buildAndRun({
        {"Mine.cajeta",
            "package app;\n"
            "import cajeta.xpu.ShapeFilter;\n"
            "import cajeta.xpu.ShapeProbe;\n"
            "public interface Mine extends ShapeProbe { void bind(^ShapeFilter f); }\n"},
        {"Impl.cajeta",
            "package app;\n"
            "import cajeta.xpu.ShapeFilter;\n"
            "public final class Impl implements Mine {\n"
            "    int32 k = 0;\n"
            "    public Impl() { return; }\n"
            "    public void bind(^ShapeFilter f) { this.k = 5; }\n"
            "    public int64 timeNanos(int32 i) { return (int64) (this.k + i); }\n"
            "}\n"},
        {"Main.cajeta",
            "package app;\n"
            "import cajeta.lang.String;\n"
            "import cajeta.lang.System;\n"
            "import cajeta.xpu.ShapeProbe;\n"
            "public class Main {\n"
            "    static int64 viaStd(ShapeProbe p) { return p.timeNanos(2); }\n"
            "    public static int32 main(String[] args) {\n"
            "        Mine m = heap Impl();\n"
            "        m.bind(null);\n"
            "        System.stdout.println(\"t \" + Main.viaStd(m) + \" direct \" + m.timeNanos(1));\n"
            "        return 0;\n"
            "    }\n"
            "}\n"}}, out);
    EXPECT_EQ(0, rc) << out;
    EXPECT_NE(std::string::npos, out.find("t 7 direct 6\n")) << out;
}
