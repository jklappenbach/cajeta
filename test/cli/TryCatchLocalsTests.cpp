// try-catch-locals: a local changed inside `try` keeps its last stored value in the catch
// and after it, at every optimization level.

#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {

const char* kLevels[] = {"--opt=O0", "--opt=O1", "--opt=O2", "--opt=O3", "--release"};

const char* kBasic =
    "package app;\n"
    "import cajeta.error.Exception;\n"
    "public class Main {\n"
    "    static int32 pick(int32 i) {\n"
    "        if (i >= 3) { throw heap Exception(\"x\"); }\n"
    "        return i + 1;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int32 s = 0;\n"
    "        int32 i = 0;\n"
    "        try {\n"
    "            int32 t = 0;\n"
    "            while (i < 5) { t = s * 2; s = s + Main.pick(i) + t - t; i = i + 1; }\n"
    "        } catch (Exception e) {\n"
    "            System.stdout.println(\"catch s=\" + s + \" i=\" + i);\n"
    "        }\n"
    "        System.stdout.println(\"after s=\" + s + \" i=\" + i);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

const char* kNested =
    "package app;\n"
    "import cajeta.error.Exception;\n"
    "public class Main {\n"
    "    static int32 boom(int32 v) {\n"
    "        if (v > 0) { throw heap Exception(\"b\"); }\n"
    "        return v;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int32 a = 0;\n"
    "        int32 b = 0;\n"
    "        try {\n"
    "            a = 1;\n"
    "            try { b = 2; Main.boom(b); b = 3; }\n"
    "            catch (Exception e) { System.stdout.println(\"inner a=\" + a + \" b=\" + b); a = 4; Main.boom(a); }\n"
    "        } catch (Exception e) {\n"
    "            System.stdout.println(\"outer a=\" + a + \" b=\" + b);\n"
    "        }\n"
    "        System.stdout.println(\"after a=\" + a + \" b=\" + b);\n"
    "        int32 n = 0;\n"
    "        try { n = 5; Main.boom(n); n = 9; } catch (Exception e) { n = n + 1; } finally { n = n * 2; }\n"
    "        System.stdout.println(\"n=\" + n);\n"
    "        int32 m = 0;\n"
    "        try {\n"
    "            try { m = 7; Main.boom(m); } finally { System.stdout.println(\"finally m=\" + m); }\n"
    "        } catch (Exception e) {\n"
    "            System.stdout.println(\"caught m=\" + m);\n"
    "        }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

const char* kShapes =
    "package app;\n"
    "import cajeta.error.Exception;\n"
    "public class Main {\n"
    "    static int32 boom(int32 v) {\n"
    "        if (v > 0) { throw heap Exception(\"b\"); }\n"
    "        return v;\n"
    "    }\n"
    "    static int32 add1(int32 x) { return x + 1; }\n"
    "    static int32 viaParam(int32 p) {\n"
    "        try { p = p + 10; Main.boom(p); } catch (Exception e) { return p; }\n"
    "        return -1;\n"
    "    }\n"
    "    static int32 loopInlined(int32 n) {\n"
    "        int32 acc = 0;\n"
    "        int32 i = 0;\n"
    "        try { while (i < n) { acc = Main.add1(acc); i = i + 1; } Main.boom(acc); }\n"
    "        catch (Exception e) { return acc; }\n"
    "        return -1;\n"
    "    }\n"
    "    static int32 inLoop(int32 n) {\n"
    "        int32 total = 0;\n"
    "        int32 k = 0;\n"
    "        while (k < n) {\n"
    "            int32 v = 0;\n"
    "            try { v = k + 1; Main.boom(v); } catch (Exception e) { total = total + v; }\n"
    "            k = k + 1;\n"
    "        }\n"
    "        return total;\n"
    "    }\n"
    "    public static async int32 worker(int32 k) {\n"
    "        int32 r = 0;\n"
    "        try { r = k * 2; Main.boom(r); } catch (Exception e) { return r; }\n"
    "        return -1;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        System.stdout.println(\"param \" + Main.viaParam(3) + \" loop \" + Main.loopInlined(20)\n"
    "            + \" inLoop \" + Main.inLoop(4));\n"
    "        Task<int32> t = spawn worker(4);\n"
    "        int32 got = await t;\n"
    "        System.stdout.println(\"task \" + got);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

/** Distinct `setjmp.pinned` allocas in the bodies of functions named `fn`. */
std::set<std::string> pinned(const Built& b, const std::string& fn) {
    std::string body = bodiesOf(dumpedIr(b), fn);
    static const std::regex name(R"((%setjmp\.pinned[0-9]*) = alloca)");
    std::set<std::string> out;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), name); it != std::sregex_iterator(); ++it)
        out.insert((*it)[1]);
    return out;
}

void expectAtEveryLevel(const char* src, const std::string& want) {
    Workspace w;
    w.write("src/app/Main.cajeta", src);
    int n = 0;
    for (const char* lvl : kLevels) {
        Built b = compile(w, "l" + std::to_string(n++), std::string("--emit=exe ") + lvl, "app.Main.main", "src");
        ASSERT_TRUE(b.ok) << lvl << "\n" << b.log;
        Ran r = run(b);
        EXPECT_EQ(r.rc, 0) << lvl << "\n" << r.err;
        EXPECT_EQ(r.out, want) << lvl;
    }
}

}  // namespace

// 1.1.1: spec 2.1, 2.2, 2.6.
TEST(TryCatchLocals, aThrowKeepsTheLastStoredValueAtEveryLevel) {
    expectAtEveryLevel(kBasic, "catch s=6 i=3\nafter s=6 i=3\n");
}

// 1.1.2, 1.1.3: spec 2.4, 2.5.
TEST(TryCatchLocals, nestedTryAndFinallyReadTheStoredValues) {
    expectAtEveryLevel(kNested,
        "inner a=1 b=2\nouter a=4 b=2\nafter a=4 b=2\nn=12\nfinally m=7\ncaught m=7\n");
}

// 1.1.4, 1.1.6: spec 2.3 and 1.3 (a parameter, an inlined callee's loop, a try in a
// loop, and a try inside a spawned task).
TEST(TryCatchLocals, parametersLoopsAndTasksKeepTheirValues) {
    expectAtEveryLevel(kShapes, "param 13 loop 20 inLoop 10\ntask 8\n");
}

// 1.1.5: spec 3.1, 3.2. Only s and i stay in memory: t is read only in the body, and
// pick has no try.
TEST(TryCatchLocals, onlyLocalsAThrowCanSeeStayInMemory) {
    Workspace w;
    w.write("src/app/Main.cajeta", kBasic);
    Built b = compile(w, "o2", "--emit=exe --opt=O2", "app.Main.main", "src");
    ASSERT_TRUE(b.ok) << b.log;
    EXPECT_EQ(pinned(b, "app.Main::main").size(), 2u) << bodiesOf(dumpedIr(b), "app.Main::main");
    EXPECT_TRUE(pinned(b, "app.Main::pick").empty());
}
