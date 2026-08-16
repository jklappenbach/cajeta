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







// --- String + concatenation -------------------------------------------------





TEST(SystemIoTests, concatStringPlusFloat) {
    auto jit = CajetaJit::compile(makeSource(
        "System.stdout.println(\"pi=\" + 3.14);"), "test.Sio");
    auto fn = jit->lookup<int32_t (*)()>("run");
    std::string out = captureFd(1, fn);
    EXPECT_EQ(out, "pi=3.14\n");
}



// --- printf with SLF4J {} templating ----------------------------------------






// --- println/print overloads for primitives ---------------------------------





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
