//
// pollable-stdin 1.1.5 — a closed peer is READY, not an error.
//
// This is the distinction a serve loop lives or dies on. At end of input
// the fd must report READY and the following read must return 0; the loop
// then breaks. If EOF were reported as NOT-ready the loop would hang
// forever on a dead pipe, and if it were reported as an ERROR the loop
// would abort a session that merely ended normally.
//
// The plan recorded 1.1.5 as BLOCKED for want of "a pipe reachable from
// cajeta source". That overstated it — the same way cabra 4.2.1's own
// blocker overstated "no non-blocking stdin". `FileReader` takes an int
// fd, and an fd is just an integer: C++ makes the pipe and the fd number
// is embedded into the generated source as a literal, exactly as the
// regular-file tests embed a path. The JIT runs in this process, so the
// descriptor is live. No new intrinsic needed.
//
// Pins (plan ids in brackets):
//   [1.1.5] peer closed, no data  -> READY, and the read returns 0
//   [1.1.5] peer closed, data     -> READY, data first, THEN a 0 read
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <cstring>
#include <unistd.h>

using cajeta_test::CajetaJit;

namespace {

// Returns the READ end of a pipe whose write end is already closed,
// optionally after writing `payload`. That is a peer that has hung up.
int makeClosedPeerPipe(const char* payload) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    if (payload && *payload) {
        ssize_t n = ::write(fds[1], payload, ::strlen(payload));
        (void) n;
    }
    ::close(fds[1]);                 // the peer hangs up
    return fds[0];
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

} // namespace

#if defined(__linux__)

// [1.1.5] Nothing was ever written and the peer is gone. The wait must say
// READY (so the caller reaches its read) and the read must say 0 (so the
// caller learns it is EOF). Reporting not-ready here is the hang.
TEST(FileReaderAwaitEofTests, closedPeerWithNoDataIsReadyAndReadsZero) {
    int fd = makeClosedPeerPipe(nullptr);
    ASSERT_GE(fd, 0) << "pipe() failed";

    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FileReader r = heap FileReader(" + std::to_string(fd) + ");\n"
        "        boolean threw = false;\n"
        "        boolean ready = false;\n"
        "        int32 n = 0 - 1;\n"
        "        int8[] buf = heap int8[8];\n"
        "        try {\n"
        "            ready = r.awaitReadable(1000);\n"
        "            n = r.read(buf, 8);\n"
        "        } catch (Throwable t) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (threw) { return -2; }\n"
        "        if (!ready) { return -3; }\n"
        "        return n;\n"
        "    }\n"
        "}\n";
    int32_t rc = runI32(src);
    ::close(fd);

    ASSERT_NE(-2, rc) << "EOF was reported as an ERROR — a serve loop would "
                         "abort a session that merely ended";
    ASSERT_NE(-3, rc) << "EOF was reported as NOT READY — a serve loop would "
                         "hang forever on a dead pipe";
    EXPECT_EQ(0, rc) << "expected a zero-byte read at EOF, got " << rc;
}

// [1.1.5] Data first, then EOF. Proves readiness is not simply pinned true
// for a closed pipe: the payload arrives on the first read, and only the
// SECOND read reports 0 — while both waits say ready.
TEST(FileReaderAwaitEofTests, closedPeerDeliversDataThenEof) {
    int fd = makeClosedPeerPipe("hi\n");
    ASSERT_GE(fd, 0) << "pipe() failed";

    auto src =
        "package test;\n"
        "import cajeta.io.file.FileReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FileReader r = heap FileReader(" + std::to_string(fd) + ");\n"
        "        int8[] buf = heap int8[8];\n"
        "        boolean r1 = r.awaitReadable(1000);\n"
        "        int32 n1 = r.read(buf, 8);\n"
        "        boolean r2 = r.awaitReadable(1000);\n"
        "        int32 n2 = r.read(buf, 8);\n"
        "        if (!r1) { return -3; }\n"
        "        if (!r2) { return -4; }\n"
        "        return n1 * 10 + n2;\n"
        "    }\n"
        "}\n";
    int32_t rc = runI32(src);
    ::close(fd);

    ASSERT_NE(-3, rc) << "the wait missed buffered data";
    ASSERT_NE(-4, rc) << "the wait reported NOT READY at EOF";
    EXPECT_EQ(30, rc)
        << "expected 3 bytes then a 0 read (encoded 30), got " << rc;
}

#endif // __linux__
