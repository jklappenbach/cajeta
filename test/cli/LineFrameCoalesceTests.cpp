// tensor-element-access Unit 8: once inlining has run, back-to-back line-frame pushes (and
// pops) in one block become one update of the stack top. Every frame is still written, so
// traces match the call path.

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include "AotProgram.h"

using namespace cajeta_test_aot;

namespace {

const char* kFill =
    "package app;\n"
    "import cajeta.math.Tensor;\n"
    "public class Main {\n"
    "    static void fill(Tensor<float64> t, int64 n) {\n"
    "        int64 i = 0;\n"
    "        while (i < n) { t.flatSet(i, (float64) i); i = i + 1; }\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int64[] shape = heap int64[1];\n"
    "        shape[0] = 64;\n"
    "        Tensor<float64> t #= Tensor.zeros<float64>(shape);\n"
    "        Main.fill(t, 64);\n"
    "        System.stdout.println(\"last \" + (int64) t.flatGet(63));\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

const char* kNested =
    "package app;\n"
    "import cajeta.error.Exception;\n"
    "import cajeta.error.StackFrame;\n"
    "public class Main {\n"
    "    static int32 inner(int32[] a, int32 i) {\n"
    "        if (i >= 3) { throw heap Exception(\"inner\"); }\n"
    "        return a[i];\n"
    "    }\n"
    "    static int32 adjacent(int32[] a, int32 i) { return Main.inner(a, i); }\n"
    "    static int32 marked(int32[] a, int32 i) {\n"
    "        int32 k = i + 1;\n"
    "        int32 r = Main.inner(a, k - 1);\n"
    "        return r;\n"
    "    }\n"
    "    static #String names(Exception e) {\n"
    "        StackFrame[] fs #= e.getStackTrace();\n"
    "        String out #= \"\" + fs.count() + \":\";\n"
    "        int64 i = 0;\n"
    "        while (i < fs.count() && i < 6) { out #= out + \" \" + fs[i].method + \"@\" + fs[i].line; i = i + 1; }\n"
    "        return #out;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int32[] a = [ 1, 2, 3 ];\n"
    "        int32 s = 0;\n"
    "        try { int32 i = 0; while (i < 5) { s = s + Main.adjacent(a, i); i = i + 1; } }\n"
    "        catch (Exception e) { System.stdout.println(\"adjacent \" + Main.names(e)); }\n"
    "        try { int32 i = 0; while (i < 5) { s = s + Main.marked(a, i); i = i + 1; } }\n"
    "        catch (Exception e) { System.stdout.println(\"marked \" + Main.names(e)); }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

const char* kDeep =
    "package app;\n"
    "import cajeta.error.Exception;\n"
    "import cajeta.error.StackFrame;\n"
    "public class Main {\n"
    "    static int32 boom(int32 i) {\n"
    "        if (i < 0) { throw heap Exception(\"neg\"); }\n"
    "        return i;\n"
    "    }\n"
    "    static int32 leafB(int32 i) { return i & 1; }\n"
    "    static int32 leafA(int32 i) { return Main.leafB(i); }\n"
    "    static int32 deep(int32 d) {\n"
    "        if (d == 0) { return 0; }\n"
    "        return Main.leafA(d) + Main.deep(d - 1);\n"
    "    }\n"
    "    static #String names(Exception e) {\n"
    "        StackFrame[] fs #= e.getStackTrace();\n"
    "        String out #= \"\" + fs.count() + \":\";\n"
    "        int64 i = 0;\n"
    "        while (i < fs.count() && i < 6) { out #= out + \" \" + fs[i].method; i = i + 1; }\n"
    "        return #out;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        int32 d = 505;\n"
    "        while (d < 520) { System.stdout.println(\"deep \" + Main.deep(d)); d = d + 1; }\n"
    "        try { Main.boom(-1); } catch (Exception e) { System.stdout.println(\"shallow \" + Main.names(e)); }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

/** Stores to the shadow stack's top in the bodies of functions named `fn`. */
int topStores(const Built& b, const std::string& fn) {
    std::string body = bodiesOf(dumpedIr(b), fn);
    static const std::regex store(R"(store (volatile )?i32 [^,]+, ptr %shadow\.top\.ptr)");
    return (int) std::distance(std::sregex_iterator(body.begin(), body.end(), store), std::sregex_iterator());
}

}  // namespace

// 8.1.1: flatSet and Storage.set inline into the loop; their two pushes and two pops become
// one top update each, so the loop function stores the top at most two thirds as often.
TEST(LineFrameCoalesce, inlinedFramesShareOneTopUpdate) {
    Workspace w;
    w.write("src/app/Main.cajeta", kFill);
    Built on = compile(w, "on", "--emit=exe --opt=O2", "app.Main.main", "src");
    ASSERT_TRUE(on.ok) << on.log;
    Built off = compile(w, "off", "--emit=exe --opt=O2", "app.Main.main", "src",
                        {{"CAJETA_LINE_FRAME_COALESCE", "0"}});
    ASSERT_TRUE(off.ok) << off.log;
    int a = topStores(on, "app.Main::fill"), b = topStores(off, "app.Main::fill");
    EXPECT_GT(a, 0) << bodiesOf(dumpedIr(on), "app.Main::fill");
    EXPECT_LE(3 * a, 2 * b) << "coalesced " << a << " vs " << b << ":\n" << bodiesOf(dumpedIr(on), "app.Main::fill");
    EXPECT_EQ(run(on).out, "last 63\n");
    EXPECT_EQ(run(off).out, "last 63\n");
}

// 8.1.2, 8.1.3: a throw two inlined levels deep shows both frames, and under
// --debug-info=full their lines, whether the enters are adjacent or a statement sits between.
TEST(LineFrameCoalesce, nestedInlinedTracesMatchTheCallPath) {
    Workspace w;
    w.write("src/app/Main.cajeta", kNested);
    for (std::string flags : {"--emit=exe --opt=O2", "--emit=exe --opt=O2 --debug-info=full"}) {
        Built on = compile(w, "on", flags, "app.Main.main", "src");
        ASSERT_TRUE(on.ok) << on.log;
        Built calls = compile(w, "calls", flags, "app.Main.main", "src",
                              {{"CAJETA_INLINE_LINE_FRAMES", "0"}, {"CAJETA_XMODULE_INLINE", "0"}});
        ASSERT_TRUE(calls.ok) << calls.log;
        Ran a = run(on), b = run(calls);
        EXPECT_EQ(a.rc, 0) << a.err;
        EXPECT_EQ(a.out, b.out) << flags;
        bool full = flags.find("full") != std::string::npos;
        EXPECT_NE(a.out.find(full ? "inner@6 adjacent@9 main@" : "inner@0 adjacent@0 main@"),
                  std::string::npos) << flags << "\n" << a.out;
        EXPECT_NE(a.out.find(full ? "inner@6 marked@12 main@" : "inner@0 marked@0 main@"),
                  std::string::npos) << flags << "\n" << a.out;
    }
}

// 8.1.4: a merged push that straddles the shadow stack's capacity stays balanced: after
// recursion past 512 frames, each level with two inlined frames, a throw shows its own frames.
TEST(LineFrameCoalesce, aMergedPushAtTheCapacityStaysBalanced) {
    Workspace w;
    w.write("src/app/Main.cajeta", kDeep);
    Built on = compile(w, "on", "--emit=exe --opt=O2", "app.Main.main", "src");
    ASSERT_TRUE(on.ok) << on.log;
    Built calls = compile(w, "calls", "--emit=exe --opt=O2", "app.Main.main", "src",
                          {{"CAJETA_INLINE_LINE_FRAMES", "0"}, {"CAJETA_XMODULE_INLINE", "0"}});
    ASSERT_TRUE(calls.ok) << calls.log;
    EXPECT_GT(topStores(on, "app.Main::deep"), 0);
    Ran a = run(on), b = run(calls);
    EXPECT_EQ(a.rc, 0) << a.err;
    EXPECT_EQ(a.out, b.out);
    EXPECT_NE(a.out.find("deep 260\n"), std::string::npos) << a.out;
    EXPECT_NE(a.out.find("shallow 2: boom main\n"), std::string::npos) << a.out;
}
