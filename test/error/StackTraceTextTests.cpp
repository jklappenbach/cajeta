// diagnostic-exceptions Unit 3 (3.1.3): an uncaught throw prints a semantic text
// trace — `Package.Class.method(File.cajeta:NN)` — driven end-to-end through
// the built binary via `cajeta jit-run`. The `:NN` is a `-g` feature since the
// per-statement mark was measured at 3.5-9.4x; the frame's type/method/file is
// free and stays on by default. See runJitCapturingStderr. The runtime text
// path (__cajeta_print_trace over the shadow snapshot) is identical for
// --emit=exe; a true AOT-exe trace is attached to the plan separately (lld).

#include <gtest/gtest.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_ST_DEVNULL "NUL"
#else
#  define CAJETA_ST_DEVNULL "/dev/null"
#endif

// Line number out of a trace frame: given `prefix` = "test.App.run(App.cajeta:",
// find `<prefix><digits>)` in `text` and return the digits. -1 if there is no
// such frame, 0 is never returned for a well-formed one — so `> 0` asserts both
// "the frame is present" and "its line number is real", which is what the tests
// want to say.
//
// Hand-rolled rather than std::regex, and NOT a style preference: <regex> in this
// TU emitted libstdc++'s _AnyMatcher vague-linkage statics, which collided at
// link time with the copies inside liblldCommon.a and broke the mingw release
// target (`multiple definition of ...::__nul`). See the commit message — the
// underlying toolchain mixing on the Windows runner is a separate, unfixed bug.
int frameLine(const std::string& text, const std::string& prefix) {
    auto at = text.find(prefix);
    if (at == std::string::npos) return -1;
    size_t p = at + prefix.size(), q = p;
    while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q]))) ++q;
    if (q == p || q >= text.size() || text[q] != ')') return -1;
    return std::stoi(text.substr(p, q - p));
}

// Every line for `prefix`, in the order the frames were printed. frameLine above
// answers "the first one", which cannot distinguish two lambdas in one method
// from one lambda reported twice — the question 1.1.4 and 1.1.5 have to ask.
std::vector<int> allFrameLines(const std::string& text, const std::string& prefix) {
    std::vector<int> out;
    size_t at = 0;
    while ((at = text.find(prefix, at)) != std::string::npos) {
        size_t p = at + prefix.size(), q = p;
        while (q < text.size() && std::isdigit(static_cast<unsigned char>(text[q]))) ++q;
        if (q > p && q < text.size() && text[q] == ')') {
            out.push_back(std::stoi(text.substr(p, q - p)));
        }
        at = (q > at) ? q : at + 1;
    }
    return out;
}

std::string compilerBinary() {
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

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_sttext_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

fs::path writeProject(const fs::path& root, const std::string& body) {
    auto dir = root / "test";
    fs::create_directories(dir);
    std::ofstream src(dir / "App.cajeta");
    src << "package test;\n"
           "import cajeta.error.Exception;\n"
           "public final class App {\n"
           "    public static void run() {\n"
        << "        " << body << "\n"
           "    }\n"
           "}\n";
    src.close();
    return root;
}

// Same as writeProject but the caller supplies the whole class body, so a test
// can declare helper methods and build a cause chain.
fs::path writeProjectClass(const fs::path& root, const std::string& classBody) {
    auto dir = root / "test";
    fs::create_directories(dir);
    std::ofstream src(dir / "App.cajeta");
    src << "package test;\n"
           "import cajeta.error.Exception;\n"
           "public final class App {\n"
        << classBody
        << "}\n";
    src.close();
    return root;
}

// Same as writeProjectClass but the caller supplies extra imports. The lambda
// tests need a collection to drive a stream through, and the fixed preamble
// above only imports Exception.
fs::path writeProjectClassImporting(const fs::path& root,
                                    const std::string& imports,
                                    const std::string& classBody) {
    auto dir = root / "test";
    fs::create_directories(dir);
    std::ofstream src(dir / "App.cajeta");
    src << "package test;\n"
           "import cajeta.error.Exception;\n"
        << imports
        << "public final class App {\n"
        << classBody
        << "}\n";
    src.close();
    return root;
}

// `withLines` passes -g, which is jit-run's spelling of --debug-info=full.
//
// It has to be a parameter now. Per-statement line marks became a
// full-debug-info feature when they were measured at 3.5-9.4x (see
// LineInfoCodegen::emitLineMark), so a DEFAULT build resolves a frame to
// Type.method(File.cajeta) and carries no line — `:0`, which StackFrame
// documents as "line-info unavailable". Both halves are asserted below rather
// than one being dropped: the semantic frame is what these tests are named
// for and it is still free, and the exact line is still available on request.
int runJitCapturingStderr(const fs::path& proj, std::string& err,
                          bool withLines = false) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = proj / "stderr.txt";
    std::string cmd = bin + " jit-run " + (withLines ? "-g " : "")
                      + proj.string() + " test.App.run"
                      " > " CAJETA_ST_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

} // namespace

// 3.1.3 — the uncaught trace prints a SEMANTIC frame,
// test.App.run(App.cajeta:...), rather than an address. This is the half that
// every build gets: the per-CALL probe that carries type/method/file measured
// at parity with an uninstrumented build, so it stays on by default.
TEST(StackTraceText, uncaughtPrintsSemanticFrame) {
    auto proj = writeProject(freshTempDir("sem"),
                             "throw heap Exception(\"boom\");");
    std::string err;
    int rc = runJitCapturingStderr(proj, err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    // >= 0 rather than > 0: frameLine returns -1 when the frame is ABSENT, so
    // this still asserts the frame is there and well-formed. The default build
    // carries no line (see runJitCapturingStderr).
    EXPECT_GE(frameLine(err, "test.App.run(App.cajeta:"), 0)
        << "expected a semantic frame naming type, method and file; stderr:\n" << err;
}

// The other half, on request. Asserted as a PAIR with the test above so the
// trade stays pinned from both sides: dropping per-statement marks entirely
// would pass the default-build test alone, and turning them back on
// everywhere would pass this one alone.
TEST(StackTraceText, debugInfoFullAddsTheExactLine) {
    auto proj = writeProject(freshTempDir("semg"),
                             "throw heap Exception(\"boom\");");
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/true);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    // e.g. "  at test.App.run(App.cajeta:5)"
    EXPECT_GT(frameLine(err, "test.App.run(App.cajeta:"), 0)
        << "expected a positive line number under -g; stderr:\n" << err;
}

// ExceptionReview 5.7 — printStackTrace() prints the throwable's own message
// before its frames. It previously printed frames with no message line at all.
TEST(StackTraceText, printStackTracePrintsMessageThenFrames) {
    auto proj = writeProjectClass(freshTempDir("msg"),
        "    public static void run() {\n"
        "        try {\n"
        "            deep();\n"
        "        } catch (Exception e) {\n"
        "            e.printStackTrace();\n"
        "        }\n"
        "    }\n"
        "    private static void deep() {\n"
        "        throw heap Exception(\"outer failure\");\n"
        "    }\n");
    std::string err;
    int rc = runJitCapturingStderr(proj, err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "the throw is caught; the run must succeed. stderr:\n" << err;
    EXPECT_NE(err.find("outer failure"), std::string::npos)
        << "message line missing; stderr:\n" << err;
    EXPECT_GE(frameLine(err, "test.App.deep(App.cajeta:"), 0)
        << "expected a semantic frame for the throw site; stderr:\n" << err;
    // Message precedes frames.
    EXPECT_LT(err.find("outer failure"), err.find("at test.App.deep"))
        << "message must precede frames; stderr:\n" << err;
}

// ExceptionReview 5.7 / optional-borrow-ownership 5.1.2 — printStackTrace() walks
// getCause(), emitting a "Caused by: " link per level. The native helper
// (__cajeta_print_trace_one) always carried the prefix but had zero call sites.
// The walk itself was blocked until Optional's `#T` ctor became mode-dependent:
// before that, the loop-scoped Optional dropped and freed the borrowed cause.
TEST(StackTraceText, printStackTracePrintsCauseChain) {
    auto proj = writeProjectClass(freshTempDir("cause"),
        "    public static void run() {\n"
        "        try {\n"
        "            deep();\n"
        "        } catch (Exception e) {\n"
        "            e.printStackTrace();\n"
        "        }\n"
        "    }\n"
        "    private static void deep() {\n"
        "        throw heap Exception(\"outer failure\",\n"
        "            heap Exception(\"root cause\"));\n"
        "    }\n");
    std::string err;
    int rc = runJitCapturingStderr(proj, err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "the throw is caught; the run must succeed. stderr:\n" << err;
    EXPECT_NE(err.find("outer failure"), std::string::npos)
        << "top throwable's message missing; stderr:\n" << err;
    EXPECT_NE(err.find("Caused by: root cause"), std::string::npos)
        << "cause chain not printed; stderr:\n" << err;
    // The top throwable was thrown, so it carries semantic frames. The cause was
    // constructed but never thrown -> no frames, per throw-site capture (3.1).
    EXPECT_GE(frameLine(err, "test.App.deep(App.cajeta:"), 0)
        << "expected a semantic frame for the throw site; stderr:\n" << err;
    // Ordering: the cause link must follow the top throwable's message.
    EXPECT_LT(err.find("outer failure"), err.find("Caused by: root cause"))
        << "cause must print after the wrapping throwable; stderr:\n" << err;
}

// ---------------------------------------------------------------------------
// lambda-frame-line — a lambda's shadow frame must carry a real source line.
//
// The frame is pushed by LambdaExpression::generateCode and was only ever given
// a line by the statement marks its BODY emits. Block::generateCode is the only
// mark site, so an expression body — `(x) -> f(x)`, the common form — left the
// frame on the zero it was pushed with. Measured 2026-08-31, same program and
// same -g build: block body `<lambda>(App.cajeta:10)`, expression body
// `<lambda>(App.cajeta:0)`, every neighbouring frame real.
//
// Neither this suite nor ProfilerEndToEndTests contained a lambda, which is why
// it went unseen: the frame-push shipped with a comment asserting the behaviour
// and nothing measuring it.
// ---------------------------------------------------------------------------

namespace {
    // The preamble writeProjectClassImporting emits is 4 lines, so a class body
    // passed to it starts at line 5. Every expected line below is counted from
    // there, and named rather than spelled twice.
    const char* kListImport = "import cajeta.collection.ArrayList;\n";

    const char* kBoom =
        "    public static void boom(int32 x) {\n"
        "        throw heap Exception(\"boom\");\n"
        "    }\n";

    const char* kLambdaFrame = "test.App.<lambda>(App.cajeta:";
}

// 1.1.1 (spec 2.1, 3.1) — the defect. RED before the fix at `:0`.
TEST(StackTraceText, expressionBodiedLambdaFrameHasARealLine) {
    auto proj = writeProjectClassImporting(freshTempDir("lamexpr"), kListImport,
        "    public static void run() {\n"                                  // 5
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"           // 6
        "        xs.add(1);\n"                                              // 7
        "        xs.stream().forEach((x) -> App.boom(x));\n"                // 8
        "    }\n"                                                           // 9
        + std::string(kBoom));
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/true);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    // frameLine returns -1 when the frame is ABSENT and 0 when it is present
    // with no line, so this one assertion separates the two (spec 4.1).
    EXPECT_EQ(frameLine(err, kLambdaFrame), 8)
        << "the lambda frame must carry the line its body sits on; stderr:\n" << err;
}

// 1.1.2 (spec 2.2) — the control arm. Green before AND after: deleting the
// statement marks entirely would fail this while 1.1.1 still passed, and
// reverting the fix fails 1.1.1 while this still passes (spec 4.2).
TEST(StackTraceText, blockBodiedLambdaFrameHasARealLine) {
    auto proj = writeProjectClassImporting(freshTempDir("lamblock"), kListImport,
        "    public static void run() {\n"                                  // 5
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"           // 6
        "        xs.add(1);\n"                                              // 7
        "        xs.stream().forEach((x) -> {\n"                            // 8
        "            App.boom(x);\n"                                        // 9
        "        });\n"                                                     // 10
        "    }\n"                                                           // 11
        + std::string(kBoom));
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/true);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    // The statement inside the block, which is what the frame advances to.
    EXPECT_EQ(frameLine(err, kLambdaFrame), 9)
        << "a block body's frame must report its running statement; stderr:\n" << err;
}

// 1.1.3 (spec 2.6) — the line is the BODY's, not the parameter list's. Pins
// which of the two the fix reads: they are the same line in every other test
// here, so only a split body can tell them apart.
TEST(StackTraceText, lambdaLineIsTheBodyNotTheParameterList) {
    auto proj = writeProjectClassImporting(freshTempDir("lamsplit"), kListImport,
        "    public static void run() {\n"                                  // 5
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"           // 6
        "        xs.add(1);\n"                                              // 7
        "        xs.stream().forEach((x) ->\n"                              // 8
        "            App.boom(x));\n"                                       // 9
        "    }\n"                                                           // 10
        + std::string(kBoom));
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/true);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    EXPECT_EQ(frameLine(err, kLambdaFrame), 9)
        << "expected the body's line (9), not the arrow's (8); stderr:\n" << err;
}

// 1.1.4 (spec 2.5) — two lambdas in one method stay distinguishable, which is
// the whole point of giving a lambda its own frame. Both throws are caught so
// both traces print.
TEST(StackTraceText, twoLambdasInOneMethodReportDifferentLines) {
    auto proj = writeProjectClassImporting(freshTempDir("lamtwo"), kListImport,
        "    public static void run() {\n"                                  // 5
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"           // 6
        "        xs.add(1);\n"                                              // 7
        "        try {\n"                                                   // 8
        "            xs.stream().forEach((x) -> App.boom(x));\n"            // 9
        "        } catch (Exception e) {\n"                                 // 10
        "            e.printStackTrace();\n"                                // 11
        "        }\n"                                                       // 12
        "        try {\n"                                                   // 13
        "            xs.stream().forEach((x) -> App.boom(x));\n"            // 14
        "        } catch (Exception e) {\n"                                 // 15
        "            e.printStackTrace();\n"                                // 16
        "        }\n"                                                       // 17
        "    }\n"                                                           // 18
        + std::string(kBoom));
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/true);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_EQ(rc, 0) << "both throws are caught; the run must succeed. stderr:\n" << err;
    auto lines = allFrameLines(err, kLambdaFrame);
    ASSERT_GE(lines.size(), 2u)
        << "expected a lambda frame from each of the two traces; stderr:\n" << err;
    EXPECT_EQ(lines[0], 9) << "stderr:\n" << err;
    EXPECT_EQ(lines[1], 14) << "stderr:\n" << err;
}

// 1.1.5 (spec 2.4) — nested lambdas each carry their own line. Split across
// lines deliberately: written on one line the two frames would be
// indistinguishable even when both are correct, and the test could not fail.
TEST(StackTraceText, nestedLambdasEachCarryTheirOwnLine) {
    auto proj = writeProjectClassImporting(freshTempDir("lamnest"), kListImport,
        "    public static void run() {\n"                                  // 5
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"           // 6
        "        xs.add(1);\n"                                              // 7
        "        xs.stream().forEach((x) ->\n"                              // 8
        "            xs.stream().forEach((y) ->\n"                          // 9
        "                App.boom(y)));\n"                                  // 10
        "    }\n"                                                           // 11
        + std::string(kBoom));
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/true);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    auto lines = allFrameLines(err, kLambdaFrame);
    ASSERT_GE(lines.size(), 2u)
        << "expected an inner and an outer lambda frame; stderr:\n" << err;
    // Innermost frame prints first: inner body on 10, outer body on 9.
    EXPECT_EQ(lines[0], 10) << "inner lambda; stderr:\n" << err;
    EXPECT_EQ(lines[1], 9) << "outer lambda; stderr:\n" << err;
}

// 1.1.6 (spec 2.7) — the opt-out still holds. Per-statement marks are a
// full-debug-info feature (measured 3.5-9.4x), and stamping the frame at its
// push must not slip a line into a build that declined to pay for one.
TEST(StackTraceText, withoutLineInfoALambdaFrameReportsZero) {
    auto proj = writeProjectClassImporting(freshTempDir("lamnog"), kListImport,
        "    public static void run() {\n"
        "        ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "        xs.add(1);\n"
        "        xs.stream().forEach((x) -> App.boom(x));\n"
        "    }\n"
        + std::string(kBoom));
    std::string err;
    int rc = runJitCapturingStderr(proj, err, /*withLines=*/false);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    // 0, not -1: the semantic frame is still there (it is free), it just
    // carries no line.
    EXPECT_EQ(frameLine(err, kLambdaFrame), 0)
        << "a default build must report no line; stderr:\n" << err;
}
