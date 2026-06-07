//
// Tests for the System.{stdout,stderr,stdin}.{print,println,printf} intrinsics.
// Captures fd-level output via a pipe so the JIT'd program's writes are visible
// to the test assertions.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstdio>
#include <string>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class Sio {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "        return 0;\n"
           "    }\n"
           "}\n";
}

// Run a no-arg int32-returning JIT function while capturing the requested fd
// (1=stdout or 2=stderr). Returns the captured bytes as a std::string.
//
// Redirects the descriptor to a temp file via dup2(), runs, restores, then
// reads the file back. The runtime writes via write(fd, ...) directly, which
// a file-backed fd captures faithfully. This avoids pipe()/fcntl()/O_NONBLOCK
// (absent on MinGW) and can't deadlock on a full pipe buffer the way a
// pipe-based capture could for large output. tmpfile() opens in binary mode
// on Windows, so no CRLF translation skews the captured bytes.
std::string captureFd(int fd, int32_t (*fn)()) {
    std::FILE* tmp = std::tmpfile();
    if (!tmp) return "<tmpfile failed>";
    int tmpFd = fileno(tmp);
    std::FILE* cfile = (fd == 2) ? stderr : stdout;
    std::fflush(cfile);
    int origFd = dup(fd);
    if (origFd == -1) { std::fclose(tmp); return "<dup failed>"; }
    dup2(tmpFd, fd);
    fn();
    std::fflush(cfile);
    dup2(origFd, fd);
    close(origFd);

    std::fflush(tmp);
    std::fseek(tmp, 0, SEEK_SET);
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

} // namespace

// --- print / println --------------------------------------------------------

TEST(SystemIoTests, printlnAppendsNewline) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"hello\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "hello\n");
}

TEST(SystemIoTests, printNoNewline) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.print(\"hi\");\n"
        "System.stdout.print(\"there\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "hithere");
}

TEST(SystemIoTests, stderrRoutesToFd2) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stderr.println(\"oops\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string outStdout = captureFd(1, fn);
    EXPECT_EQ(outStdout, "") << "stderr message leaked to stdout";

    // Re-JIT and capture fd 2 to confirm the message reached stderr.
    auto jit2 = CajetaJit::compile(makeSource(
        "System.stderr.println(\"oops\");"), "test.Sio");
    auto fn2 = jit2->lookup<int32_t (*)()>("run");
    std::string outStderr = captureFd(2, fn2);
    EXPECT_EQ(outStderr, "oops\n");
}

TEST(SystemIoTests, stderrorAliasSameAsStderr) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stderror.println(\"via alias\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(2, fn);
    EXPECT_EQ(out, "via alias\n");
}

TEST(SystemIoTests, multipleLinesOrdered) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"one\");\n"
        "System.stdout.println(\"two\");\n"
        "System.stdout.println(\"three\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "one\ntwo\nthree\n");
}

TEST(SystemIoTests, printlnFromLocalVariable) {
    auto jit = CajetaJit::compile(makeSource(
        "String s = \"local var\";\n"
        "System.stdout.println(s);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "local var\n");
}

// --- String + concatenation -------------------------------------------------

TEST(SystemIoTests, concatStringPlusString) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"hello \" + \"world\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "hello world\n");
}

TEST(SystemIoTests, concatStringPlusInt) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"x = \" + 42);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "x = 42\n");
}

TEST(SystemIoTests, concatIntPlusString) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(7 + \" is seven\");"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "7 is seven\n");
}

TEST(SystemIoTests, concatStringPlusBool) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"flag=\" + true);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "flag=true\n");
}

TEST(SystemIoTests, concatStringPlusFloat) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"pi=\" + 3.14);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "pi=3.14\n");
}

TEST(SystemIoTests, concatChained) {
    // Left-to-right associativity: ((("a=" + 1) + " b=") + 2)
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"a=\" + 1 + \" b=\" + 2);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "a=1 b=2\n");
}

TEST(SystemIoTests, concatFromLocal) {
    auto jit = CajetaJit::compile(makeSource(
        "String name = \"world\";\n"
        "int32 n = 5;\n"
        "System.stdout.println(\"hi \" + name + \" #\" + n);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "hi world #5\n");
}

// --- printf with SLF4J {} templating ----------------------------------------

TEST(SystemIoTests, printfWithNoPlaceholders) {
    auto jit = CajetaJit::compile(makeSource(
        "String[] args = heap String[0];\n"
        "System.stdout.printf(\"plain text\", args);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "plain text");
}

TEST(SystemIoTests, printfWithOnePlaceholder) {
    auto jit = CajetaJit::compile(makeSource(
        "String[] args = heap String[1];\n"
        "args[0] = \"world\";\n"
        "System.stdout.printf(\"hello {}\", args);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "hello world");
}

TEST(SystemIoTests, printfWithMultiplePlaceholders) {
    auto jit = CajetaJit::compile(makeSource(
        "String[] args = heap String[3];\n"
        "args[0] = \"alice\";\n"
        "args[1] = \"42\";\n"
        "args[2] = \"login\";\n"
        "System.stdout.printf(\"user={} id={} action={}\", args);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "user=alice id=42 action=login");
}

TEST(SystemIoTests, printfMissingArgsRenderNull) {
    // Two placeholders, only one arg → second one prints "null".
    auto jit = CajetaJit::compile(makeSource(
        "String[] args = heap String[1];\n"
        "args[0] = \"first\";\n"
        "System.stdout.printf(\"{} and {}\", args);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "first and null");
}

TEST(SystemIoTests, printfExtraArgsIgnored) {
    auto jit = CajetaJit::compile(makeSource(
        "String[] args = heap String[3];\n"
        "args[0] = \"used\";\n"
        "args[1] = \"unused1\";\n"
        "args[2] = \"unused2\";\n"
        "System.stdout.printf(\"only {} here\", args);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "only used here");
}

// --- println/print overloads for primitives ---------------------------------

TEST(SystemIoTests, printlnInt32Literal) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(42);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "42\n");
}

TEST(SystemIoTests, printlnInt64Negative) {
    auto jit = CajetaJit::compile(makeSource(
        "int64 v = -123456789;\n"
        "System.stdout.println(v);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "-123456789\n");
}

TEST(SystemIoTests, printlnBooleanTrue) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(true);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "true\n");
}

TEST(SystemIoTests, printlnBooleanFalse) {
    auto jit = CajetaJit::compile(makeSource(
        "boolean b = false;\n"
        "System.stdout.println(b);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "false\n");
}

TEST(SystemIoTests, printlnFloat64Literal) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(3.14);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "3.14\n");
}

TEST(SystemIoTests, printInt32NoNewline) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.print(7);\n"
        "System.stdout.print(8);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "78");
}

TEST(SystemIoTests, printfToStderr) {
    auto jit = CajetaJit::compile(makeSource(
        "String[] args = heap String[1];\n"
        "args[0] = \"boom\";\n"
        "System.stderr.printf(\"error: {}\", args);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(2, fn);
    EXPECT_EQ(out, "error: boom");
}
