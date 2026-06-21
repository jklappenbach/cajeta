// Tests for cajeta.process — U1 scope: one-shot Command.run() + ProcessResult.
//
// Each test JIT-compiles a small cajeta program whose static `entry()` builds a
// Command, runs a real tiny well-known program (/bin/true, /bin/false,
// /bin/echo, /bin/cat, /bin/sh, /bin/pwd), and returns an int that encodes the
// assertion outcome (0 == pass). The entry method is named `entry` (not `run`)
// so its JIT symbol doesn't collide with Command.run().
//
// POSIX-only: the bridge is posix_spawn-based and these paths assume a POSIX
// layout. The suite already contains POSIX-gated tests; mirror that.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/buildtool/Subprocess.h"

#include <cstdint>
#include <filesystem>
#include <string>

using cajeta_test::CajetaJit;

namespace {

#ifndef _WIN32

std::string wrap(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.process.Command;\n"
        "import cajeta.process.ProcessResult;\n"
        "import cajeta.lang.String;\n"
        "public class P {\n"
        "    public static int32 entry() {\n"
        + body +
        "    }\n"
        "}\n";
}

int32_t runEntry(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.P");
    auto fn = jit->lookup<int32_t (*)()>("entry");
    if (!fn) {
        ADD_FAILURE() << "entry() symbol not found in JIT module";
        return -999;
    }
    return fn();
}

// Bodies reused across tests.
const char* kTrueBody =
    "        String[] argv = heap String[1];\n"
    "        argv[0] = \"/bin/true\";\n"
    "        Command cmd = heap Command(#argv);\n"
    "        ProcessResult r = cmd.run();\n"
    "        if (r.launched() == false) { return 100; }\n"
    "        return r.code();\n";

const char* kFalseBody =
    "        String[] argv = heap String[1];\n"
    "        argv[0] = \"/bin/false\";\n"
    "        Command cmd = heap Command(#argv);\n"
    "        ProcessResult r = cmd.run();\n"
    "        if (r.launched() == false) { return 100; }\n"
    "        return r.code();\n";

} // namespace

// 1.1.1 — /bin/true exits 0.
TEST(ProcessTests, runTrueExitsZero) {
    EXPECT_EQ(runEntry(wrap(kTrueBody)), 0);
}

// 1.1.2 — /bin/false exits 1, not raised.
TEST(ProcessTests, runFalseExitsNonZero) {
    EXPECT_EQ(runEntry(wrap(kFalseBody)), 1);
}

// 1.1.3 — capture stdout of `echo hello` == "hello\n".
TEST(ProcessTests, captureStdout) {
    std::string body =
        "        String[] argv = heap String[2];\n"
        "        argv[0] = \"/bin/echo\";\n"
        "        argv[1] = \"hello\";\n"
        "        Command cmd = heap Command(#argv);\n"
        "        cmd.captureStdout();\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() == false) { return 100; }\n"
        "        int8[] out = r.stdout();\n"
        "        if (out.count() != 6) { return 1; }\n"
        "        if ((int32) out[0] != 104) { return 2; }\n"
        "        if ((int32) out[5] != 10) { return 3; }\n"
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.4 — feed stdin "ping" to /bin/cat, capture it back.
TEST(ProcessTests, stdinToCapturedStdout) {
    std::string body =
        "        String[] argv = heap String[1];\n"
        "        argv[0] = \"/bin/cat\";\n"
        "        Command cmd = heap Command(#argv);\n"
        "        int8[] in = heap int8[4];\n"
        "        in[0] = (int8) 112;\n"   // p
        "        in[1] = (int8) 105;\n"   // i
        "        in[2] = (int8) 110;\n"   // n
        "        in[3] = (int8) 103;\n"   // g
        "        cmd.stdinBytes(#in, 4);\n"
        "        cmd.captureStdout();\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() == false) { return 100; }\n"
        "        int8[] out = r.stdout();\n"
        "        if (out.count() != 4) { return 1; }\n"
        "        if ((int32) out[0] != 112) { return 2; }\n"
        "        if ((int32) out[3] != 103) { return 3; }\n"
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.5 — working directory affects the child (/bin/pwd prints it).
TEST(ProcessTests, cwdAffectsChild) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
        ("cajeta_proc_cwd_" + std::to_string((long long) ::getpid()));
    fs::create_directories(dir);
    std::string canon = fs::canonical(dir).string();   // matches getcwd()
    std::string len = std::to_string(canon.size() + 1); // + trailing '\n'
    std::string body =
        "        String[] argv = heap String[1];\n"
        "        argv[0] = \"/bin/pwd\";\n"
        "        Command cmd = heap Command(#argv);\n"
        "        cmd.workingDir(\"" + canon + "\");\n"
        "        cmd.captureStdout();\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() == false) { return 100; }\n"
        "        int8[] out = r.stdout();\n"
        "        if (out.count() != " + len + ") { return 1; }\n"
        "        if ((int32) out[0] != 47) { return 2; }\n"          // '/'
        "        if ((int32) out[out.count() - 1] != 10) { return 3; }\n"  // '\n'
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.6 — env override visible to the child.
TEST(ProcessTests, envOverrideVisibleToChild) {
    std::string body =
        "        String[] argv = heap String[3];\n"
        "        argv[0] = \"/bin/sh\";\n"
        "        argv[1] = \"-c\";\n"
        "        argv[2] = \"printf %s \\\"$CAJETA_T\\\"\";\n"
        "        Command cmd = heap Command(#argv);\n"
        "        String[] env = heap String[1];\n"
        "        env[0] = \"CAJETA_T=marker\";\n"
        "        cmd.environment(#env);\n"
        "        cmd.captureStdout();\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() == false) { return 100; }\n"
        "        int8[] out = r.stdout();\n"
        "        if (out.count() != 6) { return 1; }\n"
        "        if ((int32) out[0] != 109) { return 2; }\n"   // 'm'
        "        if ((int32) out[5] != 114) { return 3; }\n"   // 'r'
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.7 — launch failure is a result flag, not a throw (spec §8.2).
TEST(ProcessTests, launchFailureIsFlaggedNotRaised) {
    std::string body =
        "        String[] argv = heap String[1];\n"
        "        argv[0] = \"/no/such/prog_xyz\";\n"
        "        Command cmd = heap Command(#argv);\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() != false) { return 1; }\n"
        "        if (r.code() != -1) { return 2; }\n"
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.8 — large output on BOTH streams, captured without deadlock.
TEST(ProcessTests, largeBothStreamsNoDeadlock) {
    std::string body =
        "        String[] argv = heap String[3];\n"
        "        argv[0] = \"/bin/sh\";\n"
        "        argv[1] = \"-c\";\n"
        "        argv[2] = \"head -c 1048576 /dev/zero; head -c 1048576 /dev/zero 1>&2\";\n"
        "        Command cmd = heap Command(#argv);\n"
        "        cmd.captureStdout();\n"
        "        cmd.captureStderr();\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() == false) { return 100; }\n"
        "        int8[] out = r.stdout();\n"
        "        int8[] err = r.stderr();\n"
        "        if (out.count() != 1048576) { return 1; }\n"
        "        if (err.count() != 1048576) { return 2; }\n"
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.9 — empty argv surfaces as a launch failure result.
TEST(ProcessTests, emptyArgvIsLaunchFailure) {
    std::string body =
        "        String[] argv = heap String[0];\n"
        "        Command cmd = heap Command(#argv);\n"
        "        ProcessResult r = cmd.run();\n"
        "        if (r.launched() != false) { return 1; }\n"
        "        return 0;\n";
    EXPECT_EQ(runEntry(wrap(body)), 0);
}

// 1.1.10 — cajeta code() matches the host runSubprocess on shared cases.
TEST(ProcessTests, hostParityExitCode) {
    using cajeta::buildtool::runSubprocess;
    using cajeta::buildtool::SubprocessOptions;
    SubprocessOptions optT; optT.argv = {"/bin/true"};
    SubprocessOptions optF; optF.argv = {"/bin/false"};
    EXPECT_EQ(runEntry(wrap(kTrueBody)),  runSubprocess(optT).code());
    EXPECT_EQ(runEntry(wrap(kFalseBody)), runSubprocess(optF).code());
}

#endif // !_WIN32
