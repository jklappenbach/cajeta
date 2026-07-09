// diagnostic-exceptions Unit 3 (3.1.3): an uncaught throw prints a semantic text
// trace — `Package.Class.method(File.cajeta:NN)` with real line numbers — driven
// end-to-end through the built binary via `cajeta jit-run`. The runtime text
// path (__cajeta_print_trace over the shadow snapshot) is identical for
// --emit=exe; a true AOT-exe trace is attached to the plan separately (lld).

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_ST_DEVNULL "NUL"
#else
#  define CAJETA_ST_DEVNULL "/dev/null"
#endif

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

int runJitCapturingStderr(const fs::path& proj, std::string& err) {
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return -1;
    auto errFile = proj / "stderr.txt";
    std::string cmd = bin + " jit-run " + proj.string() + " test.App.run"
                      " > " CAJETA_ST_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream in(errFile);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return rc;
}

} // namespace

// 3.1.3 — the uncaught trace prints test.App.run(App.cajeta:NN) with a real
// (positive) line number.
TEST(StackTraceText, uncaughtPrintsSemanticFrame) {
    auto proj = writeProject(freshTempDir("sem"),
                             "throw heap Exception(\"boom\");");
    std::string err;
    int rc = runJitCapturingStderr(proj, err);
    if (rc == -1) GTEST_SKIP() << "compiler binary unavailable";

    EXPECT_NE(rc, 0) << "an uncaught throw must fail the run";
    // e.g. "  at test.App.run(App.cajeta:5)"
    std::regex frame(R"(test\.App\.run\(App\.cajeta:(\d+)\))");
    std::smatch m;
    ASSERT_TRUE(std::regex_search(err, m, frame))
        << "expected a semantic frame in the trace; stderr:\n" << err;
    EXPECT_GT(std::stoi(m[1].str()), 0) << "line number must be positive";
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
    std::regex frame(R"(test\.App\.deep\(App\.cajeta:(\d+)\))");
    EXPECT_TRUE(std::regex_search(err, frame))
        << "expected a semantic frame for the throw site; stderr:\n" << err;
    // Message precedes frames.
    EXPECT_LT(err.find("outer failure"), err.find("at test.App.deep"))
        << "message must precede frames; stderr:\n" << err;
}

// ExceptionReview 5.7, BLOCKED on 5.8. The native helper
// (__cajeta_print_trace_one) already carries the "Caused by: " prefix, but the
// Cajeta-side walk cannot ship: Exception.getCause() passes its borrowed `cause`
// field to Optional's owning `#T` ctor, so a loop-scoped Optional drops and frees
// the cause. A chain of depth >= 2 then reads freed memory (verified SIGSEGV).
// Throwable.toJson() has the same latent defect; its only nested-cause test is
// one level deep. Re-enable once 5.8 lands.
TEST(StackTraceText, DISABLED_printStackTracePrintsCauseChain) {
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
    std::regex frame(R"(test\.App\.deep\(App\.cajeta:(\d+)\))");
    EXPECT_TRUE(std::regex_search(err, frame))
        << "expected a semantic frame for the throw site; stderr:\n" << err;
    // Ordering: the cause link must follow the top throwable's message.
    EXPECT_LT(err.find("outer failure"), err.find("Caused by: root cause"))
        << "cause must print after the wrapping throwable; stderr:\n" << err;
}
