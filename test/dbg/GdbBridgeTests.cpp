// external-debug Unit 5 — the gdb bridge, driven headlessly.
//
// This is the acceptance test for the whole spec (§7.1): build a program with
// --debug-info=full, attach gdb with tools/gdb/cajeta_gdb.py loaded, and check
// that a Cajeta developer can actually debug it — break on a source line, read a
// semantic stack, inspect locals with their dynamic types and ownership, and step
// one STATEMENT at a time. All of it with no DWARF anywhere in the binary.
//
// Pins:
//   5.1.1  cjbreak File.cajeta:<line> + run stops at that line.
//   5.1.2  cjstack names the frame with its file and line.
//   5.1.3  cjlocals prints name, type, value, allocation kind, ownership.
//   5.1.4  cjstep advances exactly one Cajeta statement.
//   5.1.5  On an --debug-info=off binary every command says "rebuild".
//   5.1.6  cjbreak arms PAST the prologue, so the current frame is present.
//   5.1.10 A static field (byteOffset == -1) is reported, not mis-decoded.

#include "gtest/gtest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

namespace {

    namespace fs = std::filesystem;

    std::string sourceRoot() {
        const char* env = std::getenv("CAJETA_SOURCE_ROOT");
        if (env && *env) return env;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        return CAJETA_SOURCE_ROOT_DEFAULT;
#else
        return ".";
#endif
    }

    std::string compilerPath() { return sourceRoot() + "/build/src/cajeta"; }
    std::string bridgePath()   { return sourceRoot() + "/tools/gdb/cajeta_gdb.py"; }

    bool haveGdb() {
        return std::system("gdb --version > /dev/null 2>&1") == 0;
    }

    std::string run(const std::string& cmd) {
        std::string out;
        FILE* p = popen((cmd + " 2>&1").c_str(), "r");
        if (!p) return out;
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        return out;
    }

    struct Fixture {
        fs::path base, src, build;
    };

    // A program with: a subclass held in a base-typed local (dynamic type), an
    // owned heap object (ownership), a static field (byteOffset -1), and several
    // statements in a row (stepping).
    Fixture makeFixture(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto base = fs::temp_directory_path()
                  / ("cajeta_gdb_" + tag + "_" + std::to_string(rng()));
        fs::create_directories(base / "src" / "demo");
        fs::create_directories(base / "build");
        std::ofstream out(base / "src" / "demo" / "Hello.cajeta");
        out << "package demo;\n"                                    // 1
            << "public class Shape {\n"                             // 2
            << "    public int32 sides;\n"                          // 3
            << "    public Shape() { this.sides = 0; }\n"           // 4
            << "}\n"                                                // 5
            << "public final class Circle extends Shape {\n"        // 6
            << "    public float64 radius;\n"                       // 7
            << "    public Circle() { this.sides = 7; this.radius = 2.5; }\n"
            << "}\n"                                                // 9
            << "public final class Hello {\n"                       // 10
            << "    public static int32 run() {\n"                  // 11
            << "        Shape s = heap Circle();\n"                 // 12
            << "        int32 n = s.sides;\n"                       // 13
            << "        int32 total = 0;\n"                         // 14
            << "        total = total + n;\n"                       // 15
            << "        total = total + 1;\n"                       // 16
            << "        return total;\n"                            // 17
            << "    }\n"                                            // 18
            << "}\n";                                               // 19
        return Fixture{base, base / "src", base / "build"};
    }

    bool build(const Fixture& f, const std::string& level) {
        std::string cmd = compilerPath() + " --debug-info=" + level
            + " --emit=exe demo.Hello.run " + f.src.string() + " "
            + f.build.string() + " > /dev/null 2>&1";
        return std::system(cmd.c_str()) == 0
            && fs::exists(f.build / "a.out");
    }

    // Drive gdb with a script. Everything before `run` must not call into the
    // inferior: with no process there is nothing to call.
    std::string gdb(const Fixture& f, const std::string& script) {
        auto path = f.base / "drive.gdb";
        std::ofstream out(path);
        out << "set confirm off\n"
            << "source " << bridgePath() << "\n"
            << script;
        out.close();
        return run("gdb -batch -nx -x " + path.string() + " "
                   + (f.build / "a.out").string());
    }

    bool has(const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    }

} // namespace

class GdbBridge : public ::testing::Test {
protected:
    void SetUp() override {
        if (!haveGdb()) GTEST_SKIP() << "gdb not installed";
        if (!fs::exists(bridgePath()))
            GTEST_SKIP() << "bridge script not found at " << bridgePath();
    }
};

// 5.1.1 + 5.1.2 + 5.1.6 — break on a source line, and at the stop the CURRENT
// frame is on the stack with its file and line. (Breaking on the function's
// entry symbol instead would show the callers but not this frame: the prologue's
// __cajeta_line_enter has not run yet — spec §5.1.4.)
TEST_F(GdbBridge, cjbreakStopsAtTheLineAndCjstackNamesIt) {
    auto f = makeFixture("break");
    ASSERT_TRUE(build(f, "full"));

    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:13\ncontinue\n"
                      "cjstack\nkill\nquit\n");

    EXPECT_TRUE(has(out, "cjbreak Hello.cajeta:13")) << out;
    EXPECT_TRUE(has(out, "demo.Hello.run(demo/Hello.cajeta:13)"))
        << "cjstack did not name the frame we stopped in:\n" << out;

    fs::remove_all(f.base);
}

// 5.1.3 — locals with name, declared type, DYNAMIC type, value, allocation kind
// and ownership. The last two are what DWARF cannot express (spec §4.1.7).
TEST_F(GdbBridge, cjlocalsShowsDynamicTypeValuesAndOwnership) {
    auto f = makeFixture("locals");
    ASSERT_TRUE(build(f, "full"));

    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:14\ncontinue\n"
                      "cjlocals\nkill\nquit\n");

    // Declared Shape, actually a Circle (spec §4.2.2).
    EXPECT_TRUE(has(out, "s : demo.Shape")) << out;
    EXPECT_TRUE(has(out, "dynamic: demo.Circle")) << out;
    // Three facets: where it lives, who owns it, and its lifetime state AT THIS
    // STOP. `about-to-drop` = a live owner still on the drop chain.
    EXPECT_TRUE(has(out, "[heap, owner, about-to-drop]")) << out;
    // Fields decoded from byteOffset + typeFlags — Circle's own, and the one it
    // INHERITS (a class's RTTI carries only its own fields).
    EXPECT_TRUE(has(out, ".radius")) << out;
    EXPECT_TRUE(has(out, "2.5")) << out;
    EXPECT_TRUE(has(out, "inherited from demo.Shape")) << out;
    EXPECT_TRUE(has(out, ".sides")) << out;
    EXPECT_TRUE(has(out, "= 7")) << out;
    // A primitive local reads through its declared type.
    EXPECT_TRUE(has(out, "n : int32 = 7")) << out;

    fs::remove_all(f.base);
}

// 5.1.4 — one cjstep = one Cajeta STATEMENT, not one machine instruction.
TEST_F(GdbBridge, cjstepAdvancesOneStatement) {
    auto f = makeFixture("step");
    ASSERT_TRUE(build(f, "full"));

    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:14\ncontinue\n"
                      "cjstep\ncjstep\ncjlocals\nkill\nquit\n");

    // 14 -> 15 -> 16, one statement per step.
    EXPECT_TRUE(has(out, "demo/Hello.cajeta:15")) << out;
    EXPECT_TRUE(has(out, "demo/Hello.cajeta:16")) << out;
    // `total = total + n` has run (0 + 7), `total + 1` has not.
    EXPECT_TRUE(has(out, "total : int32 = 7")) << out;

    fs::remove_all(f.base);
}

// 5.1.5 — a release-style binary carries no debug records. Every command must
// say so, and say how to fix it, rather than crash or print nonsense.
TEST_F(GdbBridge, offBinaryTellsYouToRebuild) {
    auto f = makeFixture("off");
    ASSERT_TRUE(build(f, "off"));

    auto out = gdb(f, "break main\nrun\ncjlocals\nkill\nquit\n");

    EXPECT_TRUE(has(out, "--debug-info")) << out;
    EXPECT_TRUE(has(out, "no debug records") || has(out, "Rebuild")) << out;

    fs::remove_all(f.base);
}

// 5.1.8 — a String local renders its CONTENTS, and an array its element count,
// rather than an address. String's layout is tagged (inline under 12 bytes,
// windowed root beyond), which gdb cannot decode with no DWARF — the runtime
// hands the bridge the length and the bytes.
TEST_F(GdbBridge, stringAndArrayLocalsRenderContents) {
    auto f = makeFixture("string");
    // Overwrite with a String-carrying program.
    {
        std::ofstream out(f.src / "demo" / "Hello.cajeta");
        out << "package demo;\n"                              // 1
            << "public final class Hello {\n"                 // 2
            << "    public static int32 run() {\n"            // 3
            << "        String greeting = \"hello debugger\";\n"   // 4
            << "        int32[] xs = heap int32[3];\n"        // 5
            << "        int32 k = 1;\n"                       // 6
            << "        return k;\n"                          // 7
            << "    }\n"
            << "}\n";
    }
    ASSERT_TRUE(build(f, "full"));

    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:7\ncontinue\n"
                      "cjlocals\nkill\nquit\n");

    EXPECT_TRUE(has(out, "\"hello debugger\"")) << out;
    EXPECT_TRUE(has(out, "3 element(s)")) << out;

    fs::remove_all(f.base);
}

// 5.1.9 — a moved-from local reports its state, not a stale pointer rendered as
// if it were live (spec §4.2.4). This is the reason the encoding exists: DWARF
// cannot say "ownership was transferred out of this binding", so a DWARF-driven
// debugger would happily print the consumed value.
//
// (The facet values are OwnershipRole in src/cajeta/dbg/MemoryFacets.h:
// Unknown=0, Owner=1, Borrow=2, TransferredOut=3. Getting them wrong renders a
// moved-from local as something else entirely — hence this test.)
TEST_F(GdbBridge, movedFromLocalReportsItsOwnershipState) {
    auto f = makeFixture("moved");
    {
        std::ofstream out(f.src / "demo" / "Hello.cajeta");
        out << "package demo;\n"                                  // 1
            << "public final class Box {\n"                       // 2
            << "    public int32 v;\n"                            // 3
            << "    public Box() { this.v = 3; }\n"               // 4
            << "}\n"                                              // 5
            << "public final class Hello {\n"                     // 6
            << "    public static int32 take(Box b) { return b.v; }\n"  // 7
            << "    public static int32 run() {\n"                // 8
            << "        Box owned = heap Box();\n"                // 9
            << "        int32 r = take(#owned);\n"                // 10
            << "        int32 done = r;\n"                        // 11
            << "        return done;\n"                           // 12
            << "    }\n"
            << "}\n";
    }
    ASSERT_TRUE(build(f, "full"));

    // Stop AFTER the `#owned` transfer.
    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:12\ncontinue\n"
                      "cjlocals\nkill\nquit\n");

    EXPECT_TRUE(has(out, "owned")) << out;
    EXPECT_TRUE(has(out, "moved-from")) << out
        << "\na moved-from local must not render as a live value";

    fs::remove_all(f.base);
}

// 5.1.10 — a static field has byteOffset -1: it lives in a global, not in the
// instance. It must be REPORTED as unsupported, never decoded at a bogus offset
// (spec §4.1.8).
//
// (5.1.7, cycle termination, has no test here: an OWNED cycle turns out to be
// unconstructible in safe Cajeta. `a.peer = a` MOVES `a` into the field —
// ownership is linear — so the local is consumed and renders as moved-from,
// never as a cyclic graph. The visited set in the renderer stays as defense
// against walking corrupted memory, but nothing in the language can exercise it.)
TEST_F(GdbBridge, staticFieldIsReportedNotMisdecoded) {
    auto f = makeFixture("static");
    {
        std::ofstream out(f.src / "demo" / "Hello.cajeta");
        out << "package demo;\n"                                // 1
            << "public final class Node {\n"                    // 2
            << "    public static int32 count;\n"               // 3  byteOffset -1
            << "    public int32 id;\n"                         // 4
            << "    public Node() { this.id = 1; }\n"           // 5
            << "}\n"                                            // 6
            << "public final class Hello {\n"                   // 7
            << "    public static int32 run() {\n"              // 8
            << "        Node a = heap Node();\n"                // 9
            << "        int32 z = a.id;\n"                      // 10
            << "        return z;\n"                            // 11
            << "    }\n"
            << "}\n";
    }
    ASSERT_TRUE(build(f, "full"));

    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:11\ncontinue\n"
                      "cjlocals\nkill\nquit\n");

    EXPECT_TRUE(has(out, ".count = <static — not supported>")) << out;
    // ...while the instance field beside it decodes normally.
    EXPECT_TRUE(has(out, ".id @+8 = 1")) << out;

    fs::remove_all(f.base);
}

// cjbreak on a line that emits no code fails with a real explanation, not a
// silent no-op that leaves the user waiting at a breakpoint that never fires.
TEST_F(GdbBridge, cjbreakOnANonStatementLineExplainsItself) {
    auto f = makeFixture("noline");
    ASSERT_TRUE(build(f, "full"));

    // Line 1 is `package demo;` — no statement, no loc_id.
    auto out = gdb(f, "break main\nrun\ncjbreak Hello.cajeta:1\nkill\nquit\n");

    EXPECT_TRUE(has(out, "no statement at")) << out;

    fs::remove_all(f.base);
}
