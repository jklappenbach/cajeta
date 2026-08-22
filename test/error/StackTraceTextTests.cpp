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
